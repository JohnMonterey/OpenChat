#pragma once

#include "core/Result.h"
#include "domain/ProfilePageCodec.h"
#include "domain/SongContainer.h"
#include "media/WavFile.h"

#include <QByteArray>
#include <QVector>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

struct OpusDecoder;

namespace OpenChat {

// Turns a clip of PCM into a profile song (an Opus container, SongContainer.h)
// and plays one back. A profile song crosses the wire as one media blob of at
// most maxSongBytes, so the encoder owns the budget: it measures and sets the
// loudness itself, then walks a bitrate ladder until the song fits. A chat's
// audio attachment is the same container held to longer bounds
// (chatSongEncodeOptions).

struct SongEncodeOptions final {
    int maxDurationMs = SongContainer::maxDurationMs;         // longer clips are cut (and faded) here
    QVector<int> stereoBitrates{40'000, 36'000, 32'000, 24'000}; // first result that fits wins
    QVector<int> monoBitrates{32'000, 24'000};
    // A stereo clip that fits at no stereo rung is folded to mono and tried
    // on the mono ladder before the encode gives up as TooLarge.
    bool monoFallback = false;
    double targetLoudnessDb = -16.0; // gated RMS of the mid signal, dBFS
    double peakCeilingDb = -1.0;     // no sample above this after the gain
    double maxGainDb = 12.0;
    double minGainDb = -20.0;
    // A clip whose loudest 400 ms block stays under this is refused as Silent;
    // it is also what firstAudibleMs() listens for.
    double silenceThresholdDb = -50.0;
    int minDurationMs = 1'000;
    int edgeFadeMs = 10;  // where the clip is the song's own start or end: just enough to avoid a click
    int trimFadeMs = 500; // where the chosen window cuts through the song
    qsizetype maxBytes = maxSongBytes;
    // What the container may hold: maxDurationMs is capped to its length and
    // maxBytes to its size, and the result must decode under the same limits.
    SongContainerLimits limits;
};

// A chat's audio attachment (AttachmentLimits): up to five minutes within
// maxAudioBytes, at higher rungs than a profile song can afford (down to the
// same 24 kbit/s, then mono). Voice notes are short and quiet, so 300 ms
// passes, only true silence (-70 dB) is refused, and a quiet recording is
// lifted by at most 6 dB rather than a song's 12.
[[nodiscard]] SongEncodeOptions chatSongEncodeOptions();

struct SongClip final {
    WavAudio pcm;              // any rate, any channel count
    bool trimmedStart = false; // audible music precedes the clip in the source
    bool trimmedEnd = false;   // audible music follows the clip in the source
};

enum class SongEncodeError {
    Silent,             // nothing reached SongEncodeOptions::silenceThresholdDb
    TooShort,           // under SongEncodeOptions::minDurationMs
    EncoderUnavailable, // libopus refused to create or run an encoder
    TooLarge,           // over maxBytes even at the ladder's last rung
    Cancelled,
};

// Any rate and channel count in; 48 kHz out. One channel stays mono; two or
// more become stereo (a third channel and beyond fold into both sides as a
// centre would). The gain is applied here, so the container's gainQ8 is 0.
//
// CPU-bound (up to a few seconds for 45 s of stereo): call it on a worker
// thread. `cancelled` is polled between Opus packets; `progress` (0…1, on
// the calling thread) hears how much of the song is encoded, in steps of at
// least a hundredth and never going back when a rung is abandoned for the
// next.
[[nodiscard]] Result<QByteArray, SongEncodeError>
encodeSong(const SongClip &clip, const SongEncodeOptions &options = {},
           const std::function<bool()> &cancelled = {}, const std::function<void(qreal)> &progress = {});

// The time, in ms from the start of `pcm`, of the first 10 ms block within
// frames [fromFrame, toFrame) whose RMS over every channel reaches
// silenceThresholdDb; nullopt when none does. toFrame < 0 means the end.
[[nodiscard]] std::optional<qint64> findAudibleMs(const WavAudio &pcm, double silenceThresholdDb = -50.0,
                                                  qsizetype fromFrame = 0, qsizetype toFrame = -1);
// Where the song editor's 45 s window starts by default: the first audible
// moment, or 0 for a clip that never gets there.
[[nodiscard]] qint64 firstAudibleMs(const WavAudio &pcm, double silenceThresholdDb = -50.0);

// Streams a song one Opus packet at a time, so playback never holds the
// whole song as PCM.
//
// A contact's song is hostile input. Construction checks every packet's
// table-of-contents: each must hold exactly frameSamples (the last may hold
// fewer), and together they must cover the pre-skip and every sample, or
// isValid() is false and nothing is ever decoded. A packet that still fails
// to decode mid-stream is replaced by Opus's own loss concealment; the
// stream keeps its length and never aborts.
class SongDecoder final
{
public:
    // Opus's longest packet (120 ms at 48 kHz); the decode buffer's size.
    static constexpr int maxPacketSamples = 5760;

    // `limits` bounds the song's length: a profile song's by default, a
    // chat attachment's when the caller says so (chatSongLimits).
    explicit SongDecoder(SongContainer song, const SongContainerLimits &limits = {});
    ~SongDecoder();

    SongDecoder(const SongDecoder &) = delete;
    SongDecoder &operator=(const SongDecoder &) = delete;

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] int channels() const noexcept;
    [[nodiscard]] qint64 totalSamples() const noexcept; // per channel
    // The per-channel sample the next call to next() starts at.
    [[nodiscard]] qint64 position() const noexcept;
    // The next packet's worth of interleaved S16 at 48 kHz, with the pre-skip
    // and the tail past totalSamples cut and gainQ8 applied; empty at the end.
    [[nodiscard]] QVector<qint16> next();
    [[nodiscard]] bool atEnd() const noexcept;
    // Restarts from the packet before the one holding `sample`: decoding that
    // extra packet (and dropping it) lets the decoder's state settle, so the
    // first samples after a seek are clean.
    void seekToSample(qint64 sample);

private:
    struct OpusDecoderDeleter final {
        void operator()(OpusDecoder *decoder) const noexcept;
    };

    SongContainer m_song;
    std::unique_ptr<OpusDecoder, OpusDecoderDeleter> m_decoder;
    std::vector<int> m_packetSamples; // each packet's duration, read from its TOC
    std::vector<float> m_buffer;      // maxPacketSamples × channels
    float m_gain = 1.0F;
    qsizetype m_nextPacket = 0;
    qint64 m_skip = 0;     // decoder output still to drop: the pre-skip, or a seek's pre-roll
    qint64 m_position = 0; // song samples returned so far (from the last seek)
    bool m_valid = false;
};

} // namespace OpenChat
