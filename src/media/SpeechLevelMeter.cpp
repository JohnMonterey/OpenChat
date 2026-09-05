#include "media/SpeechLevelMeter.h"

#include "media/AudioConvert.h"

#include <algorithm>

namespace OpenChat {

SpeechLevelMeter::SpeechLevelMeter(Config config)
    : m_config(config)
{
    m_config.threshold = std::clamp(m_config.threshold, 0.0, 1.0);
    m_config.release = std::clamp(m_config.release, 0.0, 0.999);
    m_config.hangoverFrames = std::max(0, m_config.hangoverFrames);
}

void SpeechLevelMeter::apply(double rms)
{
    // Fast attack, slow release: rise straight to a louder frame, fall only at
    // the release rate. This is what keeps the ring steady through the dips
    // inside a spoken word.
    m_level = rms > m_level ? rms : m_level * m_config.release;
    if (rms >= m_config.threshold)
        m_hangover = m_config.hangoverFrames;
    else if (m_hangover > 0)
        --m_hangover;
}

void SpeechLevelMeter::update(const AudioFrame &frame)
{
    apply(isFullAudioFrame(frame) ? AudioConvert::frameRms(frame) : 0.0);
}

void SpeechLevelMeter::updateSilent()
{
    apply(0.0);
}

void SpeechLevelMeter::reset()
{
    m_level = 0.0;
    m_hangover = 0;
}

} // namespace OpenChat
