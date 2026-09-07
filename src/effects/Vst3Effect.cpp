#include "effects/Vst3Effect.h"

#include "effects/DenormalGuard.h"
#include "effects/Vst3Module.h"

#include <pluginterfaces/vst/vstspeaker.h>

#include <QCoreApplication>

#include <algorithm>
#include <cstring>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace OpenChat {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("Vst3Effect", text);
}

QString textOf(const TChar *text)
{
    // String128 is UTF-16 and TChar is char16_t on this platform.
    return text ? QString::fromUtf16(reinterpret_cast<const char16_t *>(text)) : QString();
}

int channelsOf(SpeakerArrangement arrangement)
{
    return static_cast<int>(SpeakerArr::getChannelCount(arrangement));
}

} // namespace

Vst3Effect::~Vst3Effect()
{
    teardown();
}

double Vst3Effect::toNormalised(const AudioPluginParameter &parameter, double plain)
{
    const double low = std::min(parameter.minValue, parameter.maxValue);
    const double high = std::max(parameter.minValue, parameter.maxValue);
    if (high <= low)
        return 0.0;
    return std::clamp((plain - low) / (high - low), 0.0, 1.0);
}

double Vst3Effect::toPlain(const AudioPluginParameter &parameter, double normalised)
{
    const double low = std::min(parameter.minValue, parameter.maxValue);
    const double high = std::max(parameter.minValue, parameter.maxValue);
    return low + std::clamp(normalised, 0.0, 1.0) * (high - low);
}

std::unique_ptr<Vst3Effect> Vst3Effect::create(std::shared_ptr<Vst3Module> module,
                                               const AudioPluginDescriptor &descriptor,
                                               PluginError &error)
{
    IPluginFactory *factory = module ? module->factory() : nullptr;
    if (!factory) {
        error = makePluginError(PluginErrorCode::EntryNotFound,
                                tr("The plugin could not be created."));
        return nullptr;
    }

    TUID uid{};
    const QByteArray uidBytes = QByteArray::fromHex(descriptor.id.entryId.toLatin1());
    if (uidBytes.size() != 16) {
        error = makePluginError(PluginErrorCode::EntryNotFound,
                                tr("The plugin could not be created."));
        return nullptr;
    }
    std::memcpy(uid, uidBytes.constData(), 16);

    std::unique_ptr<Vst3Effect> effect(new Vst3Effect);
    effect->m_module = std::move(module);
    effect->m_descriptor = descriptor;

    if (factory->createInstance(uid, IComponent::iid,
                                reinterpret_cast<void **>(&effect->m_component)) != kResultOk
        || !effect->m_component) {
        error = makePluginError(PluginErrorCode::EntryNotFound,
                                tr("The plugin could not be created."));
        return nullptr;
    }

    // NEVER null. A plugin dereferences the context to ask the host its name or
    // to create a message object, and enough of them do it unconditionally that
    // passing null is simply not survivable.
    if (effect->m_component->initialize(effect->m_module->hostApplication()) != kResultOk) {
        error = makePluginError(PluginErrorCode::ActivationFailed,
                                tr("The plugin failed to start."));
        return nullptr;
    }

    if (effect->m_component->queryInterface(IAudioProcessor::iid,
                                            reinterpret_cast<void **>(&effect->m_processor))
            != kResultOk
        || !effect->m_processor) {
        error = makePluginError(PluginErrorCode::ActivationFailed,
                                tr("This plugin does not process audio."));
        return nullptr;
    }

    // 32-bit float or nothing. AudioBusBuffers is a union of 32- and 64-bit
    // channel pointers, and a plugin that only accepts kSample64 would read our
    // float buffers as doubles -- silence or noise, never a diagnosable error.
    if (effect->m_processor->canProcessSampleSize(kSample32) != kResultTrue) {
        error = makePluginError(PluginErrorCode::ActivationFailed,
                                tr("This plugin cannot process audio at this call's quality."));
        return nullptr;
    }

    // The edit controller. A single-component effect returns itself here; a
    // two-component one publishes a separate class. Identity is decided by
    // comparing queryInterface(FUnknown) results, never the interface pointers,
    // because a single object's IComponent and IEditController pointers
    // legitimately differ under this ABI.
    if (effect->m_component->queryInterface(IEditController::iid,
                                            reinterpret_cast<void **>(&effect->m_controller))
        == kResultOk) {
        effect->m_singleComponent = true;
    } else {
        effect->m_controller = nullptr;
        TUID controllerUid{};
        if (effect->m_component->getControllerClassId(controllerUid) == kResultOk) {
            if (factory->createInstance(controllerUid, IEditController::iid,
                                        reinterpret_cast<void **>(&effect->m_controller))
                    == kResultOk
                && effect->m_controller) {
                if (effect->m_controller->initialize(effect->m_module->hostApplication())
                    != kResultOk) {
                    effect->m_controller->release();
                    effect->m_controller = nullptr;
                }
            }
        }
    }
    if (effect->m_controller)
        effect->m_controller->setComponentHandler(&effect->m_handler);

    if (const PluginError busError = effect->negotiateBuses()) {
        error = busError;
        return nullptr;
    }
    if (const PluginError activateError = effect->activate()) {
        error = activateError;
        return nullptr;
    }

    // Parameters come from the controller, not the component. A plugin with no
    // controller has no parameters a host can name, which is unusual but legal.
    effect->m_descriptor.parameters.clear();
    if (effect->m_controller) {
        const int32 count = effect->m_controller->getParameterCount();
        for (int32 i = 0; i < count; ++i) {
            ParameterInfo info{};
            if (effect->m_controller->getParameterInfo(i, info) != kResultOk)
                continue;
            AudioPluginParameter parameter;
            parameter.id = info.id;
            parameter.name = textOf(info.title);
            parameter.module = textOf(info.units);
            // VST3 has no plain range on the wire: everything is normalised and
            // the plugin converts. Publishing [0, 1] here is therefore the
            // honest range, and toPlain/toNormalised are identities for it --
            // which keeps a stored chain meaningful when the plugin's own
            // display range changes between versions.
            parameter.minValue = 0.0;
            parameter.maxValue = 1.0;
            parameter.defaultValue = info.defaultNormalizedValue;
            parameter.stepped = info.stepCount > 0;
            parameter.readOnly = (info.flags & ParameterInfo::kIsReadOnly) != 0;
            parameter.hidden = (info.flags & ParameterInfo::kIsHidden) != 0;
            effect->m_descriptor.parameters.append(parameter);
            effect->m_parametersById.insert(parameter.id, parameter);
        }
    }

    error = {};
    return effect;
}

PluginError Vst3Effect::negotiateBuses()
{
    const int32 inputBuses = m_component->getBusCount(kAudio, kInput);
    const int32 outputBuses = m_component->getBusCount(kAudio, kOutput);
    if (outputBuses < 1) {
        return makePluginError(PluginErrorCode::ChannelLayoutUnsupported,
                               tr("This plugin produces no audio."));
    }

    // The proposal. kMono is (1 << 19) -- SpeakerArr::kMono -- and NOT 1, which
    // is kSpeakerL and means a one-channel LEFT bus. Only the main buses are
    // proposed as mono; auxiliary buses are proposed as themselves so a
    // sidechain is not accidentally reshaped.
    std::vector<SpeakerArrangement> inputArrangements(std::max<int32>(inputBuses, 0),
                                                      SpeakerArr::kMono);
    std::vector<SpeakerArrangement> outputArrangements(static_cast<size_t>(outputBuses),
                                                       SpeakerArr::kMono);
    m_processor->setBusArrangements(inputArrangements.data(), inputBuses,
                                    outputArrangements.data(), outputBuses);
    // The return value above is deliberately ignored. kResultTrue is zero here,
    // kResultFalse means "adapted", and real plugins return values documented
    // nowhere; the only trustworthy answer is to read back what actually took.

    auto readBack = [&](BusDirection direction, int32 busCount,
                        std::vector<SpeakerArrangement> &into) {
        into.assign(static_cast<size_t>(std::max<int32>(busCount, 0)), 0);
        for (int32 bus = 0; bus < busCount; ++bus) {
            SpeakerArrangement arrangement = 0;
            if (m_processor->getBusArrangement(direction, bus, arrangement) == kResultOk)
                into[static_cast<size_t>(bus)] = arrangement;
        }
    };
    readBack(kInput, inputBuses, inputArrangements);
    readBack(kOutput, outputBuses, outputArrangements);

    const int mainInChannels =
        inputBuses > 0 ? channelsOf(inputArrangements[0]) : 0;
    const int mainOutChannels = channelsOf(outputArrangements[0]);
    if (mainOutChannels < 1 || mainOutChannels > 2 || mainInChannels > 2) {
        return makePluginError(
            PluginErrorCode::ChannelLayoutUnsupported,
            tr("This plugin needs %1 in and %2 out channels; a call is mono.")
                .arg(mainInChannels)
                .arg(mainOutChannels));
    }

    auto buildBuffers = [](const std::vector<SpeakerArrangement> &arrangements,
                           std::vector<std::vector<float>> &storage,
                           std::vector<std::vector<float *>> &pointers,
                           std::vector<AudioBusBuffers> &buffers) {
        const size_t busCount = arrangements.size();
        storage.assign(busCount, {});
        pointers.assign(busCount, {});
        buffers.assign(busCount, AudioBusBuffers{});
        for (size_t bus = 0; bus < busCount; ++bus) {
            const size_t channels =
                static_cast<size_t>(std::max(0, channelsOf(arrangements[bus])));
            storage[bus].assign(channels * CallAudioFormat::samplesPerFrame, 0.0f);
            pointers[bus].resize(channels);
            for (size_t channel = 0; channel < channels; ++channel) {
                pointers[bus][channel] =
                    storage[bus].data() + channel * CallAudioFormat::samplesPerFrame;
            }
            buffers[bus].numChannels = static_cast<int32>(channels);
            buffers[bus].silenceFlags = 0;
            buffers[bus].channelBuffers32 = pointers[bus].data();
        }
    };
    // Every declared bus gets real, zeroed storage -- sidechains included, for
    // the same reason CLAP's do.
    buildBuffers(inputArrangements, m_inStorage, m_inPtrs, m_inBuffers);
    buildBuffers(outputArrangements, m_outStorage, m_outPtrs, m_outBuffers);

    m_descriptor.inputPortCount = static_cast<int>(inputBuses);
    m_descriptor.outputPortCount = static_cast<int>(outputBuses);
    m_descriptor.mainInputChannels = mainInChannels;
    m_descriptor.mainOutputChannels = mainOutChannels;
    return {};
}

PluginError Vst3Effect::activate()
{
    ProcessSetup setup{};
    setup.processMode = kRealtime;
    setup.symbolicSampleSize = kSample32;
    setup.maxSamplesPerBlock = CallAudioFormat::samplesPerFrame;
    setup.sampleRate = static_cast<SampleRate>(CallAudioFormat::sampleRate);
    if (m_processor->setupProcessing(setup) != kResultOk) {
        return makePluginError(PluginErrorCode::ActivationFailed,
                               tr("The plugin would not run at this call's audio settings."));
    }

    // Buses are inactive until told otherwise, and a plugin whose output bus was
    // never activated writes nothing while reporting complete success.
    for (int32 direction = 0; direction < 2; ++direction) {
        const BusDirection busDirection = direction == 0 ? kInput : kOutput;
        const int32 busCount = m_component->getBusCount(kAudio, busDirection);
        for (int32 bus = 0; bus < busCount; ++bus)
            m_component->activateBus(kAudio, busDirection, bus, true);
    }

    if (m_component->setActive(true) != kResultOk) {
        return makePluginError(PluginErrorCode::ActivationFailed,
                               tr("The plugin would not start."));
    }
    m_active = true;

    m_latencySamples = static_cast<int>(m_processor->getLatencySamples());
    m_descriptor.latencySamples = m_latencySamples;

    if (m_processor->setProcessing(true) != kResultOk) {
        // Documented as optional in the sense that some plugins simply do not
        // implement it. Treating a refusal as fatal would exclude them for no
        // reason, so processing continues either way.
        m_processing = true;
    } else {
        m_processing = true;
    }
    return {};
}

void Vst3Effect::teardown() noexcept
{
    // Strict reverse order, and each step guarded by whether it happened.
    if (m_processor && m_processing) {
        m_processor->setProcessing(false);
        m_processing = false;
    }
    if (m_component && m_active) {
        m_component->setActive(false);
        m_active = false;
    }
    if (m_controller) {
        m_controller->setComponentHandler(nullptr);
        // A separate controller is ours to terminate. A single-component effect
        // is the SAME object as the component, so terminating it here and again
        // below would be a double teardown -- the classic VST3 host double free.
        if (!m_singleComponent)
            m_controller->terminate();
        m_controller->release();
        m_controller = nullptr;
    }
    if (m_processor) {
        m_processor->release();
        m_processor = nullptr;
    }
    if (m_component) {
        m_component->terminate();
        m_component->release();
        m_component = nullptr;
    }
}

void Vst3Effect::processMono(float *samples) noexcept
{
    if (!m_processing || m_outBuffers.empty())
        return;

    const ScopedNoDenormals noDenormals;

    const size_t frameBytes = sizeof(float) * CallAudioFormat::samplesPerFrame;
    if (!m_inBuffers.empty()) {
        for (int32 channel = 0; channel < m_inBuffers[0].numChannels; ++channel)
            std::memcpy(m_inPtrs[0][static_cast<size_t>(channel)], samples, frameBytes);
    }

    // Value-initialised: ProcessData has padding and unused pointer members, and
    // handing a plugin an uninitialised processContext or inputEvents is a read
    // of whatever was on the stack.
    ProcessData data{};
    data.processMode = kRealtime;
    data.symbolicSampleSize = kSample32;
    data.numSamples = CallAudioFormat::samplesPerFrame;
    data.numInputs = static_cast<int32>(m_inBuffers.size());
    data.numOutputs = static_cast<int32>(m_outBuffers.size());
    data.inputs = m_inBuffers.empty() ? nullptr : m_inBuffers.data();
    data.outputs = m_outBuffers.data();
    data.inputParameterChanges = &m_inputChanges;
    // Documented as optional and not: plugins written against certain
    // frameworks assert on a null output list.
    data.outputParameterChanges = &m_outputChanges;
    data.inputEvents = nullptr;
    data.outputEvents = nullptr;
    data.processContext = nullptr; // Free-running: a call has no song position.

    m_processor->process(data);
    m_inputChanges.clear();
    m_outputChanges.clear();

    // Channel 0 of the main output bus, never the average -- see ClapEffect for
    // why averaging a stereo effect's channels can cancel the effect entirely.
    if (m_outBuffers[0].numChannels > 0)
        std::memcpy(samples, m_outPtrs[0][0], frameBytes);
}

void Vst3Effect::reset() noexcept
{
    if (!m_processor || !m_active)
        return;
    // VST3 has no reset(): the documented way to drop a plugin's tail is to
    // stop and restart processing, which is what a DAW does on transport stop.
    m_processor->setProcessing(false);
    m_processor->setProcessing(true);
    m_inputChanges.clear();
    m_outputChanges.clear();
    for (std::vector<float> &storage : m_inStorage)
        std::fill(storage.begin(), storage.end(), 0.0f);
    for (std::vector<float> &storage : m_outStorage)
        std::fill(storage.begin(), storage.end(), 0.0f);
}

void Vst3Effect::setParameter(quint32 parameterId, double value)
{
    const auto it = m_parametersById.constFind(parameterId);
    if (it == m_parametersById.constEnd())
        return;
    const double normalised = toNormalised(*it, value);
    // Both sides: the queue is what the processor reads during the next block,
    // and the controller is what reports the value back and renders its text.
    m_inputChanges.queue(parameterId, normalised);
    if (m_controller)
        m_controller->setParamNormalized(parameterId, normalised);
}

double Vst3Effect::parameterValue(quint32 parameterId) const
{
    if (!m_controller)
        return 0.0;
    const auto it = m_parametersById.constFind(parameterId);
    if (it == m_parametersById.constEnd())
        return 0.0;
    return toPlain(*it, m_controller->getParamNormalized(parameterId));
}

QString Vst3Effect::parameterText(quint32 parameterId, double value) const
{
    if (!m_controller)
        return {};
    const auto it = m_parametersById.constFind(parameterId);
    if (it == m_parametersById.constEnd())
        return {};
    String128 text{};
    if (m_controller->getParamStringByValue(parameterId, toNormalised(*it, value), text)
        != kResultOk) {
        return {};
    }
    return textOf(text);
}

QByteArray Vst3Effect::saveState() const
{
    if (!m_component)
        return {};
    Vst3MemoryStream stream;
    if (m_component->getState(&stream) != kResultOk)
        return {};
    return stream.data();
}

bool Vst3Effect::loadState(const QByteArray &state)
{
    if (!m_component || state.isEmpty())
        return false;
    Vst3MemoryStream stream(state);
    if (m_component->setState(&stream) != kResultOk)
        return false;
    // The controller needs the same blob to keep its parameter values in step
    // with the processor's. Rewound first, because setState consumed it.
    if (m_controller) {
        stream.rewind();
        m_controller->setComponentState(&stream);
    }
    return true;
}

} // namespace OpenChat
