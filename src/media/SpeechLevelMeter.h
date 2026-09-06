#pragma once

#include "media/AudioTypes.h"

namespace OpenChat {

// Turns a stream of 20 ms frames into the two numbers the call UI needs: a
// smoothed level to size a ring with, and a stable "is this person talking"
// boolean to colour it with.
//
// Raw per-frame RMS is far too twitchy to drive a visual: speech is full of
// short gaps between syllables, so a bare threshold flickers several times a
// second. Two mechanisms fix that. The level uses fast attack and slow release,
// so it jumps to a new peak immediately but falls away gradually; and the
// speaking flag holds for a hangover period after the level drops, so an
// inter-word pause does not read as "stopped talking".
class SpeechLevelMeter final
{
public:
    struct Config final {
        // RMS above which a frame counts as speech, as a fraction of full scale.
        // 0.02 is roughly -34 dBFS: comfortably above room tone, well below
        // conversational speech.
        double threshold = 0.02;
        // Per-frame multiplier applied while the level is falling. 0.85 over
        // 20 ms frames decays a peak to a tenth in about 280 ms.
        double release = 0.85;
        // Frames the speaking flag is held after the level drops below the
        // threshold. 15 frames is 300 ms — longer than any inter-word gap,
        // shorter than a conversational turn.
        int hangoverFrames = 15;
    };

    // See JitterBuffer: a nested aggregate cannot supply the enclosing class's
    // default argument, so the default is a separate constructor.
    SpeechLevelMeter()
        : SpeechLevelMeter(Config{})
    {
    }
    explicit SpeechLevelMeter(Config config);

    // Swaps the thresholds without disturbing the level or the hangover, so a
    // setting changed mid-call takes effect on the next frame with no glitch in
    // whatever is being drawn from it.
    void setConfig(Config config);
    [[nodiscard]] const Config &config() const noexcept { return m_config; }

    // Feeds one frame. A frame of the wrong length is treated as silence rather
    // than rejected, so a concealment or starvation gap decays the level the same
    // way real quiet does.
    void update(const AudioFrame &frame);

    // Decays as though one frame of silence arrived. Used when a call is muted
    // or has no audio at all, so the indicator falls away instead of freezing.
    void updateSilent();

    [[nodiscard]] double level() const noexcept { return m_level; }
    [[nodiscard]] bool isSpeaking() const noexcept { return m_hangover > 0; }

    void reset();

private:
    void apply(double rms);

    Config m_config;
    double m_level = 0.0;
    int m_hangover = 0;
};

} // namespace OpenChat
