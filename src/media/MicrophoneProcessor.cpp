#include "media/MicrophoneProcessor.h"

#include <QtEndian>

#include <algorithm>
#include <cmath>

namespace OpenChat {

namespace {

SpeechLevelMeter::Config meterConfigFor(const MicrophoneProcessor::Config &config)
{
    SpeechLevelMeter::Config meter;
    meter.threshold = config.gateThreshold;
    meter.hangoverFrames = config.gateHoldFrames;
    return meter;
}

} // namespace

MicrophoneProcessor::MicrophoneProcessor(Config config)
{
    setConfig(config);
}

void MicrophoneProcessor::setConfig(Config config)
{
    config.gain = std::clamp(std::isfinite(config.gain) ? config.gain : 1.0, 0.0, 4.0);
    config.gateThreshold =
        std::clamp(std::isfinite(config.gateThreshold) ? config.gateThreshold : 0.0, 0.0, 1.0);
    config.gateHoldFrames = std::max(0, config.gateHoldFrames);
    m_config = config;
    m_meter.setConfig(meterConfigFor(m_config));
    if (!m_config.gateEnabled)
        m_open = true;
}

AudioFrame MicrophoneProcessor::process(const AudioFrame &frame)
{
    if (!isFullAudioFrame(frame))
        return frame;
    const AudioFrame gained = applyGain(frame, m_config.gain);
    // The meter hears the frame as it would be sent, so the level the user
    // sees in settings is the level the far end would receive.
    m_meter.update(gained);
    if (!m_config.gateEnabled || m_meter.isSpeaking()) {
        m_open = true;
        return gained;
    }
    if (m_open) {
        // The closing frame: ramp it to silence instead of cutting it.
        m_open = false;
        return fadeOut(gained);
    }
    return silentAudioFrame();
}

void MicrophoneProcessor::reset()
{
    m_meter.reset();
    m_open = true;
}

AudioFrame MicrophoneProcessor::applyGain(const AudioFrame &frame, double gain)
{
    if (gain == 1.0)
        return frame;
    AudioFrame out(frame.size(), Qt::Uninitialized);
    const char *in = frame.constData();
    char *dst = out.data();
    for (qsizetype offset = 0; offset < frame.size(); offset += CallAudioFormat::bytesPerSample) {
        const double scaled = qFromLittleEndian<qint16>(in + offset) * gain;
        const long clamped = std::clamp(std::lround(scaled), -32768L, 32767L);
        qToLittleEndian<qint16>(static_cast<qint16>(clamped), dst + offset);
    }
    return out;
}

AudioFrame MicrophoneProcessor::fadeOut(const AudioFrame &frame)
{
    AudioFrame out(frame.size(), Qt::Uninitialized);
    const char *in = frame.constData();
    char *dst = out.data();
    constexpr int samples = CallAudioFormat::samplesPerFrame;
    for (int i = 0; i < samples; ++i) {
        const double weight = 1.0 - static_cast<double>(i) / samples;
        const qsizetype offset = static_cast<qsizetype>(i) * CallAudioFormat::bytesPerSample;
        const double scaled = qFromLittleEndian<qint16>(in + offset) * weight;
        qToLittleEndian<qint16>(static_cast<qint16>(std::lround(scaled)), dst + offset);
    }
    return out;
}

} // namespace OpenChat
