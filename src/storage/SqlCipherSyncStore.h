#pragma once

#include "domain/Identifiers.h"
#include "network/SyncEngine.h" // SyncStore

#include <functional>

namespace OpenChat {

class SqlCipherDatabase;

// SQLCipher-backed SyncStore. Every commit* persists all of its rows AND an
// UPSERT of local_mls_state inside a SINGLE database transaction, so the MLS
// ratchet blob becomes durable exactly when — and only when — the message /
// outbox / watermark it belongs to does. A crash between the two can no longer
// leave the ratchet ahead of the store and silently lose a message (finding F3).
//
// The non-atomic query methods (hasSeen / claimDue / markAccepted /
// scheduleRetry / advanceDeliveryState) reuse the exact SQL and lease / backoff
// / monotonic-delivery-state semantics of the existing repositories.
class SqlCipherSyncStore final : public SyncStore
{
public:
    // clock stamps replay/cursor rows for control receives, which carry no
    // message timestamp. Defaults to the system UTC clock.
    using Clock = std::function<qint64()>;

    SqlCipherSyncStore(SqlCipherDatabase &database, ProfileId profileId, Clock clock = {});

    [[nodiscard]] Result<void, RepositoryError>
    commitSend(const MessageRecord &message, const OutboxRecord &outbox,
               QByteArrayView mlsState) override;
    [[nodiscard]] Result<void, RepositoryError>
    commitControlSend(const OutboxRecord &outbox, QByteArrayView mlsState) override;

    [[nodiscard]] Result<bool, RepositoryError>
    commitReceive(const MessageRecord &message, const EnvelopeId &envelopeId, quint64 watermark,
                  QByteArrayView mlsState) override;
    [[nodiscard]] Result<bool, RepositoryError>
    commitControlReceive(const EnvelopeId &envelopeId, const DeviceId &senderDeviceId,
                         quint64 watermark, QByteArrayView mlsState) override;

    [[nodiscard]] Result<bool, RepositoryError> hasSeen(const EnvelopeId &envelopeId) override;
    [[nodiscard]] Result<QVector<OutboxRecord>, RepositoryError>
    claimDue(qint64 nowMs, int limit, qint64 leaseUntilMs) override;
    [[nodiscard]] Result<void, RepositoryError> markAccepted(const EnvelopeId &envelopeId) override;
    [[nodiscard]] Result<void, RepositoryError>
    scheduleRetry(const EnvelopeId &envelopeId, int attemptCount, qint64 nextAttemptMs) override;
    [[nodiscard]] Result<void, RepositoryError>
    advanceDeliveryState(const MessageId &messageId, DeliveryState state) override;

private:
    SqlCipherDatabase &m_database;
    ProfileId m_profileId;
    Clock m_clock;
};

} // namespace OpenChat
