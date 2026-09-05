#pragma once

#include "media/AudioTypes.h"

#include <QList>
#include <QVector>

namespace OpenChat {

// A small additive synthesiser for the interface's own sounds.
//
// The call tones are generated rather than shipped as audio files, for the same
// reason the avatars and message bubbles are drawn rather than shipped as
// images: they stay editable in one place, they cost nothing in the binary, and
// they are exactly reproducible, which is what lets a test assert that a ring is
// click-free and that two sounds are actually distinguishable.
namespace ToneSynth {

// One sine component of a segment. Summing a few partials is what turns a bare
// beep into something with a little character — a telephone ringback is two
// tones a semitone-ish apart, and a chime is a fundamental with quieter
// harmonics above it.
struct Partial final {
    double frequency = 440.0;
    double amplitude = 1.0;
};

// A single sounded (or silent) span.
//
// The envelope is the part that matters most: a sine cut off square at a
// non-zero sample is a step discontinuity, and a step is a click. Every segment
// therefore ramps in and out over a raised cosine, so the waveform starts and
// ends at silence no matter what phase the partials are in.
struct Segment final {
    QList<Partial> partials; // empty means silence for the duration
    int durationMs = 0;
    double gain = 1.0;
    int attackMs = 6;
    int releaseMs = 30;
    // Half-life of an exponential decay applied across the segment, giving a
    // struck/plucked character. 0 sustains at full level instead.
    double decayHalfLifeMs = 0.0;
};

// Renders segments back to back into call-format mono samples.
[[nodiscard]] QVector<qint16> render(const QList<Segment> &segments);

// Convenience for the common shape: one enveloped chord.
[[nodiscard]] QVector<qint16> renderSegment(const Segment &segment);

} // namespace ToneSynth

} // namespace OpenChat
