#pragma once

#include "domain/Identifiers.h"

#include <QByteArray>
#include <QByteArrayView>

#include <cstdint>
#include <optional>

namespace OpenChat {

// Wire format for relay-assisted UDP voice media.
//
// Frame = [proto u8=1][type u8][senderDeviceId (len-prefixed)][recipientDeviceId (len-prefixed)][payload]
//
// Types:
//   Hello (1): 32-byte token payload, pins senderDeviceId -> (addr, port)
//   HelloOk (2): confirmation from relay to sender
//   HelloErr (3): rejection from relay to sender (bad token, expired, etc.)
//   Media (4): E2E encrypted CallMediaPacket payload, forwarded verbatim to recipient
//   Ping (5): probe/keepalive routed to recipient like Media, carrying RTT timestamp
//   Pong (6): reply to Ping carrying echoed timestamp, routed like Media
//   Bye (7): clears sender binding and notifies peer call media has ended
struct UdpMediaFrame final {
    static constexpr quint8 currentProtocolVersion = 1;
    static constexpr qsizetype maxDatagramBytes = 1400;
    static constexpr qsizetype tokenBytes = 32;

    enum class Type : quint8 {
        Hello = 1,
        HelloOk = 2,
        HelloErr = 3,
        Media = 4,
        Ping = 5,
        Pong = 6,
        Bye = 7,
    };

    quint8 version = currentProtocolVersion;
    Type type = Type::Media;
    QByteArray senderDeviceId;
    QByteArray recipientDeviceId;
    QByteArray payload;

    [[nodiscard]] std::optional<DeviceId> senderId() const
    {
        return DeviceId::fromBytes(senderDeviceId);
    }

    [[nodiscard]] std::optional<DeviceId> recipientId() const
    {
        return DeviceId::fromBytes(recipientDeviceId);
    }

    [[nodiscard]] QByteArray encode() const;
    [[nodiscard]] static std::optional<UdpMediaFrame> decode(QByteArrayView bytes);

    static UdpMediaFrame makeHello(const DeviceId &sender, const QByteArray &token);
    static UdpMediaFrame makeHelloOk(const DeviceId &recipient);
    static UdpMediaFrame makeHelloErr(const DeviceId &recipient, const QByteArray &reason = {});
    static UdpMediaFrame makeMedia(const DeviceId &sender, const DeviceId &recipient,
                                  const QByteArray &mediaPacket);
    static UdpMediaFrame makePing(const DeviceId &sender, const DeviceId &recipient,
                                 const QByteArray &pingPayload = {});
    static UdpMediaFrame makePong(const DeviceId &sender, const DeviceId &recipient,
                                 const QByteArray &pongPayload = {});
    static UdpMediaFrame makeBye(const DeviceId &sender);
    static UdpMediaFrame makeBye(const DeviceId &sender, const DeviceId &recipient);
};

} // namespace OpenChat
