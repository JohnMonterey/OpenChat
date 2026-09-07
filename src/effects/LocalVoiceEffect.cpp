#include "effects/LocalVoiceEffect.h"

#include "diagnostics/Logging.h"

namespace OpenChat {

LocalVoiceEffect::LocalVoiceEffect(VoiceEffectChainConfig config, QList<PluginConsent> consents)
    : m_config(std::move(config))
    , m_consents(std::move(consents))
{
}

LocalVoiceEffect::~LocalVoiceEffect() = default;

bool LocalVoiceEffect::prepare()
{
    m_prepared = true;
    // Sized once, here, so that process() never resizes it.
    m_out = AudioFrame(CallAudioFormat::bytesPerFrame, Qt::Uninitialized);

    if (!m_config.hasWork()) {
        // Not an error. "Effects are off" and "effects failed" are different
        // things and a settings panel must be able to tell them apart.
        m_chain.clear();
        return false;
    }

    const bool built = m_chain.build(m_config, m_consents);
    if (built) {
        qCInfo(effectsLog, "voice effects: %d stage(s) active, %d samples of added latency",
               m_chain.activeStageCount(), m_chain.latencySamples());
    }
    return built;
}

AudioFrame LocalVoiceEffect::process(const AudioFrame &frame)
{
    if (!m_prepared || !m_chain.isRunning())
        return frame;
    m_chain.process(frame, m_out);
    return m_out;
}

void LocalVoiceEffect::reset()
{
    m_chain.reset();
}

VoiceEffectStatus LocalVoiceEffect::status() const
{
    VoiceEffectStatus status;
    status.running = m_chain.isRunning();
    status.activeSlots = m_chain.activeStageCount();
    status.latencySamples = m_chain.latencySamples();
    status.missedFrames = m_chain.missedFrames();
    status.error = m_chain.error();
    return status;
}

VoiceEffectFactory LocalVoiceEffect::factoryFor(VoiceEffectChainConfig config,
                                                QList<PluginConsent> consents)
{
    return [config = std::move(config), consents = std::move(consents)]() {
        return std::make_unique<LocalVoiceEffect>(config, consents);
    };
}

} // namespace OpenChat
