#pragma once

#include "core/Result.h"

#include <QByteArray>
#include <QString>
#include <QVector>

#include <cstdint>

namespace OpenChat {

enum class WavError {
    NotFound,      // the path could not be opened for reading
    TooLarge,      // the file exceeded the caller's byte budget
    NotRiffWave,   // missing/garbled RIFF..WAVE container
    MalformedChunk,// a chunk header ran past the end of the file
    MissingFormat, // no fmt chunk before the data chunk
    MissingData,   // no data chunk at all
    Unsupported,   // a sample encoding this reader does not decode
    Empty,         // a well-formed file that carries no sample frames
    WriteFailed,
};

// One decoded PCM file: interleaved signed 16-bit host-order samples plus the
// layout needed to interpret them. Every supported on-disk encoding (8/16/24/32
// bit integer, 32/64 bit float, WAVE_FORMAT_EXTENSIBLE) is normalised to S16 on
// read, so callers downstream deal with a single representation.
struct WavAudio final {
    int sampleRate = 0;
    int channels = 0;
    QVector<qint16> samples; // interleaved, channels-major within each frame

    // Sample frames (one per channel-group), i.e. samples.size() / channels.
    [[nodiscard]] qsizetype frameCount() const noexcept
    {
        return channels > 0 ? samples.size() / channels : 0;
    }
    [[nodiscard]] qint64 durationMs() const noexcept
    {
        return sampleRate > 0 ? frameCount() * 1000 / sampleRate : 0;
    }
};

// A defensive RIFF/WAVE reader and a minimal S16 writer.
//
// The reader is written for untrusted input: every chunk header is bounds
// checked before it is followed, a declared data size larger than the bytes
// actually present is clamped rather than trusted, chunks are walked with the
// RIFF word-alignment padding rule, and a total byte budget caps the allocation
// a hostile header can provoke. It never seeks past the buffer it was handed.
class WavFile final
{
public:
    // 256 MiB: far above any plausible call fixture, far below a size that could
    // exhaust memory on the machines this runs on.
    static constexpr qint64 defaultMaxBytes = 256LL * 1024 * 1024;

    [[nodiscard]] static Result<WavAudio, WavError> readFile(const QString &path,
                                                             qint64 maxBytes = defaultMaxBytes);
    [[nodiscard]] static Result<WavAudio, WavError> decode(const QByteArray &bytes);

    // Serialises interleaved S16 samples as a canonical 44-byte-header PCM WAVE.
    [[nodiscard]] static Result<QByteArray, WavError> encode(const WavAudio &audio);
    [[nodiscard]] static Result<void, WavError> writeFile(const QString &path,
                                                          const WavAudio &audio);
};

} // namespace OpenChat
