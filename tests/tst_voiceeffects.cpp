#include "media/VoiceEffects.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <vector>

using OpenChat::VoiceEffects;
namespace {
using Effect = VoiceEffects::Effect;
void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
std::vector<float> tone(double frequency, int samples = 48000, double amplitude = 0.2)
{
    std::vector<float> result(samples);
    for (int i = 0; i < samples; ++i)
        result[i] = float(amplitude * std::sin(2 * std::numbers::pi * frequency * i / 48000));
    return result;
}
double rms(std::span<const float> samples)
{
    double sum = 0;
    for (float x : samples) sum += x * x;
    return std::sqrt(sum / samples.size());
}
std::vector<float> render(VoiceEffects::Config config, std::vector<float> input, int chunk = 960)
{
    VoiceEffects processor;
    processor.configure(config);
    for (size_t i = 0; i < input.size(); i += chunk)
        processor.process(std::span(input).subspan(i, std::min(size_t(chunk), input.size() - i)));
    return input;
}
double peakFrequency(std::span<const float> input, int low, int high)
{
    double maximum = 0, best = 0;
    for (int hz = low; hz <= high; ++hz) {
        double re = 0, im = 0;
        for (size_t i = 0; i < input.size(); ++i) {
            const double angle = 2 * std::numbers::pi * hz * i / 48000;
            re += input[i] * std::cos(angle); im += input[i] * std::sin(angle);
        }
        const double power = re * re + im * im;
        if (power > maximum) { maximum = power; best = hz; }
    }
    return best;
}
}

int main()
{
    try {
        const auto reference = tone(200);
        VoiceEffects::Config config;
        require(render(config, reference) == reference, "None must bypass exactly");
        for (int i = 0; i <= int(Effect::Anonymous); ++i) {
            config.effect = static_cast<Effect>(i);
            config.intensity = 0;
            require(render(config, reference) == reference, "0% must bypass exactly");
            config.intensity = 0.7;
            const auto output = render(config, reference);
            const auto fragmented = render(config, reference, 173);
            require(output == fragmented, "Effect depends on audio callback size");
            for (float sample : output)
                require(std::isfinite(sample) && std::abs(sample) <= 1, "Nonfinite/clipped-overflow output");
            if (i > 0) {
                require(output != reference, "Effect did not alter audio");
                require(rms(std::span(output).subspan(24000)) > 0.001, "Effect lost speech");
            }
            auto overload = render(config, tone(900, 48000, 4));
            for (float sample : overload)
                require(i == 0 || (std::isfinite(sample) && std::abs(sample) <= 1), "Overload not bounded");
        }
        for (Effect effect : {Effect::Radio, Effect::WalkieTalkie, Effect::Telephone}) {
            config.effect = effect; config.intensity = 1;
            const auto bass = render(config, tone(60));
            const auto voice = render(config, tone(1000));
            require(rms(std::span(bass).subspan(24000)) < rms(std::span(voice).subspan(24000)) * 0.2,
                    "Speech-band effect did not reject bass");
        }
        // A formant shift changes vocal timbre, not the fundamental frequency.
        for (Effect effect : {Effect::DeepVoice, Effect::TinyVoice, Effect::Anonymous}) {
            config.effect = effect; config.intensity = 1;
            const auto output = render(config, reference);
            const double peak = peakFrequency(std::span(output).subspan(24000), 150, 220);
            require(std::abs(peak - (effect == Effect::Anonymous ? 166 : 200)) <= 2,
                    "Pitch/formant processing shifted the wrong frequency");
        }
        for (Effect effect : {Effect::Cave, Effect::Bathroom}) {
            config.effect = effect; config.intensity = 1;
            std::vector<float> impulse(48000); impulse[0] = 0.5;
            auto output = render(config, impulse);
            require(rms(std::span(output).subspan(4000, 4000)) > 0.00001, "Room has no reflection tail");
            VoiceEffects processor;
            processor.configure(config); processor.process(impulse); processor.reset();
            std::vector<float> silence(48000);
            processor.process(silence);
            require(rms(silence) == 0, "Reset retained private buffered speech");
            processor.process(impulse);
            config.effect = Effect::None; processor.configure(config);
            config.effect = effect; processor.configure(config);
            processor.process(silence);
            require(rms(silence) == 0, "Switching effects retained old speech");
        }
        config = {};
        config.studio = config.noiseReduction = config.automaticGain = config.compressor = true;
        config.effect = Effect::Radio;
        auto enhanced = render(config, reference);
        require(enhanced != render({}, reference) && rms(enhanced) > 0.005, "Enhancement/effect combination failed");
        config = {}; config.effect = Effect::Robot; config.intensity = std::numeric_limits<double>::quiet_NaN();
        for (float sample : render(config, reference)) require(std::isfinite(sample), "NaN configuration escaped");
        config.effect = static_cast<Effect>(999);
        require(render(config, reference) == reference, "Unknown effect must safely bypass");
        std::cout << "PASS: all 11 effects, exact bypass, intensity, callback continuity, speech bands, pitch/formants, room tails, reset, overload and enhancement combinations\n";
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n'; return 1;
    }
}
