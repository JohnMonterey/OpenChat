#include "effects/ClapEffect.h"

#include "effects/ClapModule.h"
#include "effects/DenormalGuard.h"

#include <QCoreApplication>

#include <algorithm>
#include <cstring>

namespace OpenChat {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("ClapEffect", text);
}

// A plugin declaring no audio-ports extension at all defaults to STEREO in and
// out. Absence of a declaration is not permission to assume the convenient
// thing; the specification's default is two channels, and a host that guessed
// mono would hand such a plugin half the buffers it expects.
constexpr int defaultChannels = 2;
constexpr int maxPorts = 8;

} // namespace

// --- ClapEventQueue -------------------------------------------------------

ClapEventQueue::ClapEventQueue()
{
    m_events.reserve(capacity);
    m_input.ctx = this;
    m_input.size = &ClapEventQueue::inputSize;
    m_input.get = &ClapEventQueue::inputGet;
    m_output.ctx = this;
    m_output.try_push = &ClapEventQueue::outputTryPush;
}

void ClapEventQueue::pushParameterValue(clap_id parameterId, void *cookie, double value)
{
    if (static_cast<int>(m_events.size()) >= capacity)
        return;

    clap_event_param_value_t event{};
    // size is the size of the WHOLE event, not of the header. A plugin that
    // trusts this field and gets sizeof(header) reads a truncated struct.
    event.header.size = sizeof(clap_event_param_value_t);
    event.header.time = 0; // At the head of the block.
    event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    event.header.type = CLAP_EVENT_PARAM_VALUE;
    event.header.flags = 0;
    event.param_id = parameterId;
    // Copied verbatim from clap_param_info. It is the plugin's own handle for
    // the parameter and passing anything else -- including null -- is undefined
    // for plugins that use it as a pointer.
    event.cookie = cookie;
    // -1 is the documented wildcard: this is not addressed to a particular
    // note, port, channel or key.
    event.note_id = -1;
    event.port_index = -1;
    event.channel = -1;
    event.key = -1;
    event.value = value;
    m_events.push_back(event);
}

uint32_t ClapEventQueue::inputSize(const clap_input_events_t *list)
{
    const auto *self = static_cast<const ClapEventQueue *>(list->ctx);
    return static_cast<uint32_t>(self->m_events.size());
}

const clap_event_header_t *ClapEventQueue::inputGet(const clap_input_events_t *list,
                                                    uint32_t index)
{
    const auto *self = static_cast<const ClapEventQueue *>(list->ctx);
    if (index >= self->m_events.size())
        return nullptr;
    return &self->m_events[index].header;
}

bool ClapEventQueue::outputTryPush(const clap_output_events_t *, const clap_event_header_t *)
{
    // Output events are what a plugin sends back: parameter gestures from its
    // own GUI, note output, latency notices. There is no GUI here and nothing
    // upstream to route notes to, so they are accepted and dropped. Returning
    // true rather than false is deliberate: false means "out of memory" and
    // some plugins treat it as a reason to stop, which would turn a message we
    // do not need into a dead effect.
    return true;
}

// --- ClapEffect -----------------------------------------------------------

ClapEffect::~ClapEffect()
{
    deactivate();
    if (m_plugin)
        m_plugin->destroy(m_plugin);
}

std::unique_ptr<ClapEffect> ClapEffect::create(std::shared_ptr<ClapModule> module,
                                               const clap_plugin_t *plugin,
                                               AudioPluginDescriptor descriptor,
                                               PluginError &error)
{
    if (!plugin) {
        error = makePluginError(PluginErrorCode::EntryNotFound,
                                tr("The plugin could not be created."));
        return nullptr;
    }

    // Constructed before init() so that the destructor owns the plugin from
    // here on: every failure below returns null and destroys it exactly once.
    std::unique_ptr<ClapEffect> effect(new ClapEffect);
    effect->m_module = std::move(module);
    effect->m_plugin = plugin;
    effect->m_descriptor = std::move(descriptor);

    if (!plugin->init(plugin)) {
        error = makePluginError(PluginErrorCode::ActivationFailed,
                                tr("The plugin failed to start."));
        return nullptr;
    }

    // Extensions are only legal to query after init().
    effect->m_ports = static_cast<const clap_plugin_audio_ports_t *>(
        plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS));
    effect->m_params = static_cast<const clap_plugin_params_t *>(
        plugin->get_extension(plugin, CLAP_EXT_PARAMS));
    effect->m_state = static_cast<const clap_plugin_state_t *>(
        plugin->get_extension(plugin, CLAP_EXT_STATE));
    effect->m_latency = static_cast<const clap_plugin_latency_t *>(
        plugin->get_extension(plugin, CLAP_EXT_LATENCY));

    if (const PluginError portError = effect->configurePorts()) {
        error = portError;
        return nullptr;
    }
    if (const PluginError activateError = effect->activate()) {
        error = activateError;
        return nullptr;
    }

    // Cache the parameter cookies now: setParameter needs them per event, and
    // get_info is a [main-thread] call that must not happen inside a frame.
    if (effect->m_params) {
        const uint32_t count = effect->m_params->count(plugin);
        for (uint32_t i = 0; i < count; ++i) {
            clap_param_info_t info{};
            if (effect->m_params->get_info(plugin, i, &info))
                effect->m_parameterCookies.insert(info.id, info.cookie);
        }
    }

    error = {};
    return effect;
}

PluginError ClapEffect::configurePorts()
{
    int inputPorts = 1;
    int outputPorts = 1;
    std::vector<int> inputChannels{defaultChannels};
    std::vector<int> outputChannels{defaultChannels};
    m_mainIn = 0;
    m_mainOut = 0;

    if (m_ports) {
        inputPorts = static_cast<int>(m_ports->count(m_plugin, true));
        outputPorts = static_cast<int>(m_ports->count(m_plugin, false));
        if (inputPorts < 1 || outputPorts < 1 || inputPorts > maxPorts
            || outputPorts > maxPorts) {
            return makePluginError(PluginErrorCode::ChannelLayoutUnsupported,
                                   tr("The plugin's audio layout is not one a call can use."));
        }
        inputChannels.assign(inputPorts, 0);
        outputChannels.assign(outputPorts, 0);

        for (int direction = 0; direction < 2; ++direction) {
            const bool isInput = direction == 0;
            const int portCount = isInput ? inputPorts : outputPorts;
            for (int port = 0; port < portCount; ++port) {
                // Zeroed before every get(). Real plugins leave fields they do
                // not use untouched -- port_type is null on every plugin
                // measured -- so anything read back from an uninitialised
                // struct would be whatever was on the stack. We never read
                // port_type, and the zeroing is what makes that safe rather
                // than lucky.
                clap_audio_port_info_t info{};
                if (!m_ports->get(m_plugin, static_cast<uint32_t>(port), isInput, &info)) {
                    return makePluginError(
                        PluginErrorCode::ChannelLayoutUnsupported,
                        tr("The plugin did not describe its audio ports."));
                }
                const int channels = static_cast<int>(info.channel_count);
                (isInput ? inputChannels : outputChannels)[port] = channels;
                // The BIT, not the word.
                if (info.flags & CLAP_AUDIO_PORT_IS_MAIN)
                    (isInput ? m_mainIn : m_mainOut) = port;
            }
        }
    }

    const int mainInChannels = inputChannels[m_mainIn];
    const int mainOutChannels = outputChannels[m_mainOut];
    // Above two channels the duplicate-and-take-channel-0 compromise stops
    // being defensible -- there is no honest way to fold a 5.1 bus into a voice
    // call -- and zero channels is not something to feed at all.
    if (mainInChannels < 1 || mainInChannels > 2 || mainOutChannels < 1
        || mainOutChannels > 2) {
        return makePluginError(
            PluginErrorCode::ChannelLayoutUnsupported,
            tr("This plugin needs %1 in and %2 out channels; a call is mono.")
                .arg(mainInChannels)
                .arg(mainOutChannels));
    }

    auto buildBuffers = [](const std::vector<int> &channelCounts,
                           std::vector<std::vector<float>> &storage,
                           std::vector<std::vector<float *>> &pointers,
                           std::vector<clap_audio_buffer_t> &buffers) {
        const size_t portCount = channelCounts.size();
        storage.assign(portCount, {});
        pointers.assign(portCount, {});
        buffers.assign(portCount, clap_audio_buffer_t{});
        for (size_t port = 0; port < portCount; ++port) {
            const size_t channels = static_cast<size_t>(std::max(0, channelCounts[port]));
            storage[port].assign(channels * CallAudioFormat::samplesPerFrame, 0.0f);
            pointers[port].resize(channels);
            for (size_t channel = 0; channel < channels; ++channel) {
                pointers[port][channel] =
                    storage[port].data() + channel * CallAudioFormat::samplesPerFrame;
            }
            buffers[port].data32 = pointers[port].data();
            buffers[port].data64 = nullptr; // We only ever offer 32-bit.
            buffers[port].channel_count = static_cast<uint32_t>(channels);
            buffers[port].latency = 0;
            buffers[port].constant_mask = 0;
        }
    };

    // Every declared port gets real, zeroed storage, sidechains included. A
    // sidechain the host never writes stays silent, which is what a plugin
    // expects when nothing is patched into it -- but it must still be there to
    // dereference.
    buildBuffers(inputChannels, m_inStorage, m_inPtrs, m_inBuffers);
    buildBuffers(outputChannels, m_outStorage, m_outPtrs, m_outBuffers);

    m_descriptor.inputPortCount = inputPorts;
    m_descriptor.outputPortCount = outputPorts;
    m_descriptor.mainInputChannels = mainInChannels;
    m_descriptor.mainOutputChannels = mainOutChannels;
    return {};
}

PluginError ClapEffect::activate()
{
    // min and max pinned to the same value: this pipeline has exactly one block
    // size and will never present another, so a plugin is free to size its
    // internals exactly and the variable-block paths a DAW host needs are never
    // reached.
    if (!m_plugin->activate(m_plugin, static_cast<double>(CallAudioFormat::sampleRate),
                            CallAudioFormat::samplesPerFrame,
                            CallAudioFormat::samplesPerFrame)) {
        return makePluginError(PluginErrorCode::ActivationFailed,
                               tr("The plugin would not run at this call's audio settings."));
    }
    m_active = true;

    if (m_latency)
        m_latencySamples = static_cast<int>(m_latency->get(m_plugin));
    m_descriptor.latencySamples = m_latencySamples;

    if (!m_plugin->start_processing(m_plugin)) {
        return makePluginError(PluginErrorCode::ActivationFailed,
                               tr("The plugin would not start processing audio."));
    }
    m_processing = true;
    return {};
}

void ClapEffect::deactivate() noexcept
{
    if (!m_plugin)
        return;
    // Strict reverse order. Calling deactivate() while still processing, or
    // destroy() while still active, is undefined and in practice a crash inside
    // the plugin's own teardown.
    if (m_processing) {
        m_plugin->stop_processing(m_plugin);
        m_processing = false;
    }
    if (m_active) {
        m_plugin->deactivate(m_plugin);
        m_active = false;
    }
}

void ClapEffect::processMono(float *samples) noexcept
{
    if (!m_processing)
        return;

    // Denormals off for the duration of somebody else's arithmetic, and back on
    // the moment it returns.
    const ScopedNoDenormals noDenormals;

    // Duplicate the mono frame into every channel of the MAIN input port. Other
    // ports keep the silence they were zeroed with and are never written again.
    const size_t frameBytes = sizeof(float) * CallAudioFormat::samplesPerFrame;
    for (uint32_t channel = 0; channel < m_inBuffers[m_mainIn].channel_count; ++channel)
        std::memcpy(m_inPtrs[m_mainIn][channel], samples, frameBytes);

    clap_process_t process{};
    process.steady_time = static_cast<int64_t>(m_steadyTime);
    process.frames_count = CallAudioFormat::samplesPerFrame;
    process.transport = nullptr; // Free-running: a call has no song position.
    process.audio_inputs = m_inBuffers.data();
    process.audio_inputs_count = static_cast<uint32_t>(m_inBuffers.size());
    process.audio_outputs = m_outBuffers.data();
    process.audio_outputs_count = static_cast<uint32_t>(m_outBuffers.size());
    // Never null -- see ClapEventQueue.
    process.in_events = m_events.input();
    process.out_events = m_events.output();

    const clap_process_status status = m_plugin->process(m_plugin, &process);
    m_steadyTime += CallAudioFormat::samplesPerFrame;
    m_events.clear();

    if (status == CLAP_PROCESS_ERROR)
        return; // Leave the caller's buffer alone: this slot passes dry.

    // Channel 0 of the main output, never the average of the channels.
    std::memcpy(samples, m_outPtrs[m_mainOut][0], frameBytes);
}

void ClapEffect::reset() noexcept
{
    if (!m_plugin || !m_active)
        return;
    m_plugin->reset(m_plugin);
    m_steadyTime = 0;
    m_events.clear();
    for (std::vector<float> &storage : m_inStorage)
        std::fill(storage.begin(), storage.end(), 0.0f);
    for (std::vector<float> &storage : m_outStorage)
        std::fill(storage.begin(), storage.end(), 0.0f);
}

void ClapEffect::setParameter(quint32 parameterId, double value)
{
    if (!m_params)
        return;
    m_events.pushParameterValue(parameterId, m_parameterCookies.value(parameterId, nullptr),
                                value);
}

double ClapEffect::parameterValue(quint32 parameterId) const
{
    if (!m_params)
        return 0.0;
    double value = 0.0;
    if (!m_params->get_value(m_plugin, parameterId, &value))
        return 0.0;
    return value;
}

QString ClapEffect::parameterText(quint32 parameterId, double value) const
{
    if (!m_params || !m_params->value_to_text)
        return {};
    char buffer[CLAP_NAME_SIZE] = {};
    if (!m_params->value_to_text(m_plugin, parameterId, value, buffer, sizeof buffer))
        return {};
    // The plugin may not have terminated it.
    buffer[sizeof buffer - 1] = '\0';
    return QString::fromUtf8(buffer);
}

QByteArray ClapEffect::saveState() const
{
    if (!m_state)
        return {};

    QByteArray blob;
    clap_ostream_t stream{};
    stream.ctx = &blob;
    stream.write = [](const clap_ostream_t *out, const void *buffer, uint64_t size) -> int64_t {
        auto *target = static_cast<QByteArray *>(out->ctx);
        // A plugin is free to write more than a QByteArray can hold; refusing
        // is better than truncating a blob that would later be handed back.
        if (size > static_cast<uint64_t>(std::numeric_limits<int>::max() - target->size()))
            return -1;
        target->append(static_cast<const char *>(buffer), static_cast<qsizetype>(size));
        return static_cast<int64_t>(size);
    };
    if (!m_state->save(m_plugin, &stream))
        return {};
    return blob;
}

bool ClapEffect::loadState(const QByteArray &state)
{
    if (!m_state || state.isEmpty())
        return false;

    struct Reader {
        const QByteArray *data;
        qsizetype offset;
    } reader{&state, 0};

    clap_istream_t stream{};
    stream.ctx = &reader;
    stream.read = [](const clap_istream_t *in, void *buffer, uint64_t size) -> int64_t {
        auto *source = static_cast<Reader *>(in->ctx);
        const qsizetype remaining = source->data->size() - source->offset;
        if (remaining <= 0)
            return 0; // End of stream, which is not an error.
        const qsizetype take =
            std::min<qsizetype>(remaining, static_cast<qsizetype>(
                                               std::min<uint64_t>(size, INT64_MAX)));
        std::memcpy(buffer, source->data->constData() + source->offset,
                    static_cast<size_t>(take));
        source->offset += take;
        return static_cast<int64_t>(take);
    };
    return m_state->load(m_plugin, &stream);
}

} // namespace OpenChat
