#include "domain/SongContainer.h"

#include "domain/Attachment.h"
#include "domain/ProfilePageCodec.h"

#include <QtEndian>

#include <algorithm>
#include <limits>

namespace OpenChat {

namespace {

constexpr char magic[4] = {'O', 'C', 'S', 'G'};
constexpr quint8 containerVersion = 1;
constexpr qsizetype headerBytes = 26;
constexpr qsizetype lengthBytes = 2;

[[nodiscard]] bool isFrameSize(qint64 frameSamples) noexcept
{
    return frameSamples == 960 || frameSamples == 1920 || frameSamples == 2880;
}

// The packets needed to cover the pre-skip and every sample.
[[nodiscard]] qint64 minimumPackets(qint64 totalSamples, qint64 preSkip, qint64 frameSamples) noexcept
{
    return (totalSamples + preSkip + frameSamples - 1) / frameSamples;
}

// At most one packet more than needed: an encoder may flush a final packet
// that holds only its look-ahead. The frame size is checked here too, not
// only by the callers' earlier checks, so no reordering of those can divide
// by a hostile zero.
[[nodiscard]] bool packetCountFits(qint64 count, qint64 totalSamples, qint64 preSkip, qint64 frameSamples) noexcept
{
    if (!isFrameSize(frameSamples))
        return false;
    const qint64 minimum = minimumPackets(totalSamples, preSkip, frameSamples);
    return count >= minimum && count <= minimum + 1;
}

[[nodiscard]] int clampGain(int gainQ8) noexcept
{
    return std::clamp(gainQ8, SongContainer::minGainQ8, 0);
}

template<typename T>
void appendLittleEndian(QByteArray &out, T value)
{
    char bytes[sizeof(T)];
    qToLittleEndian(value, bytes);
    out.append(bytes, sizeof(T));
}

template<typename T>
[[nodiscard]] T readLittleEndian(QByteArrayView bytes, qsizetype offset) noexcept
{
    return qFromLittleEndian<T>(bytes.data() + offset);
}

} // namespace

static_assert(SongContainerLimits{}.maxBytes == maxSongBytes);

SongContainerLimits chatSongLimits()
{
    // Five minutes and the same half-second of slack a profile song gets for
    // the encoder's last frame.
    constexpr qint64 slackSamples = SongContainer::maxTotalSamples
        - qint64(SongContainer::maxDurationMs) * SongContainer::sampleRate / 1000;
    return SongContainerLimits{
        qsizetype(AttachmentLimits::maxAudioBytes),
        AttachmentLimits::maxAudioMs * SongContainer::sampleRate / 1000 + slackSamples,
        AttachmentLimits::maxAudioMs,
    };
}

QByteArray encodeSongContainer(const SongContainer &song)
{
    return encodeSongContainer(song, SongContainerLimits{});
}

QByteArray encodeSongContainer(const SongContainer &song, const SongContainerLimits &limits)
{
    // totalSamples is a u32 on the wire, whatever the limits allow.
    const qint64 maxTotalSamples =
        std::min<qint64>(limits.maxTotalSamples, std::numeric_limits<quint32>::max());
    const bool valid = (song.channels == 1 || song.channels == 2) && isFrameSize(song.frameSamples)
        && song.preSkip >= 0 && song.preSkip <= SongContainer::maxPreSkip && song.totalSamples >= 1
        && song.totalSamples <= maxTotalSamples
        && packetCountFits(song.packets.size(), song.totalSamples, song.preSkip, song.frameSamples)
        && std::all_of(song.packets.cbegin(), song.packets.cend(), [](const QByteArray &packet) {
               return !packet.isEmpty() && packet.size() <= SongContainer::maxPacketBytes;
           });
    if (!valid)
        return {};

    QByteArray out;
    qsizetype size = headerBytes;
    for (const QByteArray &packet : song.packets)
        size += lengthBytes + packet.size();
    out.reserve(size);
    out.append(magic, sizeof(magic));
    appendLittleEndian<quint8>(out, containerVersion);
    appendLittleEndian<quint8>(out, static_cast<quint8>(song.channels));
    appendLittleEndian<quint16>(out, 0);
    appendLittleEndian<quint32>(out, SongContainer::sampleRate);
    appendLittleEndian<quint16>(out, static_cast<quint16>(song.frameSamples));
    appendLittleEndian<quint16>(out, static_cast<quint16>(song.preSkip));
    appendLittleEndian<quint32>(out, static_cast<quint32>(song.totalSamples));
    appendLittleEndian<qint16>(out, static_cast<qint16>(clampGain(song.gainQ8)));
    appendLittleEndian<quint32>(out, static_cast<quint32>(song.packets.size()));
    for (const QByteArray &packet : song.packets) {
        appendLittleEndian<quint16>(out, static_cast<quint16>(packet.size()));
        out.append(packet);
    }
    return out;
}

std::optional<SongContainer> decodeSongContainer(QByteArrayView bytes)
{
    return decodeSongContainer(bytes, SongContainerLimits{});
}

std::optional<SongContainer> decodeSongContainer(QByteArrayView bytes, const SongContainerLimits &limits)
{
    if (bytes.size() < headerBytes || bytes.size() > limits.maxBytes || !looksLikeSongContainer(bytes))
        return std::nullopt;
    if (readLittleEndian<quint8>(bytes, 4) != containerVersion)
        return std::nullopt;
    const quint8 channels = readLittleEndian<quint8>(bytes, 5);
    if (channels != 1 && channels != 2)
        return std::nullopt;
    if (readLittleEndian<quint16>(bytes, 6) != 0)
        return std::nullopt;
    if (readLittleEndian<quint32>(bytes, 8) != quint32(SongContainer::sampleRate))
        return std::nullopt;
    const quint16 frameSamples = readLittleEndian<quint16>(bytes, 12);
    const quint16 preSkip = readLittleEndian<quint16>(bytes, 14);
    const quint32 totalSamples = readLittleEndian<quint32>(bytes, 16);
    const qint16 gainQ8 = readLittleEndian<qint16>(bytes, 20);
    const quint32 packetCount = readLittleEndian<quint32>(bytes, 22);
    if (!isFrameSize(frameSamples) || preSkip > SongContainer::maxPreSkip || totalSamples < 1
        || totalSamples > limits.maxTotalSamples)
        return std::nullopt;
    // Checked before reading any packet, so a forged count cannot make the
    // loop below allocate or walk more than the duration allows.
    if (!packetCountFits(packetCount, totalSamples, preSkip, frameSamples))
        return std::nullopt;

    SongContainer song;
    song.channels = channels;
    song.frameSamples = frameSamples;
    song.preSkip = preSkip;
    song.totalSamples = totalSamples;
    song.gainQ8 = clampGain(gainQ8);
    song.packets.reserve(static_cast<qsizetype>(packetCount));
    qsizetype pos = headerBytes;
    for (quint32 i = 0; i < packetCount; ++i) {
        if (pos + lengthBytes > bytes.size())
            return std::nullopt;
        const quint16 length = readLittleEndian<quint16>(bytes, pos);
        pos += lengthBytes;
        if (length < 1 || length > SongContainer::maxPacketBytes || pos + length > bytes.size())
            return std::nullopt;
        song.packets.push_back(bytes.sliced(pos, length).toByteArray());
        pos += length;
    }
    if (pos != bytes.size())
        return std::nullopt;
    return song;
}

bool looksLikeSongContainer(QByteArrayView bytes)
{
    const QByteArrayView expected(magic, sizeof(magic));
    return bytes.size() >= expected.size() && bytes.first(expected.size()) == expected;
}

} // namespace OpenChat
