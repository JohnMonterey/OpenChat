#include "call/SyncCallTransport.h"

#include "call/UdpCallMediaPath.h"
#include "network/SyncEngine.h"

namespace OpenChat {

SyncCallTransport::SyncCallTransport(SyncEngine &engine, QObject *parent)
    : QObject(parent)
    , m_engine(engine)
{
    // Direct connections keep the call's ordering identical to the engine's; a
    // queued hop here would let a hangup overtake the offer it refers to.
    QObject::connect(
        &m_engine, &SyncEngine::callSignalReceived, this,
        [this](const ConversationId &conversation, const DeviceId &sender,
               const QByteArray &payload) {
            if (onSignal)
                onSignal(conversation, sender, payload);
        },
        Qt::DirectConnection);
    QObject::connect(
        &m_engine, &SyncEngine::callMediaReceived, this,
        [this](const ConversationId &conversation, const DeviceId &sender,
               const QByteArray &payload) {
            if (onMedia)
                onMedia(conversation, sender, payload);
        },
        Qt::DirectConnection);
}

void SyncCallTransport::setUdpMediaPath(UdpCallMediaPath *path)
{
    m_udpMediaPath = path;
}

void SyncCallTransport::sendSignal(const ConversationId &conversation,
                                   const DeviceId &recipientDevice, const QByteArray &payload)
{
    m_engine.sendCallSignal(conversation, recipientDevice, payload);
}

void SyncCallTransport::sendMedia(const ConversationId &conversation,
                                  const DeviceId &recipientDevice, const QByteArray &packet)
{
    // Voice only: wire version byte 1, negotiated codec must be Opus so packets stay ≈230 B (<= 1400 B).
    // Camera (v2) and screen (v3) packets — 64–96 KiB — stay on the WS path.
    if (m_udpMediaPath && !packet.isEmpty() && static_cast<quint8>(packet.at(0)) == 1
        && packet.size() <= 1400) {
        if (m_udpMediaPath->sendMedia(recipientDevice, packet))
            return;
    }
    m_engine.sendCallMedia(conversation, recipientDevice, packet);
}

bool SyncCallTransport::isConnected() const
{
    return m_connected;
}

void SyncCallTransport::setConnected(bool connected)
{
    m_connected = connected;
}

} // namespace OpenChat
