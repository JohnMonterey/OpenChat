#pragma once

#include "call/CallTransport.h"

#include <QObject>

namespace OpenChat {

class SyncEngine;

// Adapts the durable SyncEngine to the two channels a call needs, in the same
// shape RelayTransport adapts RelayClient to the engine: a thin, non-owning
// forwarding layer that adds no policy of its own.
//
// Signals go out through the engine's reliable control-send path and come back
// from callSignalReceived, already decrypted and already checked against the
// MLS sender credential. Media goes out as a datagram and comes back sealed, for
// the call session to open — the engine never holds a call's media key.
class SyncCallTransport final : public QObject, public CallTransport
{
    Q_OBJECT

public:
    explicit SyncCallTransport(SyncEngine &engine, QObject *parent = nullptr);

    void sendSignal(const ConversationId &conversation, const DeviceId &recipientDevice,
                    const QByteArray &payload) override;
    void sendMedia(const ConversationId &conversation, const DeviceId &recipientDevice,
                   const QByteArray &packet) override;
    [[nodiscard]] bool isConnected() const override;

    // Media only flows while the app believes the link is up. The engine does
    // not expose its transport's connectivity, so the app that owns both tells
    // this adapter; until it does, media is sent optimistically (an unreachable
    // frame is dropped harmlessly downstream anyway).
    void setConnected(bool connected);

private:
    SyncEngine &m_engine;
    bool m_connected = true;
};

} // namespace OpenChat
