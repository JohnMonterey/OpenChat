#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QVector>

#include <optional>

namespace OpenChat {

// A profile song: Opus packets at 48 kHz in a small little-endian container
// ("OCSG"), so a page carries its song as one bounded blob without an Ogg
// parser on the receiving side. A contact's song is hostile input, so the
// decoder checks every field and every packet length before anything
// touches the codec; SongDecoder (media/SongCodec.h) then checks the packets
// themselves.
//
//   0  magic "OCSG"          4  version (1)        5  channels (1 or 2)
//   6  reserved u16 (0)      8  sampleRate u32 (48 000)
//   12 frameSamples u16 (960, 1920 or 2880)       14 preSkip u16 (≤ 3840)
//   16 totalSamples u32 (per channel, after pre-skip; 1 … 2 184 000)
//   20 gainQ8 i16 (1/256 dB, attenuation only)    22 packetCount u32
//   26 packetCount × (u16 length 1…1275, then the packet), nothing after
struct SongContainer final {
    static constexpr int sampleRate = 48'000;
    static constexpr int maxDurationMs = 45'000;          // what the encoder cuts to
    static constexpr qint64 maxTotalSamples = 2'184'000;  // 45.5 s: slack for the encoder's last frame
    static constexpr int maxPreSkip = 3840;
    static constexpr int maxPacketBytes = 1275;           // Opus's own limit for one frame
    static constexpr int minGainQ8 = -24 * 256;           // −24 dB; gains above 0 dB are never applied

    int channels = 1;
    int frameSamples = 2880; // 60 ms
    int preSkip = 0;
    qint64 totalSamples = 0;
    int gainQ8 = 0;
    QVector<QByteArray> packets;

    [[nodiscard]] qint64 durationMs() const { return totalSamples * 1000 / sampleRate; }

    friend bool operator==(const SongContainer &, const SongContainer &) = default;
};

// Serialises `song`, clamping gainQ8 to attenuation as the decoder does. A
// song that breaks any other rule above encodes to an empty array, so a
// malformed container never leaves this process. The whole-size cap
// (maxSongBytes) is the caller's budget to check.
[[nodiscard]] QByteArray encodeSongContainer(const SongContainer &song);
// Rejects anything malformed or larger than maxSongBytes; gainQ8 is clamped
// to [−24 dB, 0], never rejected.
[[nodiscard]] std::optional<SongContainer> decodeSongContainer(QByteArrayView bytes);
// Checks the magic only: enough to tell a song from other bytes, not to trust it.
[[nodiscard]] bool looksLikeSongContainer(QByteArrayView bytes);

} // namespace OpenChat
