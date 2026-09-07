#pragma once

#include "media/AudioTypes.h"

#include <QList>
#include <QVector>

namespace OpenChat {

// A small additive synthesiser, kept around for anything that still wants a
// generated tone rather than a shipped audio file: exactly reproducible, and
// cheap to reason about in a test.
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
