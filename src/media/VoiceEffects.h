#pragma once

#include <array>
#include <complex>
#include <span>

namespace OpenChat {

// Mono 48 kHz processing, shared by the test meter and every call recipient.
// All buffers are bounded and allocated with the processor, never per sample.
class VoiceEffects final
{
public:
    enum class Effect {
        None, Radio, WalkieTalkie, Telephone, DeepVoice, TinyVoice,
        Robot, Intercom, Cave, Bathroom, Megaphone, Anonymous
    };
    struct Config {
        Effect effect = Effect::None;
        double intensity = 0.5;
        bool studio = false;
        bool noiseReduction = false;
        bool automaticGain = false;
        bool compressor = false;
    };

    void configure(Config config);
    void reset();
    void process(std::span<float> samples);
    [[nodiscard]] bool enabled() const;
    [[nodiscard]] static bool validEffect(int effect);

private:
    static constexpr int fftSize = 2048;
    static constexpr int hopSize = fftSize / 4;
    static constexpr int bins = fftSize / 2 + 1;
    struct Biquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double z1 = 0, z2 = 0;
        float process(float x);
        void set(bool highPass, double frequency, double q = 0.707);
        void peak(double frequency, double q, double db);
        void reset() { z1 = z2 = 0; }
    };
    static void fft(std::array<std::complex<double>, fftSize> &values, bool inverse);
    float spectral(float x);
    void transform();
    float enhance(float x);
    float effect(float x);
    float reverb(float x, bool cave);
    float noise();

    Config m_config;
    Biquad m_high, m_low, m_presence, m_studioHigh, m_studioPresence, m_deessLow;
    double m_envelope = 0, m_sibilance = 0, m_autoGain = 1;
    double m_phase = 0, m_squelchEnvelope = 0;
    int m_crackleRemaining = 0;
    unsigned m_random = 0x12345678;
    std::array<std::array<float, 4096>, 4> m_room{};
    std::array<int, 4> m_roomPosition{};
    std::array<float, 4> m_roomDamping{};
    std::array<float, fftSize> m_input{}, m_output{};
    std::array<std::complex<double>, fftSize> m_spectrum{};
    std::array<double, bins> m_noiseFloor{}, m_previousPhase{}, m_synthesisPhase{};
    std::array<double, bins> m_magnitude{}, m_envelopeSpectrum{}, m_frequency{}, m_shifted{}, m_shiftedFrequency{};
    int m_position = 0, m_hop = 0;
};

} // namespace OpenChat
