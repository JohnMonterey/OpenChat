#pragma once

#include "domain/Identifiers.h"

#include <QByteArray>

#include <functional>

namespace OpenChat {

// The two channels a call needs, and the reason they are different channels.
//
// Signalling must arrive: an offer or a hangup that is dropped leaves a call
// stuck, so it goes through the durable, MLS-ratcheted envelope path that
// already retries. Media must arrive *soon*: a 20 ms frame redelivered a second
// later is worse than useless, so it goes through an unreliable datagram path
// that is never stored, never retried, and dropped outright when the peer is
// not connected.
class CallTransport
{
public:
    virtual ~CallTransport() = default;

    // Reliable, encrypted under the conversation's MLS group.
    virtual void sendSignal(const ConversationId &conversation, const DeviceId &recipientDevice,
                            const QByteArray &payload) = 0;

    // Unreliable. `packet` is already sealed under the call's own media key, so
    // this path never carries anything the transport could read.
    virtual void sendMedia(const ConversationId &conversation, const DeviceId &recipientDevice,
                           const QByteArray &packet) = 0;

    // True when media has somewhere to go. Signals may still be queued while
    // this is false; media is simply dropped.
    [[nodiscard]] virtual bool isConnected() const = 0;

    // Installed by the call engine.
    std::function<void(const ConversationId &, const DeviceId &, const QByteArray &)> onSignal;
    std::function<void(const ConversationId &, const DeviceId &, const QByteArray &)> onMedia;
};

} // namespace OpenChat
