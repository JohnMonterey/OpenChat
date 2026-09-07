#include "effects/VoiceEffectChain.h"

#include "diagnostics/Logging.h"
#include "effects/PluginScanner.h"
#include "effects/SampleBridge.h"

#include <QCoreApplication>
#include <QElapsedTimer>

#include <algorithm>

namespace OpenChat {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("VoiceEffectChain", text);
}

// A frame is 20 ms of audio. Spending more than a quarter of that on effects
// leaves the rest of the pipeline -- Opus, crypto, the network write, and every
// other thing sharing this thread -- visibly less room than it had.
constexpr qint64 frameBudgetUs = 5000;
// One slow frame is a scheduling accident. Twenty-five in a row is half a
// second of a plugin that cannot keep up, and it will not recover on its own.
constexpr int overrunsBeforeDisable = 25;
// Crossfades the processed signal into the dry one across one frame, so that
// switching effects off is heard as the effect ending rather than as a click.
// Applied in place: `wet` becomes the blend.
void crossfadeToDry(float *wet, const AudioFrame &dry) noexcept
{
    float dryFloat[CallAudioFormat::samplesPerFrame];
    SampleBridge::frameToFloat(dry, dryFloat);
    constexpr int samples = CallAudioFormat::samplesPerFrame;
    for (int i = 0; i < samples; ++i) {
        const float wetWeight = 1.0f - static_cast<float>(i) / samples;
        wet[i] = wet[i] * wetWeight + dryFloat[i] * (1.0f - wetWeight);
    }
}

} // namespace

VoiceEffectChain::~VoiceEffectChain()
{
    clear();
}

bool VoiceEffectChain::build(const VoiceEffectChainConfig &config,
                             const QList<PluginConsent> &consents)
{
    clear();
    m_scratch.assign(CallAudioFormat::samplesPerFrame, 0.0f);

    for (const VoiceEffectStage &stage : config.stages) {
        if (!stage.enabled || !stage.id.isValid())
            continue;
        if (static_cast<int>(m_stages.size()) >= VoiceEffectChainConfig::maxStages)
            break;

        StageStatus status;
        status.id = stage.id;

        // Consent is checked against the file that will actually be executed,
        // which for VST3 is inside the bundle rather than the bundle itself.
        const QString loadable =
            PluginScanner::loadablePathFor(stage.id.format, stage.id.bundlePath);
        status.error = AudioPluginTrust::verifyAgainstConsent(loadable, consents);
        if (status.error) {
            qCWarning(effectsLog, "refusing plugin %s: %s",
                      qUtf8Printable(stage.id.toString()), qUtf8Printable(status.error.message));
            m_stageStatus.append(status);
            continue;
        }

        PluginError openError;
        std::shared_ptr<AudioPluginModule> module =
            PluginScanner::openModule(stage.id.format, stage.id.bundlePath, openError);
        if (!module) {
            status.error = openError;
            m_stageStatus.append(status);
            continue;
        }

        PluginError instanceError;
        std::unique_ptr<AudioPluginInstance> instance =
            module->createInstance(stage.id.entryId, instanceError);
        if (!instance) {
            status.error = instanceError;
            m_stageStatus.append(status);
            continue;
        }

        // State first, then explicit parameters: a stored blob is the whole
        // configuration and the parameter map is the user's later edits on top
        // of it, so applying them the other way round would discard the edits.
        if (!stage.state.isEmpty())
            instance->loadState(stage.state);
        for (auto it = stage.parameters.constBegin(); it != stage.parameters.constEnd(); ++it)
            instance->setParameter(it.key(), it.value());

        status.name = instance->descriptor().name;
        status.loaded = true;
        status.latencySamples = instance->latencySamples();
        m_latencySamples += status.latencySamples;
        m_stageStatus.append(status);
        m_stages.push_back(LiveStage{std::move(module), std::move(instance)});
    }

    m_running = !m_stages.empty();
    if (!m_running && !m_stageStatus.isEmpty()) {
        // Report the first real reason rather than a generic one: the user
        // needs to know it was refused, not merely that nothing happened.
        for (const StageStatus &status : std::as_const(m_stageStatus)) {
            if (status.error) {
                m_error = status.error.message;
                break;
            }
        }
    }
    return m_running;
}

void VoiceEffectChain::clear() noexcept
{
    // Instances before modules: destroying a module unloads the code an
    // instance is still living in. The vector's own order would be the reverse.
    for (auto it = m_stages.rbegin(); it != m_stages.rend(); ++it)
        it->instance.reset();
    m_stages.clear();
    m_stageStatus.clear();
    m_latencySamples = 0;
    m_missedFrames = 0;
    m_consecutiveOverruns = 0;
    m_running = false;
    m_error.clear();
}

void VoiceEffectChain::process(const AudioFrame &in, AudioFrame &out) noexcept
{
    // A frame of the wrong length is passed through untouched, for the same
    // reason MicrophoneProcessor does it: nothing downstream would accept it.
    if (!isFullAudioFrame(in)) {
        out = in;
        return;
    }
    if (!m_running || m_stages.empty()) {
        out = in;
        return;
    }

    if (out.size() != CallAudioFormat::bytesPerFrame)
        out.resize(CallAudioFormat::bytesPerFrame);

    SampleBridge::frameToFloat(in, m_scratch.data());

    QElapsedTimer timer;
    timer.start();
    for (LiveStage &stage : m_stages)
        stage.instance->processMono(m_scratch.data());
    const qint64 elapsedUs = timer.nsecsElapsed() / 1000;

    // NaN checked once, on the chain's output, rather than after every stage:
    // one pass over 960 floats is about a microsecond, and a stage that poisons
    // the signal poisons it all the way to the end regardless of which one it
    // was.
    if (!SampleBridge::isFinite(m_scratch.data())) {
        ++m_missedFrames;
        disableWith(tr("A voice effect stopped working and was switched off."),
                    PluginErrorCode::ProducedNonFinite);
        // No crossfade here, and deliberately: there is nothing finite to fade
        // FROM. Blending a NaN buffer towards dry yields NaN for most of the
        // frame, so the step into dry audio is the better of the two.
        out = in;
        return;
    }

    if (elapsedUs > frameBudgetUs) {
        ++m_missedFrames;
        if (++m_consecutiveOverruns >= overrunsBeforeDisable) {
            disableWith(tr("A voice effect was too slow for a call and was switched off."),
                        PluginErrorCode::TooSlow);
            // This frame's processed audio is valid -- it merely took too long
            // to produce -- so it is what the fade starts from.
            crossfadeToDry(m_scratch.data(), in);
            SampleBridge::floatToFrame(m_scratch.data(), out.data());
            return;
        }
    } else {
        m_consecutiveOverruns = 0;
    }

    SampleBridge::floatToFrame(m_scratch.data(), out.data());
}

void VoiceEffectChain::disableWith(const QString &reason, PluginErrorCode code)
{
    qCWarning(effectsLog, "voice effect chain disabled: %s", qUtf8Printable(reason));
    m_error = reason;
    m_running = false;
    for (StageStatus &status : m_stageStatus) {
        if (status.loaded && !status.error)
            status.error = makePluginError(code, reason);
    }
    // The plugins are kept alive rather than destroyed here. Unloading a shared
    // object from inside the frame that was running its code is not something
    // to attempt; teardown happens when the call ends or the chain is rebuilt.
}

void VoiceEffectChain::reset() noexcept
{
    for (LiveStage &stage : m_stages)
        stage.instance->reset();
    std::fill(m_scratch.begin(), m_scratch.end(), 0.0f);
    m_consecutiveOverruns = 0;
}

void VoiceEffectChain::setParameter(int stageIndex, quint32 parameterId, double value)
{
    if (stageIndex < 0 || stageIndex >= static_cast<int>(m_stages.size()))
        return;
    m_stages[static_cast<size_t>(stageIndex)].instance->setParameter(parameterId, value);
}

QList<QByteArray> VoiceEffectChain::captureState() const
{
    QList<QByteArray> states;
    states.reserve(static_cast<qsizetype>(m_stages.size()));
    for (const LiveStage &stage : m_stages)
        states.append(stage.instance->saveState());
    return states;
}

} // namespace OpenChat
