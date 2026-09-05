#include "storage/SqlCipherSyncStore.h"

#include "storage/RepositorySql.h"
#include "storage/SqlCipherDatabase.h"

#include <QDateTime>

#include <limits>
#include <utility>

namespace OpenChat {
namespace {

using RepositorySql::Statement;

constexpr qsizetype maximumMlsStateSize = qsizetype{8} * 1024 * 1024;

bool fitsSqlInteger(quint64 value)
{
    return value <= static_cast<quint64>(std::numeric_limits<qint64>::max());
}

RepositoryError error(RepositoryErrorCode code, const QString &diagnostic)
{
    return RepositorySql::error(code, diagnostic);
}

RepositoryError internalError(const QString &code)
{
    return error(RepositoryErrorCode::Internal, code);
}

// Translates the most recent SQLite result into a RepositoryError. Read before
// any ROLLBACK so it reflects the statement that actually failed.
RepositoryError mapSqliteError(sqlite3 *database, RepositoryErrorCode fallback, const QString &code)
{
    RepositoryErrorCode mapped = fallback;
    switch (sqlite3_extended_errcode(database) & 0xFF) {
    case SQLITE_FULL:
        mapped = RepositoryErrorCode::DiskFull;
        break;
    case SQLITE_CONSTRAINT:
        mapped = RepositoryErrorCode::Conflict;
        break;
    case SQLITE_CORRUPT:
    case SQLITE_NOTADB:
        mapped = RepositoryErrorCode::IntegrityFailure;
        break;
    case SQLITE_BUSY:
    case SQLITE_LOCKED:
        mapped = RepositoryErrorCode::Locked;
        break;
    case SQLITE_READONLY:
    case SQLITE_CANTOPEN:
    case SQLITE_IOERR:
    case SQLITE_PERM:
        mapped = RepositoryErrorCode::Unavailable;
        break;
    default:
        break;
    }
    return error(mapped, code);
}

bool begin(sqlite3 *database)
{
    return RepositorySql::execute(database, "BEGIN IMMEDIATE;");
}

bool commit(sqlite3 *database)
{
    return RepositorySql::execute(database, "COMMIT;");
}

void rollback(sqlite3 *database)
{
    (void)RepositorySql::execute(database, "ROLLBACK;");
}

// Same schema and binds as SqlCipherChatRepository::insertMessage.
bool insertMessage(sqlite3 *database, const MessageRecord &message)
{
    Statement statement(database,
                        "INSERT INTO messages("
                        "id, conversation_id, sender_device_id, content_kind, content, "
                        "client_created_at_ms, server_sequence, delivery_state, flow, body, "
                        "sent_at_ms, reply_to_id) "
                        "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12)");
    if (!statement.isValid())
        return false;

    const QByteArray body = message.body.toUtf8();
    const bool bound = statement.bindBlob(1, message.id.bytes())
                       && statement.bindBlob(2, message.conversationId.bytes())
                       && statement.bindBlob(3, message.senderDeviceId.bytes())
                       && statement.bindInt(4, static_cast<int>(message.kind))
                       && statement.bindBlob(5, body)
                       && statement.bindInt64(6, message.sentAtMs)
                       && (message.serverSequence
                               ? statement.bindInt64(7, static_cast<qint64>(*message.serverSequence))
                               : statement.bindNull(7))
                       && statement.bindInt(8, static_cast<int>(message.deliveryState))
                       && statement.bindInt(9, static_cast<int>(message.flow))
                       && statement.bindText(10, message.body)
                       && statement.bindInt64(11, message.sentAtMs)
                       && (message.replyToId ? statement.bindBlob(12, message.replyToId->bytes())
                                             : statement.bindNull(12));
    return bound && sqlite3_step(statement.get()) == SQLITE_DONE;
}

// Same schema and binds as SqlCipherChatRepository::saveOutgoing's outbox insert.
bool insertOutbox(sqlite3 *database, const OutboxRecord &outbox)
{
    Statement statement(database,
                        "INSERT INTO outbox(envelope_id, message_id, ciphertext, attempt_count, "
                        "next_attempt_at_ms, state, conversation_id, lease_until_ms) "
                        "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)");
    return statement.isValid()
           && statement.bindBlob(1, outbox.envelopeId.bytes())
           && statement.bindBlob(2, outbox.messageId.bytes())
           && statement.bindBlob(3, outbox.envelope)
           && statement.bindInt(4, outbox.attemptCount)
           && statement.bindInt64(5, outbox.nextAttemptMs)
           && statement.bindInt(6, static_cast<int>(outbox.state))
           && statement.bindBlob(7, outbox.conversationId.bytes())
           && statement.bindInt64(8, outbox.leaseUntilMs)
           && sqlite3_step(statement.get()) == SQLITE_DONE;
}

// Same UPSERT as SqlCipherDatabase::storeMlsState, run as one statement inside
// the caller's transaction so the ratchet state commits atomically with the rest.
bool upsertMlsState(sqlite3 *database, const ProfileId &profileId, QByteArrayView state)
{
    Statement statement(database,
                        "INSERT INTO local_mls_state(profile_id, state_blob) VALUES(?1, ?2) "
                        "ON CONFLICT(profile_id) DO UPDATE SET state_blob = excluded.state_blob");
    return statement.isValid() && statement.bindBlob(1, profileId.bytes())
           && statement.bindBlob(2, state) && sqlite3_step(statement.get()) == SQLITE_DONE;
}

// Same INSERT OR IGNORE as SqlCipherChatRepository::applyIncoming's replay guard.
bool insertReplay(sqlite3 *database, const EnvelopeId &envelopeId, qint64 receivedAtMs,
                  const DeviceId &senderDeviceId)
{
    Statement statement(database,
                        "INSERT OR IGNORE INTO replay_cache("
                        "envelope_id, received_at_ms, sender_device_id) VALUES(?1, ?2, ?3)");
    return statement.isValid() && statement.bindBlob(1, envelopeId.bytes())
           && statement.bindInt64(2, receivedAtMs)
           && statement.bindBlob(3, senderDeviceId.bytes())
           && sqlite3_step(statement.get()) == SQLITE_DONE;
}

// Same monotonic watermark UPSERT as SqlCipherSyncRepository::advanceWatermark.
bool advanceCursor(sqlite3 *database, const DeviceId &deviceId, quint64 watermark,
                   qint64 updatedAtMs)
{
    Statement statement(database,
                        "INSERT INTO device_sync_cursors("
                        "device_id, server_watermark, updated_at_ms) VALUES(?1, ?2, ?3) "
                        "ON CONFLICT(device_id) DO UPDATE SET "
                        "server_watermark=excluded.server_watermark, "
                        "updated_at_ms=excluded.updated_at_ms "
                        "WHERE excluded.server_watermark > device_sync_cursors.server_watermark");
    return statement.isValid() && statement.bindBlob(1, deviceId.bytes())
           && statement.bindInt64(2, static_cast<qint64>(watermark))
           && statement.bindInt64(3, updatedAtMs) && sqlite3_step(statement.get()) == SQLITE_DONE;
}

// Contact state integers, mirrored from SqlCipherContactRepository's encoding.
constexpr int pendingIncomingContactState = 1;
constexpr int acceptedContactState = 2;
constexpr int blockedContactState = 3;

// Snapshot of a contact's state read under the current locked connection. found
// is false for an unknown peer; queryOk is false only on a real query failure.
struct ContactStateRead final {
    bool queryOk = false;
    bool found = false;
    int state = 0;
};

// Reads a single contact's state int for the atomic is-Blocked guard. Reuses the
// same column and encoding as SqlCipherContactRepository::readContactState.
ContactStateRead readContactState(sqlite3 *database, const AccountId &accountId)
{
    Statement statement(database, "SELECT state FROM contacts WHERE account_id=?1");
    if (!statement.isValid() || !statement.bindBlob(1, accountId.bytes()))
        return ContactStateRead{false, false, 0};
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE)
        return ContactStateRead{true, false, 0};
    if (step != SQLITE_ROW)
        return ContactStateRead{false, false, 0};
    return ContactStateRead{true, true, sqlite3_column_int(statement.get(), 0)};
}

// Stash insert. ON CONFLICT(conversation_id) DO NOTHING keeps a redelivered
// Welcome for a group already stashed from erroring; the envelope-level replay
// guard is the primary idempotency mechanism, this is a belt-and-braces on the
// sender-chosen group key.
bool insertPendingHandshake(sqlite3 *database, const ConversationId &conversationId,
                            const AccountId &senderAccountId, const DeviceId &senderDeviceId,
                            const EnvelopeId &envelopeId, QByteArrayView welcome,
                            qint64 receivedAtMs)
{
    Statement statement(database,
                        "INSERT INTO pending_handshakes(conversation_id, sender_account_id, "
                        "sender_device_id, envelope_id, welcome, received_at_ms) "
                        "VALUES(?1, ?2, ?3, ?4, ?5, ?6) "
                        "ON CONFLICT(conversation_id) DO NOTHING");
    return statement.isValid() && statement.bindBlob(1, conversationId.bytes())
           && statement.bindBlob(2, senderAccountId.bytes())
           && statement.bindBlob(3, senderDeviceId.bytes())
           && statement.bindBlob(4, envelopeId.bytes()) && statement.bindBlob(5, welcome)
           && statement.bindInt64(6, receivedAtMs) && sqlite3_step(statement.get()) == SQLITE_DONE;
}

// Defensive decode of a stash row: any bad-length id blob is an integrity fault,
// in the same style as SqlCipherContactRepository::decodeContact.
std::optional<PendingHandshakeRecord> decodePendingHandshake(sqlite3_stmt *statement)
{
    const auto conversationId = ConversationId::fromBytes(RepositorySql::blob(statement, 0));
    const auto senderAccountId = AccountId::fromBytes(RepositorySql::blob(statement, 1));
    const auto senderDeviceId = DeviceId::fromBytes(RepositorySql::blob(statement, 2));
    const auto envelopeId = EnvelopeId::fromBytes(RepositorySql::blob(statement, 3));
    if (!conversationId || !senderAccountId || !senderDeviceId || !envelopeId)
        return std::nullopt;
    return PendingHandshakeRecord{*conversationId,
                                  *senderAccountId,
                                  *senderDeviceId,
                                  *envelopeId,
                                  RepositorySql::blob(statement, 4),
                                  sqlite3_column_int64(statement, 5)};
}

std::optional<OutboxRecord> decodeOutbox(sqlite3_stmt *statement)
{
    const auto envelopeId = EnvelopeId::fromBytes(RepositorySql::blob(statement, 0));
    const auto messageId = MessageId::fromBytes(RepositorySql::blob(statement, 1));
    const auto conversationId = ConversationId::fromBytes(RepositorySql::blob(statement, 2));
    const int state = sqlite3_column_int(statement, 7);
    if (!envelopeId || !messageId || !conversationId
        || state < static_cast<int>(OutboxState::Pending)
        || state > static_cast<int>(OutboxState::Failed))
        return std::nullopt;
    return OutboxRecord{*envelopeId,
                        *messageId,
                        *conversationId,
                        RepositorySql::blob(statement, 3),
                        sqlite3_column_int(statement, 4),
                        sqlite3_column_int64(statement, 5),
                        sqlite3_column_int64(statement, 6),
                        static_cast<OutboxState>(state)};
}

} // namespace

SqlCipherSyncStore::SqlCipherSyncStore(SqlCipherDatabase &database, ProfileId profileId, Clock clock)
    : m_database(database), m_profileId(std::move(profileId)), m_clock(std::move(clock))
{
    if (!m_clock)
        m_clock = [] { return QDateTime::currentMSecsSinceEpoch(); };
}

Result<void, RepositoryError> SqlCipherSyncStore::commitMlsState(QByteArrayView mlsState)
{
    if (mlsState.isEmpty() || mlsState.size() > maximumMlsStateSize)
        return Result<void, RepositoryError>::failure(
            error(RepositoryErrorCode::InvalidInput, QStringLiteral("commitMlsState.invalid")));

    return m_database.withConnection([&](sqlite3 *database) {
        if (!begin(database))
            return Result<void, RepositoryError>::failure(
                internalError(QStringLiteral("commitMlsState.begin")));
        if (!upsertMlsState(database, m_profileId, mlsState)) {
            auto e = mapSqliteError(database, RepositoryErrorCode::Conflict,
                                    QStringLiteral("commitMlsState.mls"));
            rollback(database);
            return Result<void, RepositoryError>::failure(e);
        }
        if (!commit(database)) {
            auto e = mapSqliteError(database, RepositoryErrorCode::Internal,
                                    QStringLiteral("commitMlsState.commit"));
            rollback(database);
            return Result<void, RepositoryError>::failure(e);
        }
        return Result<void, RepositoryError>::success();
    });
}

Result<void, RepositoryError> SqlCipherSyncStore::commitSend(const MessageRecord &message,
                                                             const OutboxRecord &outbox,
                                                             QByteArrayView mlsState)
{
    if (message.flow != MessageFlow::Outgoing || message.id != outbox.messageId
        || message.conversationId != outbox.conversationId || message.serverSequence
        || outbox.envelope.isEmpty() || mlsState.size() > maximumMlsStateSize)
        return Result<void, RepositoryError>::failure(
            error(RepositoryErrorCode::InvalidInput, QStringLiteral("commitSend.invalid")));

    return m_database.withConnection([&](sqlite3 *database) {
        if (!begin(database))
            return Result<void, RepositoryError>::failure(
                internalError(QStringLiteral("commitSend.begin")));
        if (!insertMessage(database, message)) {
            auto e = mapSqliteError(database, RepositoryErrorCode::Conflict,
                                    QStringLiteral("commitSend.message"));
            rollback(database);
            return Result<void, RepositoryError>::failure(e);
        }
        if (!insertOutbox(database, outbox)) {
            auto e = mapSqliteError(database, RepositoryErrorCode::Conflict,
                                    QStringLiteral("commitSend.outbox"));
            rollback(database);
            return Result<void, RepositoryError>::failure(e);
        }
        if (!upsertMlsState(database, m_profileId, mlsState)) {
            auto e = mapSqliteError(database, RepositoryErrorCode::Conflict,
                                    QStringLiteral("commitSend.mls"));
            rollback(database);
            return Result<void, RepositoryError>::failure(e);
        }
        if (!commit(database)) {
            auto e = mapSqliteError(database, RepositoryErrorCode::Internal,
                                    QStringLiteral("commitSend.commit"));
            rollback(database);
            return Result<void, RepositoryError>::failure(e);
        }
        return Result<void, RepositoryError>::success();
    });
}

Result<void, RepositoryError> SqlCipherSyncStore::commitControlSend(const OutboxRecord &outbox,
                                                                    QByteArrayView mlsState)
{
    if (outbox.envelope.isEmpty() || mlsState.size() > maximumMlsStateSize)
        return Result<void, RepositoryError>::failure(
            error(RepositoryErrorCode::InvalidInput, QStringLiteral("commitControlSend.invalid")));

    return m_database.withConnection([&](sqlite3 *database) {
        if (!begin(database))
            return Result<void, RepositoryError>::failure(
                internalError(QStringLiteral("commitControlSend.begin")));
        if (!insertOutbox(database, outbox)) {
            auto e = mapSqliteError(database, RepositoryErrorCode::Conflict,
                                    QStringLiteral("commitControlSend.outbox"));
            rollback(database);
            return Result<void, RepositoryError>::failure(e);
        }
        if (!upsertMlsState(database, m_profileId, mlsState)) {
            auto e = mapSqliteError(database, RepositoryErrorCode::Conflict,
                                    QStringLiteral("commitControlSend.mls"));
            rollback(database);
            return Result<void, RepositoryError>::failure(e);
        }
        if (!commit(database)) {
            auto e = mapSqliteError(database, RepositoryErrorCode::Internal,
                                    QStringLiteral("commitControlSend.commit"));
            rollback(database);
            return Result<void, RepositoryError>::failure(e);
        }
        return Result<void, RepositoryError>::success();
    });
}

Result<bool, RepositoryError> SqlCipherSyncStore::commitReceive(const MessageRecord &message,
                                                                const EnvelopeId &envelopeId,
                                                                quint64 watermark,
                                                                QByteArrayView mlsState)
{
    if (message.flow != MessageFlow::Incoming || !message.serverSequence
        || *message.serverSequence != watermark || !fitsSqlInteger(watermark)
        || mlsState.size() > maximumMlsStateSize)
        return Result<bool, RepositoryError>::failure(
            error(RepositoryErrorCode::InvalidInput, QStringLiteral("commitReceive.invalid")));

    return m_database.withConnection([&](sqlite3 *database) {
        if (!begin(database))
            return Result<bool, RepositoryError>::failure(
                internalError(QStringLiteral("commitReceive.begin")));

        if (!insertReplay(database, envelopeId, message.sentAtMs, message.senderDeviceId)) {
            auto e = mapSqliteError(database, RepositoryErrorCode::Internal,
                                    QStringLiteral("commitReceive.replay"));
            rollback(database);
            return Result<bool, RepositoryError>::failure(e);
        }
        // Already applied: no duplicate row, no watermark regression, and the
        // ratchet state is left exactly as the first delivery committed it.
        if (sqlite3_changes(database) == 0) {
            if (!commit(database)) {
                auto e = mapSqliteError(database, RepositoryErrorCode::Internal,
                                        QStringLiteral("commitReceive.idempotent.commit"));
                rollback(database);
                return Result<bool, RepositoryError>::failure(e);
            }
            return Result<bool, RepositoryError>::success(false);
        }

        if (!insertMessage(database, message)) {
            auto e = mapSqliteError(database, RepositoryErrorCode::Conflict,
                                    QStringLiteral("commitReceive.message"));
            rollback(database);
            return Result<bool, RepositoryError>::failure(e);
        }
        if (!advanceCursor(database, message.senderDeviceId, watermark, message.sentAtMs)) {
            auto e = mapSqliteError(database, RepositoryErrorCode::Internal,
                                    QStringLiteral("commitReceive.cursor"));
            rollback(database);
            return Result<bool, RepositoryError>::failure(e);
        }
        if (!upsertMlsState(database, m_profileId, mlsState)) {
            auto e = mapSqliteError(database, RepositoryErrorCode::Conflict,
                                    QStringLiteral("commitReceive.mls"));
            rollback(database);
            return Result<bool, RepositoryError>::failure(e);
        }
        if (!commit(database)) {
            auto e = mapSqliteError(database, RepositoryErrorCode::Internal,
                                    QStringLiteral("commitReceive.commit"));
            rollback(database);
            return Result<bool, RepositoryError>::failure(e);
        }
        return Result<bool, RepositoryError>::success(true);
    });
}

Result<bool, RepositoryError>
SqlCipherSyncStore::commitControlReceive(const EnvelopeId &envelopeId,
                                         const DeviceId &senderDeviceId, quint64 watermark,
                                         QByteArrayView mlsState)
{
    if (!fitsSqlInteger(watermark) || mlsState.size() > maximumMlsStateSize)
        return Result<bool, RepositoryError>::failure(
            error(RepositoryErrorCode::InvalidInput, QStringLiteral("commitControlReceive.invalid")));

    const qint64 nowMs = m_clock();
    return m_database.withConnection([&](sqlite3 *database) {
        if (!begin(database))
            return Result<bool, RepositoryError>::failure(
                internalError(QStringLiteral("commitControlReceive.begin")));

        if (!insertReplay(database, envelopeId, nowMs, senderDeviceId)) {
            auto e = mapSqliteError(database, RepositoryErrorCode::Internal,
                                    QStringLiteral("commitControlReceive.replay"));
            rollback(database);
            return Result<bool, RepositoryError>::failure(e);
        }
        if (sqlite3_changes(database) == 0) {
            if (!commit(database)) {
                auto e = mapSqliteError(database, RepositoryErrorCode::Internal,
                                        QStringLiteral("commitControlReceive.idempotent.commit"));
                rollback(database);
                return Result<bool, RepositoryError>::failure(e);
            }
            return Result<bool, RepositoryError>::success(false);
        }

        if (!advanceCursor(database, senderDeviceId, watermark, nowMs)) {
            auto e = mapSqliteError(database, RepositoryErrorCode::Internal,
                                    QStringLiteral("commitControlReceive.cursor"));
            rollback(database);
            return Result<bool, RepositoryError>::failure(e);
        }
        if (!upsertMlsState(database, m_profileId, mlsState)) {
            auto e = mapSqliteError(database, RepositoryErrorCode::Conflict,
                                    QStringLiteral("commitControlReceive.mls"));
            rollback(database);
            return Result<bool, RepositoryError>::failure(e);
        }
        if (!commit(database)) {
            auto e = mapSqliteError(database, RepositoryErrorCode::Internal,
                                    QStringLiteral("commitControlReceive.commit"));
            rollback(database);
            return Result<bool, RepositoryError>::failure(e);
        }
        return Result<bool, RepositoryError>::success(true);
    });
}

Result<HandshakeReceiveOutcome, RepositoryError>
SqlCipherSyncStore::commitHandshakeReceive(const EnvelopeId &envelopeId,
                                           const AccountId &senderAccountId,
                                           const DeviceId &senderDeviceId,
                                           const ConversationId &conversationId,
                                           QByteArrayView welcome, qint64 receivedAtMs,
                                           quint64 watermark)
{
    using Ret = Result<HandshakeReceiveOutcome, RepositoryError>;
    if (welcome.isEmpty() || welcome.size() > maximumMlsStateSize || !fitsSqlInteger(watermark))
        return Ret::failure(error(RepositoryErrorCode::InvalidInput,
                                  QStringLiteral("commitHandshakeReceive.invalid")));

    return m_database.withConnection([&](sqlite3 *database) {
        if (!begin(database))
            return Ret::failure(internalError(QStringLiteral("commitHandshakeReceive.begin")));

        if (!insertReplay(database, envelopeId, receivedAtMs, senderDeviceId)) {
            auto e = mapSqliteError(database, RepositoryErrorCode::Internal,
                                    QStringLiteral("commitHandshakeReceive.replay"));
            rollback(database);
            return Ret::failure(e);
        }
        // Idempotent redelivery: the envelope was already consumed. No duplicate
        // stash and no watermark regression -- everything is left as the first
        // delivery committed it.
        if (sqlite3_changes(database) == 0) {
            if (!commit(database)) {
                auto e = mapSqliteError(database, RepositoryErrorCode::Internal,
                                        QStringLiteral("commitHandshakeReceive.idempotent.commit"));
                rollback(database);
                return Ret::failure(e);
            }
            return Ret::success(HandshakeReceiveOutcome::AlreadySeen);
        }

        const auto existing = readContactState(database, senderAccountId);
        if (!existing.queryOk) {
            rollback(database);
            return Ret::failure(internalError(QStringLiteral("commitHandshakeReceive.contact")));
        }
        // A blocked peer can never even leave a stash. The envelope is still
        // consumed (cursor advanced) so the relay stops redelivering it. This is a
        // single durable state read, not the roster state machine: the full
        // no-regress/no-resurrect policy stays in ContactRepository.
        if (existing.found && existing.state == blockedContactState) {
            if (!advanceCursor(database, senderDeviceId, watermark, receivedAtMs)) {
                auto e = mapSqliteError(database, RepositoryErrorCode::Internal,
                                        QStringLiteral("commitHandshakeReceive.blocked.cursor"));
                rollback(database);
                return Ret::failure(e);
            }
            if (!commit(database)) {
                auto e = mapSqliteError(database, RepositoryErrorCode::Internal,
                                        QStringLiteral("commitHandshakeReceive.blocked.commit"));
                rollback(database);
                return Ret::failure(e);
            }
            return Ret::success(HandshakeReceiveOutcome::DroppedBlocked);
        }

        if (!insertPendingHandshake(database, conversationId, senderAccountId, senderDeviceId,
                                    envelopeId, welcome, receivedAtMs)) {
            auto e = mapSqliteError(database, RepositoryErrorCode::Conflict,
                                    QStringLiteral("commitHandshakeReceive.stash"));
            rollback(database);
            return Ret::failure(e);
        }
        if (!advanceCursor(database, senderDeviceId, watermark, receivedAtMs)) {
            auto e = mapSqliteError(database, RepositoryErrorCode::Internal,
                                    QStringLiteral("commitHandshakeReceive.cursor"));
            rollback(database);
            return Ret::failure(e);
        }
        if (!commit(database)) {
            auto e = mapSqliteError(database, RepositoryErrorCode::Internal,
                                    QStringLiteral("commitHandshakeReceive.commit"));
            rollback(database);
            return Ret::failure(e);
        }
        return Ret::success(HandshakeReceiveOutcome::Stashed);
    });
}

Result<void, RepositoryError>
SqlCipherSyncStore::commitHandshakeAccept(const AccountId &accountId,
                                          const ConversationId &conversationId, qint64 updatedAtMs,
                                          QByteArrayView mlsState, QByteArrayView peerSigningKey)
{
    if (mlsState.isEmpty() || mlsState.size() > maximumMlsStateSize)
        return Result<void, RepositoryError>::failure(
            error(RepositoryErrorCode::InvalidInput, QStringLiteral("commitHandshakeAccept.invalid")));

    return m_database.withConnection([&](sqlite3 *database) {
        if (!begin(database))
            return Result<void, RepositoryError>::failure(
                internalError(QStringLiteral("commitHandshakeAccept.begin")));

        if (!upsertMlsState(database, m_profileId, mlsState)) {
            auto e = mapSqliteError(database, RepositoryErrorCode::Conflict,
                                    QStringLiteral("commitHandshakeAccept.mls"));
            rollback(database);
            return Result<void, RepositoryError>::failure(e);
        }

        // The peer signing key is bound INSIDE this guarded, atomic UPDATE so a
        // durable Accepted flip always carries a durable peer key (or NULL when the
        // engine forwarded no authenticated key).
        Statement update(database,
                         "UPDATE contacts SET state=?2, conversation_id=?3, updated_at_ms=?4, "
                         "peer_signing_key=?6 WHERE account_id=?1 AND state=?5");
        if (!update.isValid() || !update.bindBlob(1, accountId.bytes())
            || !update.bindInt(2, acceptedContactState) || !update.bindBlob(3, conversationId.bytes())
            || !update.bindInt64(4, updatedAtMs) || !update.bindInt(5, pendingIncomingContactState)
            || !(peerSigningKey.isEmpty() ? update.bindNull(6) : update.bindBlob(6, peerSigningKey))
            || sqlite3_step(update.get()) != SQLITE_DONE) {
            auto e = mapSqliteError(database, RepositoryErrorCode::Internal,
                                    QStringLiteral("commitHandshakeAccept.update"));
            rollback(database);
            return Result<void, RepositoryError>::failure(e);
        }
        // Only a still-PendingIncoming contact may be accepted. A blocked, already
        // accepted, or absent contact matches no row: roll the WHOLE transaction
        // back so the MLS state is NOT persisted and the stash is left intact -- a
        // mid-flight block can never half-accept.
        if (sqlite3_changes(database) == 0) {
            rollback(database);
            return Result<void, RepositoryError>::failure(
                error(RepositoryErrorCode::Conflict, QStringLiteral("accept.not-pending")));
        }

        Statement remove(database, "DELETE FROM pending_handshakes WHERE conversation_id=?1");
        if (!remove.isValid() || !remove.bindBlob(1, conversationId.bytes())
            || sqlite3_step(remove.get()) != SQLITE_DONE) {
            auto e = mapSqliteError(database, RepositoryErrorCode::Internal,
                                    QStringLiteral("commitHandshakeAccept.delete"));
            rollback(database);
            return Result<void, RepositoryError>::failure(e);
        }
        if (!commit(database)) {
            auto e = mapSqliteError(database, RepositoryErrorCode::Internal,
                                    QStringLiteral("commitHandshakeAccept.commit"));
            rollback(database);
            return Result<void, RepositoryError>::failure(e);
        }
        return Result<void, RepositoryError>::success();
    });
}

Result<std::optional<PendingHandshakeRecord>, RepositoryError>
SqlCipherSyncStore::loadPendingHandshake(const ConversationId &conversationId)
{
    return m_database.withConnection([&](sqlite3 *database) {
        using Ret = Result<std::optional<PendingHandshakeRecord>, RepositoryError>;
        Statement statement(database,
                            "SELECT conversation_id, sender_account_id, sender_device_id, "
                            "envelope_id, welcome, received_at_ms FROM pending_handshakes "
                            "WHERE conversation_id=?1");
        if (!statement.isValid() || !statement.bindBlob(1, conversationId.bytes()))
            return Ret::failure(internalError(QStringLiteral("handshake.load.prepare")));
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE)
            return Ret::success(std::optional<PendingHandshakeRecord>{});
        if (step != SQLITE_ROW)
            return Ret::failure(internalError(QStringLiteral("handshake.load.read")));
        auto decoded = decodePendingHandshake(statement.get());
        if (!decoded)
            return Ret::failure(error(RepositoryErrorCode::IntegrityFailure,
                                      QStringLiteral("handshake.load.decode")));
        return Ret::success(std::optional<PendingHandshakeRecord>(std::move(*decoded)));
    });
}

Result<QVector<PendingHandshakeRecord>, RepositoryError> SqlCipherSyncStore::pendingHandshakes()
{
    return m_database.withConnection([&](sqlite3 *database) {
        using Ret = Result<QVector<PendingHandshakeRecord>, RepositoryError>;
        Statement statement(database,
                            "SELECT conversation_id, sender_account_id, sender_device_id, "
                            "envelope_id, welcome, received_at_ms FROM pending_handshakes "
                            "ORDER BY received_at_ms ASC, conversation_id ASC");
        if (!statement.isValid())
            return Ret::failure(internalError(QStringLiteral("handshake.list.prepare")));
        QVector<PendingHandshakeRecord> records;
        int step = SQLITE_ROW;
        while ((step = sqlite3_step(statement.get())) == SQLITE_ROW) {
            auto decoded = decodePendingHandshake(statement.get());
            if (!decoded)
                return Ret::failure(error(RepositoryErrorCode::IntegrityFailure,
                                          QStringLiteral("handshake.list.decode")));
            records.push_back(std::move(*decoded));
        }
        if (step != SQLITE_DONE)
            return Ret::failure(internalError(QStringLiteral("handshake.list.read")));
        return Ret::success(std::move(records));
    });
}

Result<void, RepositoryError>
SqlCipherSyncStore::deletePendingHandshake(const ConversationId &conversationId)
{
    return m_database.withConnection([&](sqlite3 *database) {
        Statement statement(database, "DELETE FROM pending_handshakes WHERE conversation_id=?1");
        if (!statement.isValid() || !statement.bindBlob(1, conversationId.bytes())
            || sqlite3_step(statement.get()) != SQLITE_DONE)
            return Result<void, RepositoryError>::failure(
                internalError(QStringLiteral("handshake.delete")));
        return Result<void, RepositoryError>::success();
    });
}

Result<bool, RepositoryError> SqlCipherSyncStore::hasSeen(const EnvelopeId &envelopeId)
{
    return m_database.withConnection([&](sqlite3 *database) {
        Statement statement(database, "SELECT 1 FROM replay_cache WHERE envelope_id=?1");
        if (!statement.isValid() || !statement.bindBlob(1, envelopeId.bytes()))
            return Result<bool, RepositoryError>::failure(
                internalError(QStringLiteral("sync.replay.prepare")));
        const int step = sqlite3_step(statement.get());
        if (step != SQLITE_ROW && step != SQLITE_DONE)
            return Result<bool, RepositoryError>::failure(
                internalError(QStringLiteral("sync.replay.read")));
        return Result<bool, RepositoryError>::success(step == SQLITE_ROW);
    });
}

Result<QVector<OutboxRecord>, RepositoryError>
SqlCipherSyncStore::claimDue(qint64 nowMs, int limit, qint64 leaseUntilMs)
{
    if (limit <= 0 || limit > 100 || leaseUntilMs <= nowMs)
        return Result<QVector<OutboxRecord>, RepositoryError>::failure(
            error(RepositoryErrorCode::InvalidInput, QStringLiteral("outbox.claim.input")));

    return m_database.withConnection([&](sqlite3 *database) {
        if (!begin(database))
            return Result<QVector<OutboxRecord>, RepositoryError>::failure(
                internalError(QStringLiteral("outbox.claim.begin")));

        Statement select(database,
                         "SELECT envelope_id, message_id, conversation_id, ciphertext, "
                         "attempt_count, next_attempt_at_ms, lease_until_ms, state "
                         "FROM outbox WHERE "
                         "(state=?1 AND next_attempt_at_ms<=?2) OR "
                         "(state=?3 AND lease_until_ms<=?2) "
                         "ORDER BY next_attempt_at_ms, envelope_id LIMIT ?4");
        if (!select.isValid() || !select.bindInt(1, static_cast<int>(OutboxState::Pending))
            || !select.bindInt64(2, nowMs)
            || !select.bindInt(3, static_cast<int>(OutboxState::Leased))
            || !select.bindInt(4, limit)) {
            rollback(database);
            return Result<QVector<OutboxRecord>, RepositoryError>::failure(
                internalError(QStringLiteral("outbox.claim.prepare")));
        }

        QVector<OutboxRecord> records;
        int step = SQLITE_ROW;
        while ((step = sqlite3_step(select.get())) == SQLITE_ROW) {
            auto record = decodeOutbox(select.get());
            if (!record) {
                rollback(database);
                return Result<QVector<OutboxRecord>, RepositoryError>::failure(
                    error(RepositoryErrorCode::IntegrityFailure,
                          QStringLiteral("outbox.claim.decode")));
            }
            records.append(std::move(*record));
        }
        if (step != SQLITE_DONE) {
            rollback(database);
            return Result<QVector<OutboxRecord>, RepositoryError>::failure(
                internalError(QStringLiteral("outbox.claim.read")));
        }

        for (auto &record : records) {
            Statement update(database,
                             "UPDATE outbox SET state=?1, lease_until_ms=?2 "
                             "WHERE envelope_id=?3");
            if (!update.isValid()
                || !update.bindInt(1, static_cast<int>(OutboxState::Leased))
                || !update.bindInt64(2, leaseUntilMs)
                || !update.bindBlob(3, record.envelopeId.bytes())
                || sqlite3_step(update.get()) != SQLITE_DONE) {
                rollback(database);
                return Result<QVector<OutboxRecord>, RepositoryError>::failure(
                    internalError(QStringLiteral("outbox.claim.lease")));
            }
            record.state = OutboxState::Leased;
            record.leaseUntilMs = leaseUntilMs;
        }

        if (!commit(database)) {
            rollback(database);
            return Result<QVector<OutboxRecord>, RepositoryError>::failure(
                internalError(QStringLiteral("outbox.claim.commit")));
        }
        return Result<QVector<OutboxRecord>, RepositoryError>::success(std::move(records));
    });
}

Result<void, RepositoryError> SqlCipherSyncStore::failSend(const EnvelopeId &envelopeId,
                                                         const MessageId &messageId)
{
    return m_database.withConnection([&](sqlite3 *database) {
        if (!begin(database))
            return Result<void, RepositoryError>::failure(
                internalError(QStringLiteral("send.fail.begin")));
        Statement outbox(database,
                         "UPDATE outbox SET state=3, lease_until_ms=0 "
                         "WHERE envelope_id=?1 AND message_id=?2 AND state IN (0,1)");
        Statement message(database,
                          "UPDATE messages SET delivery_state=6 "
                          "WHERE id=?1 AND delivery_state IN (1,2,6)");
        if (!outbox.isValid() || !outbox.bindBlob(1, envelopeId.bytes())
            || !outbox.bindBlob(2, messageId.bytes()) || sqlite3_step(outbox.get()) != SQLITE_DONE
            || sqlite3_changes(database) != 1
            || !message.isValid() || !message.bindBlob(1, messageId.bytes())
            || sqlite3_step(message.get()) != SQLITE_DONE || !commit(database)) {
            rollback(database);
            return Result<void, RepositoryError>::failure(internalError(QStringLiteral("send.fail")));
        }
        return Result<void, RepositoryError>::success();
    });
}

Result<void, RepositoryError> SqlCipherSyncStore::markAccepted(const EnvelopeId &envelopeId)
{
    return m_database.withConnection([&](sqlite3 *database) {
        Statement statement(database,
                            "UPDATE outbox SET state=?1, lease_until_ms=0 "
                            "WHERE envelope_id=?2");
        if (!statement.isValid()
            || !statement.bindInt(1, static_cast<int>(OutboxState::Accepted))
            || !statement.bindBlob(2, envelopeId.bytes())
            || sqlite3_step(statement.get()) != SQLITE_DONE)
            return Result<void, RepositoryError>::failure(
                internalError(QStringLiteral("outbox.accept")));
        if (sqlite3_changes(database) == 0)
            return Result<void, RepositoryError>::failure(
                error(RepositoryErrorCode::NotFound, QStringLiteral("outbox.accept.missing")));
        return Result<void, RepositoryError>::success();
    });
}

Result<void, RepositoryError> SqlCipherSyncStore::scheduleRetry(const EnvelopeId &envelopeId,
                                                                int attemptCount,
                                                                qint64 nextAttemptMs)
{
    if (attemptCount < 1 || nextAttemptMs < 0)
        return Result<void, RepositoryError>::failure(
            error(RepositoryErrorCode::InvalidInput, QStringLiteral("outbox.retry.input")));
    return m_database.withConnection([&](sqlite3 *database) {
        Statement statement(database,
                            "UPDATE outbox SET state=?1, attempt_count=?2, "
                            "next_attempt_at_ms=?3, lease_until_ms=0 "
                            "WHERE envelope_id=?4 AND state=?5 AND attempt_count<?2");
        if (!statement.isValid()
            || !statement.bindInt(1, static_cast<int>(OutboxState::Pending))
            || !statement.bindInt(2, attemptCount) || !statement.bindInt64(3, nextAttemptMs)
            || !statement.bindBlob(4, envelopeId.bytes())
            || !statement.bindInt(5, static_cast<int>(OutboxState::Leased))
            || sqlite3_step(statement.get()) != SQLITE_DONE)
            return Result<void, RepositoryError>::failure(
                internalError(QStringLiteral("outbox.retry")));
        if (sqlite3_changes(database) == 0)
            return Result<void, RepositoryError>::failure(
                error(RepositoryErrorCode::Conflict,
                      QStringLiteral("outbox.retry.missing-or-accepted")));
        return Result<void, RepositoryError>::success();
    });
}

Result<void, RepositoryError> SqlCipherSyncStore::advanceDeliveryState(const MessageId &messageId,
                                                                       DeliveryState state)
{
    return m_database.withConnection([&](sqlite3 *database) {
        if (!begin(database))
            return Result<void, RepositoryError>::failure(
                internalError(QStringLiteral("delivery.begin")));
        Statement current(database, "SELECT delivery_state FROM messages WHERE id=?1");
        if (!current.isValid() || !current.bindBlob(1, messageId.bytes())) {
            rollback(database);
            return Result<void, RepositoryError>::failure(
                internalError(QStringLiteral("delivery.read")));
        }
        if (sqlite3_step(current.get()) != SQLITE_ROW) {
            rollback(database);
            return Result<void, RepositoryError>::failure(
                error(RepositoryErrorCode::NotFound, QStringLiteral("delivery.missing")));
        }
        const int stored = sqlite3_column_int(current.get(), 0);
        if (stored < static_cast<int>(DeliveryState::Draft)
            || stored > static_cast<int>(DeliveryState::Failed)
            || !canTransition(static_cast<DeliveryState>(stored), state)) {
            rollback(database);
            return Result<void, RepositoryError>::failure(
                error(RepositoryErrorCode::Conflict, QStringLiteral("delivery.backward")));
        }

        Statement update(database, "UPDATE messages SET delivery_state=?1 WHERE id=?2");
        if (!update.isValid() || !update.bindInt(1, static_cast<int>(state))
            || !update.bindBlob(2, messageId.bytes())
            || sqlite3_step(update.get()) != SQLITE_DONE || !commit(database)) {
            rollback(database);
            return Result<void, RepositoryError>::failure(
                internalError(QStringLiteral("delivery.update")));
        }
        return Result<void, RepositoryError>::success();
    });
}

} // namespace OpenChat
