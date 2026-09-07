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
    if (config.gain == 0 && m_config.gain != 0)
        m_voice.reset();
    m_config = config;
    m_voice.configure(config.voice);
    m_meter.setConfig(meterConfigFor(m_config));
    if (!m_config.gateEnabled)
        m_open = true;
}

AudioFrame MicrophoneProcessor::process(const AudioFrame &frame)
{
    if (!isFullAudioFrame(frame))
        return frame;
    const AudioFrame gained = applyGain(frame, m_config.gain);
    // Meter the gained input before effects: a reverb tail or distortion must
    // not open the gate or change the threshold the user is tuning.
    m_meter.update(gained);
    if (!m_config.gateEnabled || m_meter.isSpeaking()) {
        m_open = true;
        return applyVoiceEffects(gained);
    }
    if (m_open) {
        // The closing frame: ramp it to silence instead of cutting it.
        m_open = false;
        return applyVoiceEffects(fadeOut(gained));
    }
    // Gate the input, not the effect's output: a room reflection can finish
    // naturally after speech stops. Muting clears the processor in CallEngine.
    return applyVoiceEffects(silentAudioFrame());
}

void MicrophoneProcessor::reset()
{
    m_meter.reset();
    m_open = true;
    m_voice.reset();
}

AudioFrame MicrophoneProcessor::applyVoiceEffects(const AudioFrame &frame)
{
    if (!m_voice.enabled())
        return frame;
    std::array<float, CallAudioFormat::samplesPerFrame> samples;
    for (int i = 0; i < CallAudioFormat::samplesPerFrame; ++i)
        samples[i] = qFromLittleEndian<qint16>(frame.constData() + i * 2) / 32768.f;
    m_voice.process(samples);
    AudioFrame out(frame.size(), Qt::Uninitialized);
    for (int i = 0; i < CallAudioFormat::samplesPerFrame; ++i)
        qToLittleEndian<qint16>(static_cast<qint16>(std::clamp(std::lround(samples[i] * 32768), -32768L, 32767L)),
                              out.data() + i * 2);
    return out;
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
