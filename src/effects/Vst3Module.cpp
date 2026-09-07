#include "effects/Vst3Module.h"

#include "diagnostics/Logging.h"
#include "effects/Vst3Effect.h"

#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstcomponent.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#include <dlfcn.h>
#include <sys/utsname.h>

using namespace Steinberg;

namespace OpenChat {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("Vst3Module", text);
}

// Class UIDs are 16 raw bytes. Rendered as uppercase hex so an id is copyable,
// comparable and free of anything a path or a JSON string would have to escape.
QString uidToHex(const TUID uid)
{
    return QString::fromLatin1(QByteArray(uid, 16).toHex().toUpper());
}

bool hexToUid(const QString &hex, TUID out)
{
    if (hex.size() != 32)
        return false;
    const QByteArray bytes = QByteArray::fromHex(hex.toLatin1());
    if (bytes.size() != 16)
        return false;
    std::memcpy(out, bytes.constData(), 16);
    return true;
}

} // namespace

QString Vst3Module::resolveBinaryPath(const QString &bundlePath)
{
    const QFileInfo info(bundlePath);
    // A bare shared object rather than a bundle. Uncommon but real.
    if (info.isFile())
        return info.absoluteFilePath();
    if (!info.isDir())
        return {};

    // The architecture directory is uname().machine + "-linux". Computed rather
    // than hardcoded so an aarch64 or riscv64 build finds its own subdirectory
    // in a bundle that also carries x86_64.
    struct utsname system {};
    if (::uname(&system) != 0)
        return {};
    const QString architecture = QString::fromLatin1(system.machine) + QLatin1String("-linux");

    const QDir contents(info.absoluteFilePath() + QLatin1String("/Contents/") + architecture);
    if (!contents.exists())
        return {};
    // The specification names the object after the bundle, but not every plugin
    // obeys, so the name is tried first and any single .so accepted after it.
    const QString expected = contents.filePath(info.completeBaseName() + QLatin1String(".so"));
    if (QFileInfo::exists(expected))
        return expected;
    const QStringList objects = contents.entryList({QStringLiteral("*.so")}, QDir::Files);
    if (objects.size() == 1)
        return contents.filePath(objects.first());
    return {};
}

Vst3Module::~Vst3Module()
{
    if (m_factory) {
        m_factory->release();
        m_factory = nullptr;
    }
    // ModuleExit after the factory is gone, before the library is closed. Doing
    // it in any other order tears down the plugin's globals while something
    // still points at them.
    if (m_moduleExit)
        m_moduleExit();
    if (m_handle)
        ::dlclose(m_handle);
}

std::shared_ptr<Vst3Module> Vst3Module::open(const QString &path, PluginError &error)
{
    const QString binaryPath = resolveBinaryPath(path);
    if (binaryPath.isEmpty()) {
        error = makePluginError(
            PluginErrorCode::BadBundle,
            tr("This plugin does not contain a version for this kind of computer."));
        return nullptr;
    }

    // RTLD_NOW | RTLD_LOCAL for the same reasons as CLAP: fail now rather than
    // mid-call, and keep the plugin's symbols from interposing on the process.
    void *handle = ::dlopen(binaryPath.toLocal8Bit().constData(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const QString reason = QString::fromUtf8(::dlerror());
        qCWarning(effectsLog, "dlopen failed for %s: %s", qUtf8Printable(binaryPath),
                  qUtf8Printable(reason));
        const bool missingDependency = reason.contains(QLatin1String("cannot open shared object"))
            || reason.contains(QLatin1String("undefined symbol"));
        error = makePluginError(
            missingDependency ? PluginErrorCode::DependencyMissing : PluginErrorCode::BadBundle,
            missingDependency
                ? tr("This plugin needs another program's files that are not installed.")
                : tr("This file could not be loaded as a plugin."));
        return nullptr;
    }

    auto module = std::make_shared<Vst3Module>(PrivateTag{});
    module->m_handle = handle;
    module->m_bundlePath = QFileInfo(path).absoluteFilePath();
    module->m_binaryPath = binaryPath;

    using ModuleEntryFn = bool (*)(void *);
    using ModuleExitFn = bool (*)();
    using GetFactoryFn = IPluginFactory *(*)();

    auto moduleEntry = reinterpret_cast<ModuleEntryFn>(::dlsym(handle, "ModuleEntry"));
    auto getFactory = reinterpret_cast<GetFactoryFn>(::dlsym(handle, "GetPluginFactory"));
    module->m_moduleExit = reinterpret_cast<ModuleExitFn>(::dlsym(handle, "ModuleExit"));

    if (!getFactory) {
        error = makePluginError(PluginErrorCode::BadBundle,
                                tr("This file is not a VST 3 plugin."));
        return nullptr;
    }
    // ModuleEntry before anything else. Its argument is the dlopen handle.
    if (moduleEntry && !moduleEntry(handle)) {
        error = makePluginError(PluginErrorCode::BadBundle,
                                tr("The plugin refused to initialise."));
        return nullptr;
    }

    module->m_factory = getFactory();
    if (!module->m_factory) {
        error = makePluginError(PluginErrorCode::BadBundle,
                                tr("This plugin publishes no effects."));
        return nullptr;
    }

    module->buildDescriptors();
    if (module->m_descriptors.isEmpty()) {
        error = makePluginError(PluginErrorCode::EntryNotFound,
                                tr("This plugin publishes no audio effects."));
        return nullptr;
    }

    error = {};
    return module;
}

void Vst3Module::buildDescriptors()
{
    // PClassInfo2 carries the vendor, version and subcategories that PClassInfo
    // does not. Older plugins publish only the smaller struct, so the richer
    // factory interface is asked for and gracefully declined.
    IPluginFactory2 *factory2 = nullptr;
    m_factory->queryInterface(IPluginFactory2::iid, reinterpret_cast<void **>(&factory2));

    const int32 classCount = m_factory->countClasses();
    for (int32 index = 0; index < classCount; ++index) {
        PClassInfo info{};
        if (m_factory->getClassInfo(index, &info) != kResultOk)
            continue;
        // Only audio effects. A factory also publishes edit-controller classes,
        // and instantiating one of those as an effect is a null processor.
        if (std::strcmp(info.category, kVstAudioEffectClass) != 0)
            continue;

        AudioPluginDescriptor descriptor;
        descriptor.id.format = AudioPluginFormat::Vst3;
        descriptor.id.bundlePath = m_bundlePath;
        descriptor.id.entryId = uidToHex(info.cid);
        descriptor.name = QString::fromUtf8(info.name);

        if (factory2) {
            PClassInfo2 info2{};
            if (factory2->getClassInfo2(index, &info2) == kResultOk) {
                descriptor.vendor = QString::fromUtf8(info2.vendor);
                descriptor.version = QString::fromUtf8(info2.version);
                const QString categories = QString::fromUtf8(info2.subCategories);
                if (!categories.isEmpty())
                    descriptor.features = categories.split(QLatin1Char('|'), Qt::SkipEmptyParts);
            }
        }

        m_descriptors.append(descriptor);
    }

    if (factory2)
        factory2->release();

    // Ports, latency and parameters need a live instance. Unlike CLAP, where
    // one is created per plugin anyway, VST3 instantiation is heavy enough that
    // it happens once here and the answers are cached on the descriptor -- the
    // scan pays for it, not the first question asked afterwards.
    for (AudioPluginDescriptor &descriptor : m_descriptors) {
        PluginError probeError;
        if (auto probe = Vst3Effect::create(shared_from_this(), descriptor, probeError))
            descriptor = probe->descriptor();
        else
            qCDebug(effectsLog, "vst3 probe failed for %s: %s",
                    qUtf8Printable(descriptor.name), qUtf8Printable(probeError.message));
    }
}

QList<AudioPluginDescriptor> Vst3Module::descriptors() const
{
    return m_descriptors;
}

std::unique_ptr<AudioPluginInstance> Vst3Module::createInstance(const QString &entryId,
                                                                PluginError &error)
{
    TUID uid{};
    if (!hexToUid(entryId, uid)) {
        error = makePluginError(PluginErrorCode::EntryNotFound,
                                tr("This plugin is no longer in that file."));
        return nullptr;
    }
    for (const AudioPluginDescriptor &descriptor : m_descriptors) {
        if (descriptor.id.entryId == entryId)
            return Vst3Effect::create(shared_from_this(), descriptor, error);
    }
    error = makePluginError(PluginErrorCode::EntryNotFound,
                            tr("This plugin is no longer in that file."));
    return nullptr;
}

} // namespace OpenChat
