#pragma once

#include "media/AudioTypes.h"
#include "media/WavFile.h"

#include <QByteArray>
#include <QList>
#include <QVector>

namespace OpenChat {

// Conversions between arbitrary PCM (any rate, any channel count) and the single
// CallAudioFormat the call pipeline speaks, plus the level maths the UI's
// speaking indicator is driven from.
//
// Everything here is deterministic and allocation-bounded: the same input always
// produces byte-identical output, which is what lets a call be tested by
// comparing the far end's PCM against the near end's.
namespace AudioConvert {

// Averages interleaved channels down to one. A mono input is returned unchanged
// (no rounding is applied, so a mono round-trip is exact).
[[nodiscard]] QVector<qint16> downmixToMono(const QVector<qint16> &interleaved, int channels);

// Resamples mono S16 with linear interpolation. Equal rates return the input
// unchanged, so a source already at the call rate survives bit-exact.
[[nodiscard]] QVector<qint16> resampleMono(const QVector<qint16> &samples, int fromRate,
                                           int toRate);

// Full conditioning of a decoded file into call-format mono 48 kHz samples.
[[nodiscard]] QVector<qint16> toCallFormat(const WavAudio &audio);

// Splits mono call-rate samples into whole 20 ms frames. A trailing partial
// frame is zero-padded to a full frame so no audio is silently dropped; pass
// padTail=false to discard it instead.
[[nodiscard]] QList<AudioFrame> toFrames(const QVector<qint16> &samples, bool padTail = true);

// Concatenates frames back into a flat sample vector (the inverse of toFrames,
// modulo the tail padding).
[[nodiscard]] QVector<qint16> fromFrames(const QList<AudioFrame> &frames);

// Byte/sample views of a single frame. samplesOf copies; both tolerate a frame
// of the wrong length by using only the whole samples present.
[[nodiscard]] QVector<qint16> samplesOf(const AudioFrame &frame);
[[nodiscard]] AudioFrame frameOf(const QVector<qint16> &samples);

// Root-mean-square amplitude of a frame, normalised to [0, 1]. This is the raw
// level; the speaking indicator smooths it over time rather than using it neat.
[[nodiscard]] double frameRms(const AudioFrame &frame);

// Peak absolute amplitude of a frame, normalised to [0, 1].
[[nodiscard]] double framePeak(const AudioFrame &frame);

} // namespace AudioConvert

} // namespace OpenChat
