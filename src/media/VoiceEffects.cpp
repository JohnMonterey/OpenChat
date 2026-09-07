#include "media/VoiceEffects.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace OpenChat {
namespace {
constexpr double pi = std::numbers::pi;
constexpr double rate = 48000.0;
float bounded(double x) { return static_cast<float>(std::clamp(x, -1.0, 1.0)); }
}

bool VoiceEffects::validEffect(int effect)
{
    return effect >= int(Effect::None) && effect <= int(Effect::Anonymous);
}

void VoiceEffects::configure(Config config)
{
    if (!validEffect(int(config.effect)))
        config.effect = Effect::None;
    config.intensity = std::clamp(std::isfinite(config.intensity) ? config.intensity : 0.5, 0.0, 1.0);
    // Discard old delay/spectral samples when changing modes. In particular,
    // turning an effect off must never replay its tail after turning it on.
    const bool changed = config.effect != m_config.effect || config.studio != m_config.studio
        || config.noiseReduction != m_config.noiseReduction
        || config.automaticGain != m_config.automaticGain || config.compressor != m_config.compressor
        || (config.intensity == 0) != (m_config.intensity == 0);
    m_config = config;
    double low = 240, high = 3800;
    if (config.effect == Effect::WalkieTalkie) { low = 600; high = 2500; }
    if (config.effect == Effect::Telephone) { low = 300; high = 3400; }
    if (config.effect == Effect::Intercom) { low = 450; high = 4200; }
    if (config.effect == Effect::Megaphone) { low = 450; high = 3500; }
    m_high.set(true, low);
    m_low.set(false, high);
    m_presence.peak(config.effect == Effect::Intercom ? 1100 : 1600, 2.0, 6);
    m_studioHigh.set(true, 85);
    m_studioPresence.peak(2600, 0.8, 2.5);
    m_deessLow.set(false, 5000);
    if (changed)
        reset();
}

void VoiceEffects::reset()
{
    for (auto *filter : {&m_high, &m_low, &m_presence, &m_studioHigh, &m_studioPresence, &m_deessLow})
        filter->reset();
    m_envelope = m_sibilance = m_phase = m_squelchEnvelope = 0;
    m_autoGain = 1;
    m_crackleRemaining = 0;
    m_random = 0x12345678;
    for (auto &line : m_room) line.fill(0);
    m_roomPosition.fill(0);
    m_roomDamping.fill(0);
    m_input.fill(0); m_output.fill(0);
    m_noiseFloor.fill(0); m_previousPhase.fill(0); m_synthesisPhase.fill(0);
    m_position = m_hop = 0;
}

bool VoiceEffects::enabled() const
{
    return (m_config.effect != Effect::None && m_config.intensity > 0)
        || m_config.studio || m_config.noiseReduction || m_config.automaticGain || m_config.compressor;
}

void VoiceEffects::process(std::span<float> samples)
{
    if (!enabled())
        return;
    const bool formant = m_config.intensity > 0 && (m_config.effect == Effect::DeepVoice
        || m_config.effect == Effect::TinyVoice || m_config.effect == Effect::Anonymous);
    for (float &sample : samples) {
        float x = std::isfinite(sample) ? bounded(sample) : 0;
        // Spectral processing introduces 2048 samples (~43 ms) only for
        // formant/pitch effects and noise reduction. Ordinary effects add none.
        if (formant || m_config.noiseReduction || m_config.studio)
            x = spectral(x);
        x = enhance(x);
        if (m_config.intensity > 0 && !formant && m_config.effect != Effect::None) {
            const float wet = effect(x);
            x = static_cast<float>(x + m_config.intensity * (wet - x));
        }
        sample = bounded(x);
    }
}

float VoiceEffects::Biquad::process(float x)
{
    const double y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return static_cast<float>(y);
}

void VoiceEffects::Biquad::set(bool highPass, double frequency, double q)
{
    const double w = 2 * pi * frequency / rate, c = std::cos(w), alpha = std::sin(w) / (2 * q);
    const double a0 = 1 + alpha;
    b0 = (highPass ? 1 + c : 1 - c) / (2 * a0);
    b1 = (highPass ? -(1 + c) : 1 - c) / a0;
    b2 = b0; a1 = -2 * c / a0; a2 = (1 - alpha) / a0;
}

void VoiceEffects::Biquad::peak(double frequency, double q, double db)
{
    const double a = std::pow(10.0, db / 40), w = 2 * pi * frequency / rate;
    const double alpha = std::sin(w) / (2 * q), c = std::cos(w), a0 = 1 + alpha / a;
    b0 = (1 + alpha * a) / a0; b1 = -2 * c / a0; b2 = (1 - alpha * a) / a0;
    a1 = b1; a2 = (1 - alpha / a) / a0;
}

float VoiceEffects::enhance(float x)
{
    if (m_config.studio) {
        x = m_studioPresence.process(m_studioHigh.process(x));
        const double high = std::abs(x - m_deessLow.process(x));
        m_sibilance += (high > m_sibilance ? 0.02 : 0.001) * (high - m_sibilance);
        x /= 1 + std::max(0.0, m_sibilance - 0.045) * 10;
    }
    const double level = std::abs(x);
    m_envelope += (level > m_envelope ? 0.006 : 0.00025) * (level - m_envelope);
    if (m_config.automaticGain) {
        const double target = m_envelope > 0.008 ? std::clamp(0.14 / m_envelope, 0.5, 3.0) : 1.0;
        m_autoGain += (target < m_autoGain ? 0.001 : 0.00004) * (target - m_autoGain);
        x *= m_autoGain;
    }
    if (m_config.compressor || m_config.studio) {
        const double levelAfterGain = m_envelope * (m_config.automaticGain ? m_autoGain : 1);
        x *= 1.15 * (levelAfterGain > 0.18 ? std::pow(0.18 / levelAfterGain, 0.75) : 1);
    }
    return bounded(x);
}

float VoiceEffects::noise()
{
    m_random ^= m_random << 13; m_random ^= m_random >> 17; m_random ^= m_random << 5;
    return float(m_random & 0xffff) / 32767.5f - 1;
}

float VoiceEffects::effect(float x)
{
    switch (m_config.effect) {
    case Effect::Robot: {
        const double carrier = std::sin(m_phase);
        m_phase += 2 * pi * 85 / rate;
        if (m_phase >= 2 * pi) m_phase -= 2 * pi;
        return float(x * carrier);
    }
    case Effect::Cave: return reverb(x, true);
    case Effect::Bathroom: return reverb(x, false);
    default: break;
    }
    float filtered = m_low.process(m_high.process(x));
    switch (m_config.effect) {
    case Effect::Radio: return float(std::tanh(filtered * 3) * 0.48);
    case Effect::Telephone: return float(std::tanh(filtered * 1.6) * 0.7);
    case Effect::WalkieTalkie: {
        const double previous = m_squelchEnvelope;
        m_squelchEnvelope += (std::abs(x) > previous ? 0.015 : 0.0004) * (std::abs(x) - previous);
        if ((previous < 0.025 && m_squelchEnvelope >= 0.025)
            || (previous >= 0.008 && m_squelchEnvelope < 0.008))
            m_crackleRemaining = 480;
        const float crackle = m_crackleRemaining > 0 ? noise() * 0.025f * (m_crackleRemaining-- / 480.f) : 0;
        return std::round(float(std::tanh(filtered * 5) * 0.4) * 256) / 256 + crackle;
    }
    case Effect::Intercom: return float(std::tanh(m_presence.process(filtered) * 2.8) * 0.43);
    case Effect::Megaphone: return float(std::tanh(m_presence.process(filtered) * 6) * 0.5);
    default: return x;
    }
}

float VoiceEffects::reverb(float x, bool cave)
{
    constexpr std::array<int, 4> caveLengths{1493, 1867, 2137, 2591};
    constexpr std::array<int, 4> bathroomLengths{431, 613, 809, 1031};
    const auto &lengths = cave ? caveLengths : bathroomLengths;
    const float feedback = cave ? 0.91f : 0.64f;
    float reflected = 0;
    for (int i = 0; i < 4; ++i) {
        const float delayed = m_room[i][m_roomPosition[i]];
        m_roomDamping[i] += (cave ? 0.22f : 0.7f) * (delayed - m_roomDamping[i]);
        m_room[i][m_roomPosition[i]] = x * 0.28f + m_roomDamping[i] * feedback;
        m_roomPosition[i] = (m_roomPosition[i] + 1) % lengths[i];
        reflected += delayed;
    }
    return bounded(x * 0.72 + reflected * (cave ? 0.5 : 0.65));
}

void VoiceEffects::fft(std::array<std::complex<double>, fftSize> &a, bool inverse)
{
    for (int i = 1, j = 0; i < fftSize; ++i) {
        int bit = fftSize >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (int length = 2; length <= fftSize; length <<= 1) {
        const auto step = std::polar(1.0, (inverse ? 2 : -2) * pi / length);
        for (int start = 0; start < fftSize; start += length) {
            std::complex<double> w(1, 0);
            for (int j = 0; j < length / 2; ++j) {
                const auto even = a[start + j], odd = a[start + j + length / 2] * w;
                a[start + j] = even + odd; a[start + j + length / 2] = even - odd;
                w *= step;
            }
        }
    }
    if (inverse) for (auto &value : a) value /= fftSize;
}

float VoiceEffects::spectral(float x)
{
    const float result = m_output[m_position];
    m_output[m_position] = 0;
    m_input[m_position] = x;
    m_position = (m_position + 1) % fftSize;
    if (++m_hop == hopSize) { m_hop = 0; transform(); }
    return result;
}

void VoiceEffects::transform()
{
    for (int i = 0; i < fftSize; ++i) {
        const double window = std::sin(pi * i / fftSize); // sqrt Hann, 4x overlap
        m_spectrum[i] = m_input[(m_position + i) % fftSize] * window;
    }
    fft(m_spectrum, false);
    const bool denoise = m_config.noiseReduction || m_config.studio;
    for (int k = 0; k < bins; ++k) {
        double magnitude = std::abs(m_spectrum[k]);
        if (denoise) {
            auto &floor = m_noiseFloor[k];
            // Track each bin's quiet floor slowly upwards, quickly downwards.
            // A conservative attenuation floor keeps low-level speech intact.
            floor += (magnitude < floor ? 0.15 : 0.0015) * (magnitude - floor);
            const double attenuation = std::clamp(1 - 1.8 * floor / (magnitude + 1e-9), 0.18, 1.0);
            magnitude *= attenuation;
        }
        m_magnitude[k] = magnitude;
        const double phase = std::arg(m_spectrum[k]);
        const double expected = 2 * pi * k * hopSize / fftSize;
        const double delta = std::remainder(phase - m_previousPhase[k] - expected, 2 * pi);
        m_previousPhase[k] = phase;
        m_frequency[k] = (expected + delta) / hopSize;
    }
    const bool deep = m_config.effect == Effect::DeepVoice;
    const bool tiny = m_config.effect == Effect::TinyVoice;
    const bool anonymous = m_config.effect == Effect::Anonymous;
    const bool shift = (deep || tiny || anonymous) && m_config.intensity > 0;
    const double formant = shift ? std::pow(tiny ? 1.38 : anonymous ? 0.76 : 0.72, m_config.intensity) : 1;
    const double pitch = anonymous ? std::pow(0.83, m_config.intensity) : 1;
    if (shift) {
        // Smooth the log spectrum over harmonics to estimate its vocal envelope.
        // Warp only that envelope for Deep/Tiny: their fundamental stays put.
        constexpr int radius = 14;
        double sum = 0;
        for (int k = 0; k < bins; ++k) {
            if (k == 0) {
                for (int j = 0; j <= radius; ++j) sum += std::log(m_magnitude[j] + 1e-6);
            } else {
                if (k - radius - 1 >= 0) sum -= std::log(m_magnitude[k - radius - 1] + 1e-6);
                if (k + radius < bins) sum += std::log(m_magnitude[k + radius] + 1e-6);
            }
            m_envelopeSpectrum[k] = std::exp(sum / (std::min(bins - 1, k + radius) - std::max(0, k - radius) + 1));
        }
    }
    m_shifted.fill(0); m_shiftedFrequency.fill(0);
    for (int k = 0; k < bins; ++k) {
        double magnitude = m_magnitude[k];
        if (shift) {
            const double source = std::min(double(bins - 1), k * pitch / formant);
            const int left = int(source), right = std::min(left + 1, bins - 1);
            const double envelope = std::lerp(m_envelopeSpectrum[left], m_envelopeSpectrum[right], source - left);
            magnitude *= std::clamp(envelope / (m_envelopeSpectrum[k] + 1e-6), 0.15, 5.0);
        }
        if (anonymous && m_config.intensity > 0) {
            const double target = k * pitch;
            const int left = int(target);
            for (int b = left; b <= left + 1 && b < bins; ++b) {
                const double weight = 1 - std::abs(target - b);
                m_shifted[b] += magnitude * weight;
                m_shiftedFrequency[b] += m_frequency[k] * pitch * magnitude * weight;
            }
        } else {
            m_spectrum[k] = std::polar(magnitude, std::arg(m_spectrum[k]));
        }
    }
    if (anonymous && m_config.intensity > 0) {
        for (int k = 0; k < bins; ++k) {
            m_synthesisPhase[k] = std::remainder(m_synthesisPhase[k]
                + hopSize * m_shiftedFrequency[k] / (m_shifted[k] + 1e-9), 2 * pi);
            m_spectrum[k] = std::polar(m_shifted[k], m_synthesisPhase[k]);
        }
    }
    m_spectrum[0] = m_spectrum[0].real();
    m_spectrum[bins - 1] = m_spectrum[bins - 1].real();
    for (int k = 1; k < bins - 1; ++k) m_spectrum[fftSize - k] = std::conj(m_spectrum[k]);
    fft(m_spectrum, true);
    for (int i = 0; i < fftSize; ++i)
        m_output[(m_position + i) % fftSize] += float(m_spectrum[i].real() * std::sin(pi * i / fftSize) * 0.5);
}

} // namespace OpenChat
