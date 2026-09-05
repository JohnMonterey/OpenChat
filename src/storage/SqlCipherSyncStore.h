#pragma once

#include "domain/Identifiers.h"
#include "network/SyncEngine.h" // SyncStore

#include <QByteArray>
#include <QVector>

#include <functional>
#include <optional>

namespace OpenChat {

class SqlCipherDatabase;

// A stashed inbound contact-handshake Welcome for a group we have not joined.
// Kept out of the roster until the local user accepts; keyed by the
// sender-chosen conversation id.
struct PendingHandshakeRecord final {
    ConversationId conversationId;
    AccountId senderAccountId;
    DeviceId senderDeviceId;
    EnvelopeId envelopeId;
    QByteArray welcome;
    qint64 receivedAtMs = 0;
};

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

    // Persists the MLS ratchet blob ALONE inside a single transaction. Used for
    // MLS mutations that do not flow through the engine's send/receive path (for
    // example KeyPackage generation during account bootstrap), whose captured
    // state would otherwise be dropped on lock(). Same profile-scoped UPSERT of
    // local_mls_state that every commit* performs, run on its own so the private
    // material behind a published KeyPackage becomes durable immediately.
    [[nodiscard]] Result<void, RepositoryError> commitMlsState(QByteArrayView mlsState);

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

    [[nodiscard]] Result<HandshakeReceiveOutcome, RepositoryError>
    commitHandshakeReceive(const EnvelopeId &envelopeId, const AccountId &senderAccountId,
                           const DeviceId &senderDeviceId, const ConversationId &conversationId,
                           QByteArrayView welcome, qint64 receivedAtMs, quint64 watermark) override;
    [[nodiscard]] Result<void, RepositoryError>
    commitHandshakeAccept(const AccountId &accountId, const ConversationId &conversationId,
                          qint64 updatedAtMs, QByteArrayView mlsState,
                          QByteArrayView peerSigningKey) override;

    // Non-virtual reads/deletes over the stash, for the receive service and
    // reconcile paths that surface and clear pending requests. loadPendingHandshake
    // returns std::nullopt for an absent conversation; pendingHandshakes lists all
    // rows in a deterministic order (received_at_ms ASC, then conversation_id ASC).
    [[nodiscard]] Result<std::optional<PendingHandshakeRecord>, RepositoryError>
    loadPendingHandshake(const ConversationId &conversationId);
    [[nodiscard]] Result<QVector<PendingHandshakeRecord>, RepositoryError> pendingHandshakes();
    [[nodiscard]] Result<void, RepositoryError>
    deletePendingHandshake(const ConversationId &conversationId);

    Result<void, RepositoryError> failSend(const EnvelopeId &, const MessageId &) override;

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
