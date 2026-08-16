#include "network/RelayTransport.h"

#include "network/RelayClient.h"

namespace OpenChat {

RelayTransport::RelayTransport(RelayClient &relay, QObject *parent)
    : QObject(parent), m_relay(relay)
{
    // Republish RelayClient's inbound signals through the SyncTransport
    // callbacks. Each callback is null until the engine installs it in start(),
    // so every forward is guarded. Direct connections keep the engine's
    // single-threaded ordering (no queueing across the relay boundary).
    QObject::connect(
        &m_relay, &RelayClient::envelopeReceived, this,
        [this](const CiphertextEnvelopeV1 &envelope, quint64 serverSequence) {
            if (onEnvelope)
                onEnvelope(envelope, serverSequence);
        },
        Qt::DirectConnection);
    QObject::connect(
        &m_relay, &RelayClient::relayAccepted, this,
        [this](const EnvelopeId &envelopeId, quint64 serverSequence) {
            if (onRelayAccepted)
                onRelayAccepted(envelopeId, serverSequence);
        },
        Qt::DirectConnection);
    QObject::connect(
        &m_relay, &RelayClient::connected, this,
        [this]() {
            if (onConnected)
                onConnected();
        },
        Qt::DirectConnection);
}

bool RelayTransport::isConnected() const
{
    return m_relay.isConnected();
}

void RelayTransport::sendEnvelope(const CiphertextEnvelopeV1 &envelope)
{
    // Swallow the synchronous not-connected / encode error: the engine resends
    // from the durable outbox and acceptance is reported asynchronously.
    (void)m_relay.sendEnvelope(envelope);
}

void RelayTransport::acknowledge(const EnvelopeId &envelopeId, quint64 watermark)
{
    (void)m_relay.acknowledge(envelopeId, watermark);
}

} // namespace OpenChat
