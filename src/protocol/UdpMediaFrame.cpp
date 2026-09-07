#include "protocol/UdpMediaFrame.h"

namespace OpenChat {

QByteArray UdpMediaFrame::encode() const
{
    if (senderDeviceId.size() > 255 || recipientDeviceId.size() > 255)
        return {};

    const qsizetype totalSize = 1 + 1 + 1 + senderDeviceId.size() + 1 + recipientDeviceId.size()
        + payload.size();
    if (totalSize > maxDatagramBytes)
        return {};

    QByteArray out;
    out.reserve(totalSize);

    out.append(static_cast<char>(version));
    out.append(static_cast<char>(type));
    out.append(static_cast<char>(static_cast<quint8>(senderDeviceId.size())));
    out.append(senderDeviceId);
    out.append(static_cast<char>(static_cast<quint8>(recipientDeviceId.size())));
    out.append(recipientDeviceId);
    out.append(payload);

    return out;
}

std::optional<UdpMediaFrame> UdpMediaFrame::decode(QByteArrayView bytes)
{
    if (bytes.size() < 4 || bytes.size() > maxDatagramBytes)
        return std::nullopt;

    const auto versionByte = static_cast<quint8>(bytes[0]);
    if (versionByte != currentProtocolVersion)
        return std::nullopt;

    const auto typeByte = static_cast<quint8>(bytes[1]);
    if (typeByte < static_cast<quint8>(Type::Hello) || typeByte > static_cast<quint8>(Type::Bye))
        return std::nullopt;

    const auto senderLen = static_cast<quint8>(bytes[2]);
    if (4 + senderLen > bytes.size())
        return std::nullopt;

    const auto senderBytes = bytes.sliced(3, senderLen);

    const auto recipientLen = static_cast<quint8>(bytes[3 + senderLen]);
    if (4 + senderLen + recipientLen > bytes.size())
        return std::nullopt;

    const auto recipientBytes = bytes.sliced(4 + senderLen, recipientLen);
    const auto payloadBytes = bytes.sliced(4 + senderLen + recipientLen);

    const auto frameType = static_cast<Type>(typeByte);
    if (frameType == Type::Hello && payloadBytes.size() != tokenBytes)
        return std::nullopt;

    UdpMediaFrame frame;
    frame.version = versionByte;
    frame.type = frameType;
    frame.senderDeviceId = senderBytes.toByteArray();
    frame.recipientDeviceId = recipientBytes.toByteArray();
    frame.payload = payloadBytes.toByteArray();

    return frame;
}

UdpMediaFrame UdpMediaFrame::makeHello(const DeviceId &sender, const QByteArray &token)
{
    UdpMediaFrame frame;
    frame.type = Type::Hello;
    frame.senderDeviceId = sender.bytes();
    frame.recipientDeviceId = {};
    frame.payload = token;
    return frame;
}

UdpMediaFrame UdpMediaFrame::makeHelloOk(const DeviceId &recipient)
{
    UdpMediaFrame frame;
    frame.type = Type::HelloOk;
    frame.senderDeviceId = {};
    frame.recipientDeviceId = recipient.bytes();
    frame.payload = {};
    return frame;
}

UdpMediaFrame UdpMediaFrame::makeHelloErr(const DeviceId &recipient, const QByteArray &reason)
{
    UdpMediaFrame frame;
    frame.type = Type::HelloErr;
    frame.senderDeviceId = {};
    frame.recipientDeviceId = recipient.bytes();
    frame.payload = reason;
    return frame;
}

UdpMediaFrame UdpMediaFrame::makeMedia(const DeviceId &sender, const DeviceId &recipient,
                                      const QByteArray &mediaPacket)
{
    UdpMediaFrame frame;
    frame.type = Type::Media;
    frame.senderDeviceId = sender.bytes();
    frame.recipientDeviceId = recipient.bytes();
    frame.payload = mediaPacket;
    return frame;
}

UdpMediaFrame UdpMediaFrame::makePing(const DeviceId &sender, const DeviceId &recipient,
                                     const QByteArray &pingPayload)
{
    UdpMediaFrame frame;
    frame.type = Type::Ping;
    frame.senderDeviceId = sender.bytes();
    frame.recipientDeviceId = recipient.bytes();
    frame.payload = pingPayload;
    return frame;
}

UdpMediaFrame UdpMediaFrame::makePong(const DeviceId &sender, const DeviceId &recipient,
                                     const QByteArray &pongPayload)
{
    UdpMediaFrame frame;
    frame.type = Type::Pong;
    frame.senderDeviceId = sender.bytes();
    frame.recipientDeviceId = recipient.bytes();
    frame.payload = pongPayload;
    return frame;
}

UdpMediaFrame UdpMediaFrame::makeBye(const DeviceId &sender)
{
    UdpMediaFrame frame;
    frame.type = Type::Bye;
    frame.senderDeviceId = sender.bytes();
    frame.recipientDeviceId = {};
    frame.payload = {};
    return frame;
}

UdpMediaFrame UdpMediaFrame::makeBye(const DeviceId &sender, const DeviceId &recipient)
{
    UdpMediaFrame frame;
    frame.type = Type::Bye;
    frame.senderDeviceId = sender.bytes();
    frame.recipientDeviceId = recipient.bytes();
    frame.payload = {};
    return frame;
}

} // namespace OpenChat
