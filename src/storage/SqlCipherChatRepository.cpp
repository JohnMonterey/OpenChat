#include "storage/SqlCipherChatRepository.h"

#include "storage/RepositorySql.h"
#include "storage/SqlCipherDatabase.h"

#include <limits>

namespace OpenChat {
namespace {

using RepositorySql::Statement;

RepositoryError internalError(const QString &code)
{
    return RepositorySql::error(RepositoryErrorCode::Internal, code);
}

RepositoryError conflictError(const QString &code)
{
    return RepositorySql::error(RepositoryErrorCode::Conflict, code);
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

bool fitsSqlInteger(quint64 value)
{
    return value <= static_cast<quint64>(std::numeric_limits<qint64>::max());
}

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

std::optional<MessageRecord> decodeMessage(sqlite3_stmt *statement)
{
    const auto id = MessageId::fromBytes(RepositorySql::blob(statement, 0));
    const auto conversationId = ConversationId::fromBytes(RepositorySql::blob(statement, 1));
    const auto senderId = DeviceId::fromBytes(RepositorySql::blob(statement, 2));
    const int flow = sqlite3_column_int(statement, 3);
    const int kind = sqlite3_column_int(statement, 4);
    const int state = sqlite3_column_int(statement, 7);
    if (!id || !conversationId || !senderId || flow < static_cast<int>(MessageFlow::Incoming)
        || flow > static_cast<int>(MessageFlow::Outgoing)
        || kind < static_cast<int>(ContentKind::Text)
        || kind > static_cast<int>(ContentKind::System)
        || state < static_cast<int>(DeliveryState::Draft)
        || state > static_cast<int>(DeliveryState::Failed))
        return std::nullopt;

    std::optional<quint64> sequence;
    if (sqlite3_column_type(statement, 6) != SQLITE_NULL) {
        const qint64 value = sqlite3_column_int64(statement, 6);
        if (value < 0)
            return std::nullopt;
        sequence = static_cast<quint64>(value);
    }

    std::optional<MessageId> replyTo;
    if (sqlite3_column_type(statement, 8) != SQLITE_NULL) {
        replyTo = MessageId::fromBytes(RepositorySql::blob(statement, 8));
        if (!replyTo)
            return std::nullopt;
    }

    return MessageRecord{*id,
                         *conversationId,
                         *senderId,
                         static_cast<MessageFlow>(flow),
                         static_cast<ContentKind>(kind),
                         RepositorySql::text(statement, 5),
                         sqlite3_column_int64(statement, 9),
                         static_cast<DeliveryState>(state),
                         sequence,
                         replyTo};
}

} // namespace

SqlCipherChatRepository::SqlCipherChatRepository(SqlCipherDatabase &database)
    : m_database(database)
{
}

Result<QVector<ConversationRecord>, RepositoryError> SqlCipherChatRepository::conversations()
{
    return m_database.withConnection([](sqlite3 *database) {
        Statement statement(database,
                            "SELECT id, mls_group_id, title, kind, created_at_ms "
                            "FROM conversations ORDER BY created_at_ms DESC, id DESC");
        if (!statement.isValid())
            return Result<QVector<ConversationRecord>, RepositoryError>::failure(
                internalError(QStringLiteral("conversation.prepare")));

        QVector<ConversationRecord> records;
        int step = SQLITE_ROW;
        while ((step = sqlite3_step(statement.get())) == SQLITE_ROW) {
            const auto id = ConversationId::fromBytes(RepositorySql::blob(statement.get(), 0));
            const int kind = sqlite3_column_int(statement.get(), 3);
            if (!id || kind < static_cast<int>(ConversationKind::Direct)
                || kind > static_cast<int>(ConversationKind::Group))
                return Result<QVector<ConversationRecord>, RepositoryError>::failure(
                    RepositorySql::error(RepositoryErrorCode::IntegrityFailure,
                                         QStringLiteral("conversation.decode")));
            records.append(ConversationRecord{*id,
                                              RepositorySql::blob(statement.get(), 1),
                                              RepositorySql::text(statement.get(), 2),
                                              static_cast<ConversationKind>(kind),
                                              sqlite3_column_int64(statement.get(), 4)});
        }
        if (step != SQLITE_DONE)
            return Result<QVector<ConversationRecord>, RepositoryError>::failure(
                internalError(QStringLiteral("conversation.read")));
        return Result<QVector<ConversationRecord>, RepositoryError>::success(std::move(records));
    });
}

Result<void, RepositoryError>
SqlCipherChatRepository::upsertConversation(const ConversationRecord &conversation)
{
    return m_database.withConnection([&conversation](sqlite3 *database) {
        Statement statement(database,
                            "INSERT INTO conversations(id, kind, title, created_at_ms, mls_group_id) "
                            "VALUES(?1, ?2, ?3, ?4, ?5) "
                            "ON CONFLICT(id) DO UPDATE SET kind=excluded.kind, title=excluded.title, "
                            "mls_group_id=excluded.mls_group_id");
        if (!statement.isValid()
            || !statement.bindBlob(1, conversation.id.bytes())
            || !statement.bindInt(2, static_cast<int>(conversation.kind))
            || !statement.bindText(3, conversation.title)
            || !statement.bindInt64(4, conversation.createdAtMs)
            || !statement.bindBlob(5, conversation.mlsGroupId)
            || sqlite3_step(statement.get()) != SQLITE_DONE)
            return Result<void, RepositoryError>::failure(
                internalError(QStringLiteral("conversation.upsert")));
        return Result<void, RepositoryError>::success();
    });
}

Result<QVector<MessageRecord>, RepositoryError>
SqlCipherChatRepository::messages(const ConversationId &conversationId, int limit,
                                  const std::optional<MessageId> &before)
{
    if (limit <= 0 || limit > 500)
        return Result<QVector<MessageRecord>, RepositoryError>::failure(
            RepositorySql::error(RepositoryErrorCode::InvalidInput,
                                 QStringLiteral("message.limit")));

    return m_database.withConnection([&](sqlite3 *database) {
        qint64 anchorTime = 0;
        if (before) {
            Statement anchor(database,
                             "SELECT sent_at_ms FROM messages "
                             "WHERE id=?1 AND conversation_id=?2");
            if (!anchor.isValid() || !anchor.bindBlob(1, before->bytes())
                || !anchor.bindBlob(2, conversationId.bytes()))
                return Result<QVector<MessageRecord>, RepositoryError>::failure(
                    internalError(QStringLiteral("message.anchor.prepare")));
            if (sqlite3_step(anchor.get()) != SQLITE_ROW)
                return Result<QVector<MessageRecord>, RepositoryError>::failure(
                    RepositorySql::error(RepositoryErrorCode::NotFound,
                                         QStringLiteral("message.anchor.missing")));
            anchorTime = sqlite3_column_int64(anchor.get(), 0);
        }

        const char *sql = before
                              ? "SELECT id, conversation_id, sender_device_id, flow, content_kind, "
                                "body, server_sequence, delivery_state, reply_to_id, sent_at_ms "
                                "FROM messages WHERE conversation_id=?1 AND "
                                "(sent_at_ms < ?2 OR (sent_at_ms = ?2 AND id < ?3)) "
                                "ORDER BY sent_at_ms DESC, id DESC LIMIT ?4"
                              : "SELECT id, conversation_id, sender_device_id, flow, content_kind, "
                                "body, server_sequence, delivery_state, reply_to_id, sent_at_ms "
                                "FROM messages WHERE conversation_id=?1 "
                                "ORDER BY sent_at_ms DESC, id DESC LIMIT ?2";
        Statement statement(database, sql);
        bool bound = statement.isValid() && statement.bindBlob(1, conversationId.bytes());
        if (before)
            bound = bound && statement.bindInt64(2, anchorTime)
                    && statement.bindBlob(3, before->bytes()) && statement.bindInt(4, limit);
        else
            bound = bound && statement.bindInt(2, limit);
        if (!bound)
            return Result<QVector<MessageRecord>, RepositoryError>::failure(
                internalError(QStringLiteral("message.prepare")));

        QVector<MessageRecord> records;
        int step = SQLITE_ROW;
        while ((step = sqlite3_step(statement.get())) == SQLITE_ROW) {
            auto record = decodeMessage(statement.get());
            if (!record)
                return Result<QVector<MessageRecord>, RepositoryError>::failure(
                    RepositorySql::error(RepositoryErrorCode::IntegrityFailure,
                                         QStringLiteral("message.decode")));
            records.append(std::move(*record));
        }
        if (step != SQLITE_DONE)
            return Result<QVector<MessageRecord>, RepositoryError>::failure(
                internalError(QStringLiteral("message.read")));
        return Result<QVector<MessageRecord>, RepositoryError>::success(std::move(records));
    });
}

Result<void, RepositoryError>
SqlCipherChatRepository::saveOutgoing(const MessageRecord &message, const OutboxRecord &outbox)
{
    if (message.flow != MessageFlow::Outgoing || message.id != outbox.messageId
        || message.conversationId != outbox.conversationId || message.serverSequence
        || outbox.envelope.isEmpty())
        return Result<void, RepositoryError>::failure(
            RepositorySql::error(RepositoryErrorCode::InvalidInput,
                                 QStringLiteral("outgoing.invalid")));

    return m_database.withConnection([&](sqlite3 *database) {
        if (!begin(database))
            return Result<void, RepositoryError>::failure(
                internalError(QStringLiteral("outgoing.begin")));
        if (!insertMessage(database, message)) {
            rollback(database);
            return Result<void, RepositoryError>::failure(
                conflictError(QStringLiteral("outgoing.message")));
        }

        Statement statement(database,
                            "INSERT INTO outbox(envelope_id, message_id, ciphertext, attempt_count, "
                            "next_attempt_at_ms, state, conversation_id, lease_until_ms) "
                            "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)");
        const bool inserted = statement.isValid()
                              && statement.bindBlob(1, outbox.envelopeId.bytes())
                              && statement.bindBlob(2, outbox.messageId.bytes())
                              && statement.bindBlob(3, outbox.envelope)
                              && statement.bindInt(4, outbox.attemptCount)
                              && statement.bindInt64(5, outbox.nextAttemptMs)
                              && statement.bindInt(6, static_cast<int>(outbox.state))
                              && statement.bindBlob(7, outbox.conversationId.bytes())
                              && statement.bindInt64(8, outbox.leaseUntilMs)
                              && sqlite3_step(statement.get()) == SQLITE_DONE;
        if (!inserted || !commit(database)) {
            rollback(database);
            return Result<void, RepositoryError>::failure(
                conflictError(QStringLiteral("outgoing.outbox")));
        }
        return Result<void, RepositoryError>::success();
    });
}

Result<void, RepositoryError>
SqlCipherChatRepository::applyIncoming(const MessageRecord &message,
                                       const EnvelopeId &envelopeId, quint64 watermark)
{
    if (message.flow != MessageFlow::Incoming || !message.serverSequence
        || *message.serverSequence != watermark || !fitsSqlInteger(watermark))
        return Result<void, RepositoryError>::failure(
            RepositorySql::error(RepositoryErrorCode::InvalidInput,
                                 QStringLiteral("incoming.invalid")));

    return m_database.withConnection([&](sqlite3 *database) {
        if (!begin(database))
            return Result<void, RepositoryError>::failure(
                internalError(QStringLiteral("incoming.begin")));

        Statement replay(database,
                         "INSERT OR IGNORE INTO replay_cache("
                         "envelope_id, received_at_ms, sender_device_id) VALUES(?1, ?2, ?3)");
        if (!replay.isValid() || !replay.bindBlob(1, envelopeId.bytes())
            || !replay.bindInt64(2, message.sentAtMs)
            || !replay.bindBlob(3, message.senderDeviceId.bytes())
            || sqlite3_step(replay.get()) != SQLITE_DONE) {
            rollback(database);
            return Result<void, RepositoryError>::failure(
                internalError(QStringLiteral("incoming.replay")));
        }
        if (sqlite3_changes(database) == 0) {
            if (!commit(database)) {
                rollback(database);
                return Result<void, RepositoryError>::failure(
                    internalError(QStringLiteral("incoming.idempotent.commit")));
            }
            return Result<void, RepositoryError>::success();
        }

        if (!insertMessage(database, message)) {
            rollback(database);
            return Result<void, RepositoryError>::failure(
                conflictError(QStringLiteral("incoming.message")));
        }

        Statement cursor(database,
                         "INSERT INTO device_sync_cursors("
                         "device_id, server_watermark, updated_at_ms) VALUES(?1, ?2, ?3) "
                         "ON CONFLICT(device_id) DO UPDATE SET "
                         "server_watermark=excluded.server_watermark, "
                         "updated_at_ms=excluded.updated_at_ms "
                         "WHERE excluded.server_watermark > device_sync_cursors.server_watermark");
        const bool advanced = cursor.isValid()
                              && cursor.bindBlob(1, message.senderDeviceId.bytes())
                              && cursor.bindInt64(2, static_cast<qint64>(watermark))
                              && cursor.bindInt64(3, message.sentAtMs)
                              && sqlite3_step(cursor.get()) == SQLITE_DONE;
        if (!advanced || !commit(database)) {
            rollback(database);
            return Result<void, RepositoryError>::failure(
                internalError(QStringLiteral("incoming.cursor")));
        }
        return Result<void, RepositoryError>::success();
    });
}

Result<void, RepositoryError>
SqlCipherChatRepository::advanceDeliveryState(const MessageId &messageId, DeliveryState state)
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
                RepositorySql::error(RepositoryErrorCode::NotFound,
                                     QStringLiteral("delivery.missing")));
        }
        const int stored = sqlite3_column_int(current.get(), 0);
        if (stored < static_cast<int>(DeliveryState::Draft)
            || stored > static_cast<int>(DeliveryState::Failed)
            || !canTransition(static_cast<DeliveryState>(stored), state)) {
            rollback(database);
            return Result<void, RepositoryError>::failure(
                conflictError(QStringLiteral("delivery.backward")));
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
