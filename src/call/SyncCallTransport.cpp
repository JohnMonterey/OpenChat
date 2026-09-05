#include "call/SyncCallTransport.h"

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

void SyncCallTransport::sendSignal(const ConversationId &conversation,
                                   const DeviceId &recipientDevice, const QByteArray &payload)
{
    m_engine.sendCallSignal(conversation, recipientDevice, payload);
}

void SyncCallTransport::sendMedia(const ConversationId &conversation,
                                  const DeviceId &recipientDevice, const QByteArray &packet)
{
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
