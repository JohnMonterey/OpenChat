#pragma once

#include "domain/ClipContainer.h"
#include "domain/SongContainer.h"
#include "media/WavFile.h"

#include <QByteArray>
#include <QImage>
#include <QSize>
#include <QVector>

#include <functional>
#include <memory>
#include <optional>

struct OpusDecoder;

namespace OpenChat {

// Panel videos (docs/profile-panels.md): VP9 pictures and Opus sound packed
// into ClipContainer segments of five seconds, each within one PageMedia
// message. The sound is ONE Opus stream cut between segments on 20 ms
// packet boundaries (the first segment carries its pre-skip), so playing
// the segments back to back is seamless, and a segment still validates on
// its own. Needs libvpx (OPENCHAT_HAVE_VPX); without it nothing here works
// and clipCodecAvailable() says so.

[[nodiscard]] bool clipCodecAvailable();

struct ClipEncodeOptions final {
    int longSide = 480;            // pictures are scaled so the long side is at most this (even sides)
    int fps = 15;
    int segmentMs = 5'000;         // a multiple of 20 ms
    qint64 maxDurationMs = 30'000; // longer sources are cut here
    // Video bitrates (kbit/s) tried in turn until a segment fits its share.
    QVector<int> videoKbps{380, 300, 240, 190, 150, 115, 85, 60};
    int stereoAudioBps = 40'000;
    int monoAudioBps = 32'000;
    qsizetype maxSegmentBytes = 224 * 1024;
};

// The size pictures are scaled to for `source`: aspect kept, never larger
// than the source, the long side at most `longSide`, both sides even and at
// least ClipContainer::minDimension.
[[nodiscard]] QSize clipFrameSize(QSize source, int longSide);

// The sound of a whole clip as one Opus stream: 48 kHz, 20 ms packets,
// covering the pre-skip and exactly durationMs of samples. `pcm` may be any
// rate and channel count (more than two channels fold into stereo); empty
// gives a silent clip (no packets, 0 channels).
struct ClipAudio final {
    int channels = 0;
    int preSkip = 0;
    QVector<QByteArray> packets;
};
[[nodiscard]] std::optional<ClipAudio> encodeClipAudio(const WavAudio &pcm, qint64 durationMs, int bitrate);

// Encodes one segment's pictures (already at their final size) as VP9,
// starting with a keyframe. Tries each bitrate of `kbps` in turn and keeps
// the first result of at most `maxBytes`; nullopt when none fits or libvpx
// refuses. `cancelled` is polled between frames.
[[nodiscard]] std::optional<QVector<ClipContainer::Frame>>
encodeClipVideo(const QVector<QImage> &frames, int fps, const QVector<int> &kbps, qsizetype maxBytes,
                const std::function<bool()> &cancelled = {});

// Splits `audio` over segments of the given durations (each a multiple of
// 20 ms but the last) and packs every segment. Empty when any segment would
// break the container's rules or exceed maxBytes.
[[nodiscard]] QVector<QByteArray> packClip(const QVector<QVector<ClipContainer::Frame>> &video,
                                           const QVector<qint64> &durationsMs, QSize size, int fps,
                                           const ClipAudio &audio, qsizetype maxBytes);

// The clip's whole sound as one song for SongStream, or nullopt for a
// silent clip (or segments whose sound does not line up).
[[nodiscard]] std::optional<SongContainer> clipSoundtrack(const QVector<ClipContainer> &segments);

// Decodes a clip's pictures in order, one segment after another. A
// contact's clip is hostile input: every segment passed decodeClipContainer
// on arrival, and libvpx parses the frames itself; a frame it rejects is
// skipped (the last good picture stays up).
class ClipVideoDecoder final
{
public:
    ClipVideoDecoder();
    ~ClipVideoDecoder();
    ClipVideoDecoder(const ClipVideoDecoder &) = delete;
    ClipVideoDecoder &operator=(const ClipVideoDecoder &) = delete;

    [[nodiscard]] bool isValid() const noexcept;
    // Decodes one frame; the picture comes back only when `wantPicture`
    // (frames the player skips are decoded but never converted).
    [[nodiscard]] QImage decode(const ClipContainer::Frame &frame, bool wantPicture);
    void reset();

private:
    struct State;
    std::unique_ptr<State> m_state;
};

} // namespace OpenChat
