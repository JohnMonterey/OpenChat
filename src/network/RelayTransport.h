#pragma once

#include "domain/Identifiers.h"
#include "network/SyncEngine.h" // SyncTransport
#include "protocol/CiphertextEnvelope.h"

#include <QObject>

namespace OpenChat {

class RelayClient;

// Adapts the concrete RelayClient to the SyncTransport surface the SyncEngine
// drives. A thin, non-owning forwarding layer: it delegates the outbound calls
// to RelayClient and republishes RelayClient's inbound signals through the
// SyncTransport callbacks the engine installs in start().
//
// The engine owns delivery reliability through its durable outbox, so a
// synchronous send/acknowledge failure (not connected, encode error) is
// intentionally swallowed here and never propagated as an exception; the engine
// retries from the outbox and relay acceptance arrives asynchronously via
// relayAccepted -> onRelayAccepted. This layer weakens none of RelayClient's
// security or validation behaviour.
class RelayTransport final : public QObject, public SyncTransport
{
    Q_OBJECT

public:
    explicit RelayTransport(RelayClient &relay, QObject *parent = nullptr);

    [[nodiscard]] bool isConnected() const override;
    void sendEnvelope(const CiphertextEnvelopeV1 &envelope) override;
    void sendDatagram(const CiphertextEnvelopeV1 &envelope) override;
    void acknowledge(const EnvelopeId &envelopeId, quint64 watermark) override;

private:
    RelayClient &m_relay;
};

} // namespace OpenChat
