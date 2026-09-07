#include "effects/ClapModule.h"

#include "diagnostics/Logging.h"
#include "effects/ClapEffect.h"

#include <clap/clap.h>

#include <QCoreApplication>
#include <QFileInfo>

#include <dlfcn.h>

namespace OpenChat {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("ClapModule", text);
}

QString textOf(const char *text)
{
    return text ? QString::fromUtf8(text) : QString();
}

} // namespace

ClapModule::~ClapModule()
{
    if (m_entry)
        m_entry->deinit();
    if (m_handle) {
        // dlclose is a request, not a guarantee: glibc keeps an object mapped
        // when anything still references it, and some plugins deliberately make
        // themselves unloadable. Calling it is still right -- refusing to would
        // leak every module for the life of the process -- but nothing here may
        // assume the code is gone afterwards.
        ::dlclose(m_handle);
    }
}

std::shared_ptr<ClapModule> ClapModule::open(const QString &path, PluginError &error)
{
    const QByteArray localPath = path.toLocal8Bit();

    // RTLD_NOW resolves every symbol up front, so a plugin missing a dependency
    // fails here rather than in the middle of a call. RTLD_LOCAL keeps its
    // symbols out of the global namespace, which matters more than it looks:
    // with RTLD_GLOBAL a plugin exporting, say, its own `malloc` or a symbol
    // name that collides with OpenSSL's would interpose on the whole process,
    // and crypto code calling a plugin's implementation of something is not a
    // situation to leave available.
    void *handle = ::dlopen(localPath.constData(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const QString reason = textOf(::dlerror());
        qCWarning(effectsLog, "dlopen failed for %s: %s", qUtf8Printable(path),
                  qUtf8Printable(reason));
        // A missing dependency is the single most common reason a real plugin
        // will not load on a machine that does not also have a DAW installed,
        // and it is worth telling apart from "this is not a plugin" because the
        // user can act on it.
        const bool missingDependency = reason.contains(QLatin1String("cannot open shared object"))
            || reason.contains(QLatin1String("undefined symbol"));
        error = makePluginError(
            missingDependency ? PluginErrorCode::DependencyMissing : PluginErrorCode::BadBundle,
            missingDependency
                ? tr("This plugin needs another program's files that are not installed.")
                : tr("This file could not be loaded as a plugin."));
        return nullptr;
    }

    auto module = std::make_shared<ClapModule>(PrivateTag{});
    module->m_handle = handle;
    module->m_path = path;

    // clap_entry is a DATA symbol: dlsym yields a pointer to the struct itself.
    // Casting it to a function and calling it is the classic first-CLAP-host
    // crash.
    auto *entry = static_cast<const clap_plugin_entry_t *>(::dlsym(handle, "clap_entry"));
    if (!entry || !entry->init || !entry->get_factory) {
        error = makePluginError(PluginErrorCode::BadBundle,
                                tr("This file is not a CLAP plugin."));
        return nullptr;
    }
    if (!clap_version_is_compatible(entry->clap_version)) {
        error = makePluginError(PluginErrorCode::BadBundle,
                                tr("This plugin was built for a newer plugin standard."));
        return nullptr;
    }

    // init() takes the plugin's own absolute path. Plugins use it to find their
    // presets and resources beside themselves, so passing anything else leaves
    // them looking in the wrong place.
    if (!entry->init(localPath.constData())) {
        error = makePluginError(PluginErrorCode::BadBundle,
                                tr("The plugin refused to initialise."));
        return nullptr;
    }
    // Recorded only now, so the destructor calls deinit() exactly when init()
    // succeeded.
    module->m_entry = entry;

    module->m_factory = static_cast<const clap_plugin_factory_t *>(
        entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    if (!module->m_factory || !module->m_factory->get_plugin_count
        || !module->m_factory->create_plugin) {
        error = makePluginError(PluginErrorCode::BadBundle,
                                tr("This plugin publishes no effects."));
        return nullptr;
    }

    module->m_host = std::make_unique<ClapHostContext>();
    module->buildDescriptors();
    if (module->m_descriptors.isEmpty()) {
        error = makePluginError(PluginErrorCode::EntryNotFound,
                                tr("This plugin publishes no effects."));
        return nullptr;
    }

    error = {};
    return module;
}

void ClapModule::buildDescriptors()
{
    const uint32_t count = m_factory->get_plugin_count(m_factory);
    m_descriptors.reserve(static_cast<qsizetype>(count));

    for (uint32_t index = 0; index < count; ++index) {
        const clap_plugin_descriptor_t *info =
            m_factory->get_plugin_descriptor(m_factory, index);
        if (!info || !info->id)
            continue;

        AudioPluginDescriptor descriptor;
        descriptor.id.format = AudioPluginFormat::Clap;
        descriptor.id.bundlePath = m_path;
        descriptor.id.entryId = textOf(info->id);
        descriptor.name = textOf(info->name);
        descriptor.vendor = textOf(info->vendor);
        descriptor.version = textOf(info->version);
        descriptor.description = textOf(info->description);
        for (const char *const *feature = info->features; feature && *feature; ++feature)
            descriptor.features.append(textOf(*feature));

        // Ports, latency and parameters are only knowable from a live instance,
        // so each plugin is created, interrogated and destroyed once here.
        // Doing it now rather than lazily means a scan is a scan: by the time
        // anything above asks a question, no more plugin code has to run.
        const clap_plugin_t *plugin =
            m_factory->create_plugin(m_factory, m_host->handle(), info->id);
        if (plugin && plugin->init(plugin)) {
            if (const auto *ports = static_cast<const clap_plugin_audio_ports_t *>(
                    plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS))) {
                descriptor.inputPortCount = static_cast<int>(ports->count(plugin, true));
                descriptor.outputPortCount = static_cast<int>(ports->count(plugin, false));
                auto mainChannels = [&](bool isInput) {
                    const int portCount =
                        isInput ? descriptor.inputPortCount : descriptor.outputPortCount;
                    int channels = 0;
                    for (int port = 0; port < portCount; ++port) {
                        clap_audio_port_info_t portInfo{};
                        if (!ports->get(plugin, static_cast<uint32_t>(port), isInput, &portInfo))
                            continue;
                        if (port == 0 || (portInfo.flags & CLAP_AUDIO_PORT_IS_MAIN))
                            channels = static_cast<int>(portInfo.channel_count);
                        if (portInfo.flags & CLAP_AUDIO_PORT_IS_MAIN)
                            break;
                    }
                    return channels;
                };
                descriptor.mainInputChannels = mainChannels(true);
                descriptor.mainOutputChannels = mainChannels(false);
            } else {
                // No extension means the specification's default: stereo.
                descriptor.inputPortCount = 1;
                descriptor.outputPortCount = 1;
                descriptor.mainInputChannels = 2;
                descriptor.mainOutputChannels = 2;
            }

            if (const auto *params = static_cast<const clap_plugin_params_t *>(
                    plugin->get_extension(plugin, CLAP_EXT_PARAMS))) {
                const uint32_t parameterCount = params->count(plugin);
                for (uint32_t i = 0; i < parameterCount; ++i) {
                    clap_param_info_t parameterInfo{};
                    if (!params->get_info(plugin, i, &parameterInfo))
                        continue;
                    AudioPluginParameter parameter;
                    parameter.id = parameterInfo.id;
                    parameter.name = QString::fromUtf8(parameterInfo.name);
                    parameter.module = QString::fromUtf8(parameterInfo.module);
                    parameter.minValue = parameterInfo.min_value;
                    parameter.maxValue = parameterInfo.max_value;
                    parameter.defaultValue = parameterInfo.default_value;
                    parameter.stepped = (parameterInfo.flags & CLAP_PARAM_IS_STEPPED) != 0;
                    parameter.readOnly = (parameterInfo.flags & CLAP_PARAM_IS_READONLY) != 0;
                    parameter.hidden = (parameterInfo.flags & CLAP_PARAM_IS_HIDDEN) != 0;
                    descriptor.parameters.append(parameter);
                }
            }

            if (const auto *latency = static_cast<const clap_plugin_latency_t *>(
                    plugin->get_extension(plugin, CLAP_EXT_LATENCY))) {
                // Reported before activation this is only indicative; the real
                // figure is read again once the plugin is activated at the
                // call's rate and block size.
                descriptor.latencySamples = static_cast<int>(latency->get(plugin));
            }
        }
        if (plugin)
            plugin->destroy(plugin);

        m_descriptors.append(descriptor);
    }
}

QList<AudioPluginDescriptor> ClapModule::descriptors() const
{
    return m_descriptors;
}

std::unique_ptr<AudioPluginInstance> ClapModule::createInstance(const QString &entryId,
                                                                PluginError &error)
{
    const AudioPluginDescriptor *match = nullptr;
    for (const AudioPluginDescriptor &descriptor : m_descriptors) {
        if (descriptor.id.entryId == entryId) {
            match = &descriptor;
            break;
        }
    }
    if (!match) {
        error = makePluginError(PluginErrorCode::EntryNotFound,
                                tr("This plugin is no longer in that file."));
        return nullptr;
    }

    const QByteArray localId = entryId.toUtf8();
    const clap_plugin_t *plugin =
        m_factory->create_plugin(m_factory, m_host->handle(), localId.constData());
    if (!plugin) {
        error = makePluginError(PluginErrorCode::EntryNotFound,
                                tr("The plugin could not be created."));
        return nullptr;
    }

    // The instance takes a reference to this module, so the code the plugin
    // lives in cannot be unloaded while the plugin is still running in it.
    return ClapEffect::create(shared_from_this(), plugin, *match, error);
}

} // namespace OpenChat
