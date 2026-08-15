#include "storage/SqlCipherOutboxRepository.h"

#include "storage/RepositorySql.h"
#include "storage/SqlCipherDatabase.h"

namespace OpenChat {
namespace {

using RepositorySql::Statement;

RepositoryError internalError(const QString &code)
{
    return RepositorySql::error(RepositoryErrorCode::Internal, code);
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

SqlCipherOutboxRepository::SqlCipherOutboxRepository(SqlCipherDatabase &database)
    : m_database(database)
{
}

Result<QVector<OutboxRecord>, RepositoryError>
SqlCipherOutboxRepository::claimDue(qint64 nowMs, int limit, qint64 leaseUntilMs)
{
    if (limit <= 0 || limit > 100 || leaseUntilMs <= nowMs)
        return Result<QVector<OutboxRecord>, RepositoryError>::failure(
            RepositorySql::error(RepositoryErrorCode::InvalidInput,
                                 QStringLiteral("outbox.claim.input")));

    return m_database.withConnection([&](sqlite3 *database) {
        if (!RepositorySql::execute(database, "BEGIN IMMEDIATE;"))
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
            (void)RepositorySql::execute(database, "ROLLBACK;");
            return Result<QVector<OutboxRecord>, RepositoryError>::failure(
                internalError(QStringLiteral("outbox.claim.prepare")));
        }

        QVector<OutboxRecord> records;
        int step = SQLITE_ROW;
        while ((step = sqlite3_step(select.get())) == SQLITE_ROW) {
            auto record = decodeOutbox(select.get());
            if (!record) {
                (void)RepositorySql::execute(database, "ROLLBACK;");
                return Result<QVector<OutboxRecord>, RepositoryError>::failure(
                    RepositorySql::error(RepositoryErrorCode::IntegrityFailure,
                                         QStringLiteral("outbox.claim.decode")));
            }
            records.append(std::move(*record));
        }
        if (step != SQLITE_DONE) {
            (void)RepositorySql::execute(database, "ROLLBACK;");
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
                (void)RepositorySql::execute(database, "ROLLBACK;");
                return Result<QVector<OutboxRecord>, RepositoryError>::failure(
                    internalError(QStringLiteral("outbox.claim.lease")));
            }
            record.state = OutboxState::Leased;
            record.leaseUntilMs = leaseUntilMs;
        }

        if (!RepositorySql::execute(database, "COMMIT;")) {
            (void)RepositorySql::execute(database, "ROLLBACK;");
            return Result<QVector<OutboxRecord>, RepositoryError>::failure(
                internalError(QStringLiteral("outbox.claim.commit")));
        }
        return Result<QVector<OutboxRecord>, RepositoryError>::success(std::move(records));
    });
}

Result<void, RepositoryError>
SqlCipherOutboxRepository::markAccepted(const EnvelopeId &envelopeId)
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
                RepositorySql::error(RepositoryErrorCode::NotFound,
                                     QStringLiteral("outbox.accept.missing")));
        return Result<void, RepositoryError>::success();
    });
}

Result<void, RepositoryError>
SqlCipherOutboxRepository::scheduleRetry(const EnvelopeId &envelopeId, int attemptCount,
                                         qint64 nextAttemptMs)
{
    if (attemptCount < 1 || nextAttemptMs < 0)
        return Result<void, RepositoryError>::failure(
            RepositorySql::error(RepositoryErrorCode::InvalidInput,
                                 QStringLiteral("outbox.retry.input")));
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
                RepositorySql::error(RepositoryErrorCode::Conflict,
                                     QStringLiteral("outbox.retry.missing-or-accepted")));
        return Result<void, RepositoryError>::success();
    });
}

} // namespace OpenChat
