#include "domain/ClipContainer.h"

#include "domain/ProfilePageCodec.h"

#include <QtEndian>

#include <algorithm>

namespace OpenChat {

namespace {

constexpr char magic[4] = {'O', 'C', 'C', 'L'};
constexpr quint8 containerVersion = 1;
constexpr qsizetype headerBytes = 22;
constexpr qsizetype frameHeaderBytes = 5;
constexpr qsizetype packetLengthBytes = 2;

[[nodiscard]] bool isDimension(qint64 value) noexcept
{
    return value >= ClipContainer::minDimension && value <= ClipContainer::maxDimension && value % 2 == 0;
}

// The 20 ms packets that cover the pre-skip and every sample, and at most
// one more (an encoder may flush a packet holding only its look-ahead).
[[nodiscard]] bool audioPacketCountFits(qint64 count, qint64 samples, qint64 preSkip) noexcept
{
    const qint64 minimum = (samples + preSkip + ClipContainer::audioFrameSamples - 1)
                           / ClipContainer::audioFrameSamples;
    return count >= minimum && count <= minimum + 1;
}

[[nodiscard]] bool headerFits(const ClipContainer &clip) noexcept
{
    if (!isDimension(clip.width) || !isDimension(clip.height) || clip.fps < 1
        || clip.fps > ClipContainer::maxFps || clip.durationMs < 1
        || clip.durationMs > ClipContainer::maxDurationMs)
        return false;
    if (clip.audioChannels < 0 || clip.audioChannels > 2)
        return false;
    if (clip.audioChannels == 0)
        return clip.audioPreSkip == 0 && clip.audioPackets.isEmpty();
    return clip.audioPreSkip >= 0 && clip.audioPreSkip <= ClipContainer::maxPreSkip
        && audioPacketCountFits(clip.audioPackets.size(), clip.audioSamples(), clip.audioPreSkip);
}

[[nodiscard]] bool framesFit(const ClipContainer &clip) noexcept
{
    const qsizetype count = clip.frames.size();
    return count >= 1 && count <= ClipContainer::maxFramesFor(clip.durationMs, clip.fps)
        && clip.frames.first().key;
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

QByteArray encodeClipContainer(const ClipContainer &clip)
{
    const bool valid = headerFits(clip) && framesFit(clip)
        && std::all_of(clip.frames.cbegin(), clip.frames.cend(),
                       [](const ClipContainer::Frame &frame) { return !frame.data.isEmpty(); })
        && std::all_of(clip.audioPackets.cbegin(), clip.audioPackets.cend(), [](const QByteArray &packet) {
               return !packet.isEmpty() && packet.size() <= ClipContainer::maxPacketBytes;
           });
    if (!valid)
        return {};

    QByteArray out;
    out.append(magic, sizeof(magic));
    appendLittleEndian<quint8>(out, containerVersion);
    appendLittleEndian<quint8>(out, ClipContainer::codecVp9);
    appendLittleEndian<quint16>(out, quint16(clip.width));
    appendLittleEndian<quint16>(out, quint16(clip.height));
    appendLittleEndian<quint8>(out, quint8(clip.fps));
    appendLittleEndian<quint8>(out, quint8(clip.audioChannels));
    appendLittleEndian<quint32>(out, quint32(clip.durationMs));
    appendLittleEndian<quint16>(out, quint16(clip.audioPreSkip));
    appendLittleEndian<quint16>(out, quint16(clip.frames.size()));
    appendLittleEndian<quint16>(out, quint16(clip.audioPackets.size()));
    for (const ClipContainer::Frame &frame : clip.frames) {
        appendLittleEndian<quint8>(out, frame.key ? 1 : 0);
        appendLittleEndian<quint32>(out, quint32(frame.data.size()));
        out.append(frame.data);
    }
    for (const QByteArray &packet : clip.audioPackets) {
        appendLittleEndian<quint16>(out, quint16(packet.size()));
        out.append(packet);
    }
    return out;
}

std::optional<ClipContainer> decodeClipContainer(QByteArrayView bytes)
{
    if (bytes.size() < headerBytes || bytes.size() > maxClipSegmentBytes || !looksLikeClipContainer(bytes))
        return std::nullopt;
    if (readLittleEndian<quint8>(bytes, 4) != containerVersion
        || readLittleEndian<quint8>(bytes, 5) != ClipContainer::codecVp9)
        return std::nullopt;

    ClipContainer clip;
    clip.width = readLittleEndian<quint16>(bytes, 6);
    clip.height = readLittleEndian<quint16>(bytes, 8);
    clip.fps = readLittleEndian<quint8>(bytes, 10);
    clip.audioChannels = readLittleEndian<quint8>(bytes, 11);
    clip.durationMs = readLittleEndian<quint32>(bytes, 12);
    clip.audioPreSkip = readLittleEndian<quint16>(bytes, 16);
    const quint16 frameCount = readLittleEndian<quint16>(bytes, 18);
    const quint16 packetCount = readLittleEndian<quint16>(bytes, 20);
    // Every count is checked against the header before any loop runs, so a
    // forged count cannot make the reader allocate or walk further.
    if (frameCount < 1 || frameCount > ClipContainer::maxFramesFor(clip.durationMs, clip.fps))
        return std::nullopt;
    if (clip.audioChannels == 0 && packetCount != 0)
        return std::nullopt;
    if (clip.audioChannels != 0
        && !audioPacketCountFits(packetCount, clip.audioSamples(), clip.audioPreSkip))
        return std::nullopt;
    // headerFits() below repeats the header rules on the parsed clip, with
    // the audio counts now known.

    qsizetype pos = headerBytes;
    clip.frames.reserve(frameCount);
    for (quint16 i = 0; i < frameCount; ++i) {
        if (pos + frameHeaderBytes > bytes.size())
            return std::nullopt;
        const quint8 flags = readLittleEndian<quint8>(bytes, pos);
        const quint32 length = readLittleEndian<quint32>(bytes, pos + 1);
        pos += frameHeaderBytes;
        if (flags > 1 || length < 1 || qint64(length) > bytes.size() - pos)
            return std::nullopt;
        clip.frames.push_back({flags == 1, bytes.sliced(pos, length).toByteArray()});
        pos += length;
    }
    clip.audioPackets.reserve(packetCount);
    for (quint16 i = 0; i < packetCount; ++i) {
        if (pos + packetLengthBytes > bytes.size())
            return std::nullopt;
        const quint16 length = readLittleEndian<quint16>(bytes, pos);
        pos += packetLengthBytes;
        if (length < 1 || length > ClipContainer::maxPacketBytes || pos + length > bytes.size())
            return std::nullopt;
        clip.audioPackets.push_back(bytes.sliced(pos, length).toByteArray());
        pos += length;
    }
    if (pos != bytes.size() || !headerFits(clip) || !framesFit(clip))
        return std::nullopt;
    return clip;
}

bool looksLikeClipContainer(QByteArrayView bytes)
{
    return bytes.size() >= qsizetype(sizeof(magic)) && std::equal(magic, magic + sizeof(magic), bytes.data());
}

} // namespace OpenChat
