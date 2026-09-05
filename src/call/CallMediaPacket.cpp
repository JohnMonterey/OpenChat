#include "call/CallMediaPacket.h"

#include <QtEndian>

#include <cstring>

namespace OpenChat {

namespace {
constexpr qsizetype callIdOffset = 2;
constexpr qsizetype sequenceOffset = 18;
} // namespace

QByteArray CallMediaPacket::header() const
{
    QByteArray bytes(headerBytes, Qt::Uninitialized);
    auto *cursor = reinterpret_cast<uchar *>(bytes.data());
    cursor[0] = version;
    cursor[1] = flags;
    const QByteArray id = callId.bytes();
    std::memcpy(cursor + callIdOffset, id.constData(), CallId::byteCount);
    qToBigEndian(sequence, cursor + sequenceOffset);
    return bytes;
}

QByteArray CallMediaPacket::encode() const
{
    QByteArray bytes = header();
    bytes.append(sealed);
    return bytes;
}

std::optional<CallMediaPacket> CallMediaPacket::decode(QByteArrayView bytes)
{
    if (bytes.size() <= headerBytes || bytes.size() > maxBytes)
        return std::nullopt;
    const auto *cursor = reinterpret_cast<const uchar *>(bytes.data());
    if (cursor[0] != currentVersion)
        return std::nullopt;
    const std::optional<CallId> callId =
        CallId::fromBytes(bytes.sliced(callIdOffset, CallId::byteCount));
    if (!callId)
        return std::nullopt;

    CallMediaPacket packet;
    packet.version = cursor[0];
    packet.flags = cursor[1];
    packet.callId = *callId;
    packet.sequence = qFromBigEndian<quint32>(cursor + sequenceOffset);
    packet.sealed = bytes.sliced(headerBytes).toByteArray();
    return packet;
}

} // namespace OpenChat
