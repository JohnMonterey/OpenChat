#pragma once

#include "media/AudioTypes.h"
#include "media/SpeechLevelMeter.h"

namespace OpenChat {

// What happens to the microphone between the device and the call: a gain, then
// a noise gate. Runs once per captured frame, before the frame is encoded for
// anyone, so every peer in a group call hears the same thing.
//
// The gate is why a call does not carry the room. A microphone never captures
// digital silence — there is always fan noise, keyboard clatter, the neighbour's
// television — and without a gate every bit of it is encoded, encrypted, sent
// and played to the far end for the whole call. Gating replaces the frames that
// carry no speech with real silence, which the far end's concealment and level
// meter already treat as "not talking".
//
// Opening is instant (the frame that crosses the threshold is itself passed)
// so no syllable onset is lost, and closing waits a hold period so the pauses
// inside a sentence do not chop it up. The last frame before the gate closes
// is faded out rather than cut, because the frame that closes the gate is
// quiet but not silent, and a hard edge on it would be heard as a click.
class MicrophoneProcessor final
{
public:
    struct Config final {
        // Linear multiplier on every sample. 1.0 leaves the signal untouched
        // (and the bytes bit-identical); values above 1 clip at full scale.
        double gain = 1.0;
        // Whether frames below the threshold are replaced with silence at all.
        // Off means the room goes out continuously, which is what a caller
        // with a very quiet voice or a very good microphone may prefer.
        bool gateEnabled = true;
        // RMS, as a fraction of full scale after gain, at or above which a
        // frame opens the gate. 0.02 is about -34 dBFS: above room tone,
        // below quiet speech.
        double gateThreshold = 0.02;
        // Frames the gate stays open after the level last crossed the
        // threshold. 15 frames is 300 ms, the same hangover the speaking ring
        // uses and longer than any gap between words.
        int gateHoldFrames = 15;
    };

    MicrophoneProcessor()
        : MicrophoneProcessor(Config{})
    {
    }
    explicit MicrophoneProcessor(Config config);

    // Takes effect on the next frame; keeps the level and gate state.
    void setConfig(Config config);
    [[nodiscard]] const Config &config() const noexcept { return m_config; }

    // Returns the frame to send: the input with gain applied, or silence when
    // the gate is closed. A frame of the wrong length is passed through
    // untouched, because nothing downstream will accept it anyway.
    [[nodiscard]] AudioFrame process(const AudioFrame &frame);

    // What the last frame measured, after gain — what a settings meter shows.
    [[nodiscard]] double level() const noexcept { return m_meter.level(); }
    // True while frames are being let through. Always true with the gate off.
    [[nodiscard]] bool isGateOpen() const noexcept { return m_open; }

    void reset();

private:
    [[nodiscard]] static AudioFrame applyGain(const AudioFrame &frame, double gain);
    [[nodiscard]] static AudioFrame fadeOut(const AudioFrame &frame);

    Config m_config;
    SpeechLevelMeter m_meter;
    bool m_open = true;
};

} // namespace OpenChat
