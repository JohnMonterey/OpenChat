#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QVector>

#include <optional>

namespace OpenChat {

// One piece of a panel video (docs/profile-panels.md): VP9 frames at a
// constant rate plus, optionally, the Opus audio for the same span, in a
// small little-endian container ("OCCL"). Every piece starts on a keyframe
// and carries its own audio, so it plays alone and a video is just its
// pieces back to back; each fits one PageMedia message (maxClipSegmentBytes).
//
// A contact's clip is hostile input: the decoder checks every field, count
// and length before anything reaches a codec. libvpx and libopus then parse
// the frames and packets themselves (media/ClipCodec.h).
//
//   0  magic "OCCL"          4  version (1)          5  codec (1 = VP9)
//   6  width u16             8  height u16           (16…640 each, even)
//   10 fps u8 (1…30)         11 audioChannels u8 (0 none, 1, 2)
//   12 durationMs u32 (1…8000)
//   16 audioPreSkip u16 (≤ 3840; 0 without audio)
//   18 frameCount u16         20 audioPacketCount u16
//   22 frameCount × (u8 flags: bit 0 keyframe; u32 length ≥ 1; the frame)
//      audioPacketCount × (u16 length 1…1275; the packet), nothing after
//
// Frame i is shown at i × 1000 / fps ms. Audio is 48 kHz Opus in 20 ms
// packets, covering the pre-skip and exactly durationMs of samples.
struct ClipContainer final {
    static constexpr int codecVp9 = 1;
    static constexpr int minDimension = 16;
    static constexpr int maxDimension = 640;
    static constexpr int maxFps = 30;
    static constexpr qint64 maxDurationMs = 8'000;
    static constexpr int audioSampleRate = 48'000;
    static constexpr int audioFrameSamples = 960; // 20 ms
    static constexpr int maxPreSkip = 3840;
    static constexpr int maxPacketBytes = 1275;

    struct Frame final {
        bool key = false;
        QByteArray data;

        friend bool operator==(const Frame &, const Frame &) = default;
    };

    int width = 0, height = 0;
    int fps = 15;
    qint64 durationMs = 0;
    int audioChannels = 0; // 0: silent clip
    int audioPreSkip = 0;
    QVector<Frame> frames;
    QVector<QByteArray> audioPackets;

    [[nodiscard]] qint64 audioSamples() const { return durationMs * audioSampleRate / 1000; }
    // How many frames a clip of durationMs at fps may hold: the frames that
    // start inside it, and at most one more.
    [[nodiscard]] static qint64 maxFramesFor(qint64 durationMs, int fps)
    {
        return (durationMs * fps + 999) / 1000 + 1;
    }

    friend bool operator==(const ClipContainer &, const ClipContainer &) = default;
};

// Serialises `clip`; a clip that breaks any rule above encodes to an empty
// array. The whole-size cap (maxClipSegmentBytes) is the caller's to check.
[[nodiscard]] QByteArray encodeClipContainer(const ClipContainer &clip);
// Rejects anything malformed or larger than maxClipSegmentBytes.
[[nodiscard]] std::optional<ClipContainer> decodeClipContainer(QByteArrayView bytes);
[[nodiscard]] bool looksLikeClipContainer(QByteArrayView bytes);

} // namespace OpenChat
