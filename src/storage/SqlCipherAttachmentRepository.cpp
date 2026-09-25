#include "storage/SqlCipherAttachmentRepository.h"

#include "storage/AttachmentRows.h"
#include "storage/RepositorySql.h"
#include "storage/SqlCipherDatabase.h"

#include <algorithm>
#include <utility>

namespace OpenChat {
namespace {

using RepositorySql::Statement;

// One bit per part a file may have.
constexpr qsizetype bitmapBytes = (AttachmentLimits::maxParts + 7) / 8;
constexpr qsizetype maxSealedPreviewBytes =
    AttachmentLimits::maxPreviewBytes + AttachmentLimits::sealOverhead;

RepositoryError error(RepositoryErrorCode code, const QString &diagnostic)
{
    return RepositorySql::error(code, diagnostic);
}

RepositoryError internalError(const QString &code)
{
    return error(RepositoryErrorCode::Internal, code);
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

bool bindRef(Statement &statement, int first, const AttachmentRef &ref)
{
    return statement.bindBlob(first, ref.conversationId.bytes())
           && statement.bindBlob(first + 1, ref.senderDeviceId.bytes())
           && statement.bindBlob(first + 2, ref.attachmentId.bytes());
}

std::optional<AttachmentRef> decodeRef(sqlite3_stmt *statement, int first)
{
    const auto conversation = ConversationId::fromBytes(RepositorySql::blob(statement, first));
    const auto sender = DeviceId::fromBytes(RepositorySql::blob(statement, first + 1));
    const auto attachment = AttachmentId::fromBytes(RepositorySql::blob(statement, first + 2));
    if (!conversation || !sender || !attachment)
        return std::nullopt;
    return AttachmentRef{*conversation, *sender, *attachment};
}

// A descriptor row and its message: StoredAttachment's columns, then the
// descriptor's (AttachmentRows::descriptorColumns).
QByteArray storedSelect(const char *where)
{
    QByteArray sql = "SELECT a.message_id, a.conversation_id, a.sender_device_id, a.attachment_id, "
                     "m.flow, m.delivery_state, a.state, a.reason, a.parts_sent, a.preview_sent, "
                     "a.recipients, a.created_at_ms, ";
    sql += AttachmentRows::descriptorColumns;
    sql += " FROM message_attachments a JOIN messages m ON m.id = a.message_id WHERE ";
    sql += where;
    return sql;
}
constexpr int storedDescriptorColumn = 12;

// Nothing for a row this version cannot use; the caller treats it as absent.
std::optional<StoredAttachment> decodeStored(sqlite3_stmt *statement)
{
    const auto messageId = MessageId::fromBytes(RepositorySql::blob(statement, 0));
    const auto ref = decodeRef(statement, 1);
    const int flow = sqlite3_column_int(statement, 4);
    const int delivery = sqlite3_column_int(statement, 5);
    const int state = sqlite3_column_int(statement, 6);
    const int reason = sqlite3_column_int(statement, 7);
    const auto recipients = AttachmentRows::decodeRecipients(RepositorySql::blob(statement, 10));
    const auto descriptor = AttachmentRows::decodeDescriptor(statement, storedDescriptorColumn);
    if (!messageId || !ref || !descriptor || !recipients || flow < int(MessageFlow::Incoming)
        || flow > int(MessageFlow::Outgoing) || delivery < int(DeliveryState::Draft)
        || delivery > int(DeliveryState::Failed) || state < int(AttachmentState::Transferring)
        || state > int(AttachmentState::Cancelled) || reason < int(AttachmentFailure::None)
        || reason > int(AttachmentFailure::SendFailed))
        return std::nullopt;

    return StoredAttachment{
        .messageId = *messageId,
        .ref = *ref,
        .flow = MessageFlow(flow),
        .deliveryState = DeliveryState(delivery),
        .descriptor = *descriptor,
        .state = AttachmentState(state),
        .reason = AttachmentFailure(reason),
        .partsSent = std::clamp(sqlite3_column_int(statement, 8), 0, descriptor->partCount),
        .previewSent = sqlite3_column_int(statement, 9) != 0,
        .recipients = *recipients,
        .createdAtMs = sqlite3_column_int64(statement, 11),
    };
}

// Every StoredAttachment `sql` selects, skipping the rows this version cannot
// use (they could never progress anyway).
Result<QVector<StoredAttachment>, RepositoryError>
storedList(sqlite3 *database, const QByteArray &sql, const DeviceId &localDevice, const QString &code)
{
    using Ret = Result<QVector<StoredAttachment>, RepositoryError>;
    Statement statement(database, sql.constData());
    if (!statement.isValid() || !statement.bindBlob(1, localDevice.bytes()))
        return Ret::failure(internalError(code + QStringLiteral(".prepare")));
    QVector<StoredAttachment> rows;
    int step = SQLITE_ROW;
    while ((step = sqlite3_step(statement.get())) == SQLITE_ROW) {
        if (auto stored = decodeStored(statement.get()))
            rows.push_back(std::move(*stored));
    }
    if (step != SQLITE_DONE)
        return Ret::failure(internalError(code + QStringLiteral(".read")));
    return Ret::success(std::move(rows));
}

constexpr char transferSelect[] =
    "SELECT t.conversation_id, t.sender_device_id, t.attachment_id, t.have_count, t.have_bytes, "
    "t.first_seen_ms, t.updated_at_ms, t.last_request_ms, t.requests_sent, t.present, "
    "t.sealed_preview FROM attachment_transfers t ";

std::optional<AttachmentTransferRecord> decodeTransfer(sqlite3_stmt *statement)
{
    const auto ref = decodeRef(statement, 0);
    if (!ref)
        return std::nullopt;
    QByteArray present = RepositorySql::blob(statement, 9);
    present.resize(bitmapBytes, '\0');
    return AttachmentTransferRecord{
        .ref = *ref,
        .haveCount = sqlite3_column_int(statement, 3),
        .haveBytes = sqlite3_column_int64(statement, 4),
        .firstSeenMs = sqlite3_column_int64(statement, 5),
        .updatedAtMs = sqlite3_column_int64(statement, 6),
        .lastRequestMs = sqlite3_column_int64(statement, 7),
        .requestsSent = sqlite3_column_int(statement, 8),
        .present = std::move(present),
        .sealedPreview = RepositorySql::blob(statement, 10),
    };
}

// Starts the transfer row of `ref` if nothing has arrived for it yet.
bool ensureTransfer(sqlite3 *database, const AttachmentRef &ref, qint64 nowMs)
{
    Statement statement(database,
                        "INSERT OR IGNORE INTO attachment_transfers(conversation_id, "
                        "sender_device_id, attachment_id, first_seen_ms, updated_at_ms, present) "
                        "VALUES(?1, ?2, ?3, ?4, ?4, ?5)");
    return statement.isValid() && bindRef(statement, 1, ref) && statement.bindInt64(4, nowMs)
           && statement.bindBlob(5, QByteArray(bitmapBytes, '\0'))
           && sqlite3_step(statement.get()) == SQLITE_DONE;
}

// Sets (`held`) or clears one part's bit and its counts in one transaction.
// Nothing changes when the bit already says so; clearing never starts a row.
Result<PartArrival, RepositoryError> changePart(sqlite3 *database, const AttachmentRef &ref,
                                                int index, qint64 bytes, qint64 nowMs, bool held,
                                                const QString &code)
{
    using Ret = Result<PartArrival, RepositoryError>;
    if (!begin(database))
        return Ret::failure(internalError(code + QStringLiteral(".begin")));
    if (held && !ensureTransfer(database, ref, nowMs)) {
        rollback(database);
        return Ret::failure(internalError(code + QStringLiteral(".start")));
    }
    Statement read(database,
                   "SELECT have_count, have_bytes, present FROM attachment_transfers "
                   "WHERE conversation_id=?1 AND sender_device_id=?2 AND attachment_id=?3");
    if (!read.isValid() || !bindRef(read, 1, ref)) {
        rollback(database);
        return Ret::failure(internalError(code + QStringLiteral(".read")));
    }
    const int step = sqlite3_step(read.get());
    if (step == SQLITE_DONE) {
        rollback(database);
        return Ret::success(PartArrival{});
    }
    if (step != SQLITE_ROW) {
        rollback(database);
        return Ret::failure(internalError(code + QStringLiteral(".read")));
    }
    PartArrival arrival{false, sqlite3_column_int(read.get(), 0),
                        sqlite3_column_int64(read.get(), 1)};
    QByteArray present = RepositorySql::blob(read.get(), 2);
    present.resize(bitmapBytes, '\0');
    const char bit = char(1 << (index % 8));
    char &byte = present[index / 8];
    if (((byte & bit) != 0) == held) {
        if (!commit(database)) {
            rollback(database);
            return Ret::failure(internalError(code + QStringLiteral(".commit")));
        }
        return Ret::success(arrival);
    }

    byte = char(held ? byte | bit : byte & ~bit);
    arrival.changed = true;
    arrival.haveCount = std::max(0, arrival.haveCount + (held ? 1 : -1));
    arrival.haveBytes = std::max<qint64>(0, arrival.haveBytes + (held ? bytes : -bytes));
    Statement update(database,
                     "UPDATE attachment_transfers SET present=?4, have_count=?5, have_bytes=?6, "
                     "updated_at_ms=?7 "
                     "WHERE conversation_id=?1 AND sender_device_id=?2 AND attachment_id=?3");
    if (!update.isValid() || !bindRef(update, 1, ref) || !update.bindBlob(4, present)
        || !update.bindInt(5, arrival.haveCount) || !update.bindInt64(6, arrival.haveBytes)
        || !update.bindInt64(7, nowMs) || sqlite3_step(update.get()) != SQLITE_DONE
        || !commit(database)) {
        rollback(database);
        return Ret::failure(internalError(code + QStringLiteral(".update")));
    }
    return Ret::success(arrival);
}

} // namespace

SqlCipherAttachmentRepository::SqlCipherAttachmentRepository(SqlCipherDatabase &database)
    : m_database(database)
{
}

Result<std::optional<StoredAttachment>, RepositoryError>
SqlCipherAttachmentRepository::descriptorFor(const MessageId &messageId)
{
    using Ret = Result<std::optional<StoredAttachment>, RepositoryError>;
    return m_database.withConnection([&](sqlite3 *database) {
        const QByteArray sql = storedSelect("a.message_id=?1");
        Statement statement(database, sql.constData());
        if (!statement.isValid() || !statement.bindBlob(1, messageId.bytes()))
            return Ret::failure(internalError(QStringLiteral("attachment.descriptor.prepare")));
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE)
            return Ret::success(std::nullopt);
        if (step != SQLITE_ROW)
            return Ret::failure(internalError(QStringLiteral("attachment.descriptor.read")));
        return Ret::success(decodeStored(statement.get()));
    });
}

Result<std::optional<StoredAttachment>, RepositoryError>
SqlCipherAttachmentRepository::descriptorByRef(const AttachmentRef &ref)
{
    using Ret = Result<std::optional<StoredAttachment>, RepositoryError>;
    return m_database.withConnection([&](sqlite3 *database) {
        const QByteArray sql = storedSelect(
            "a.conversation_id=?1 AND a.sender_device_id=?2 AND a.attachment_id=?3");
        Statement statement(database, sql.constData());
        if (!statement.isValid() || !bindRef(statement, 1, ref))
            return Ret::failure(internalError(QStringLiteral("attachment.byRef.prepare")));
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE)
            return Ret::success(std::nullopt);
        if (step != SQLITE_ROW)
            return Ret::failure(internalError(QStringLiteral("attachment.byRef.read")));
        return Ret::success(decodeStored(statement.get()));
    });
}

Result<QVector<StoredAttachment>, RepositoryError>
SqlCipherAttachmentRepository::outgoingActive(const DeviceId &localDevice)
{
    // state 0 = Transferring; message_attachments_active serves both lists.
    return m_database.withConnection([&](sqlite3 *database) {
        return storedList(database,
                          storedSelect("a.state=0 AND a.sender_device_id=?1 "
                                       "ORDER BY a.created_at_ms, a.rowid"),
                          localDevice, QStringLiteral("attachment.outgoing"));
    });
}

Result<QVector<StoredAttachment>, RepositoryError>
SqlCipherAttachmentRepository::incomingActive(const DeviceId &localDevice)
{
    return m_database.withConnection([&](sqlite3 *database) {
        return storedList(database,
                          storedSelect("a.state=0 AND a.sender_device_id<>?1 "
                                       "ORDER BY a.created_at_ms, a.rowid"),
                          localDevice, QStringLiteral("attachment.incoming"));
    });
}

Result<bool, RepositoryError>
SqlCipherAttachmentRepository::setState(const MessageId &messageId, AttachmentState state,
                                        AttachmentFailure reason)
{
    if (state != AttachmentState::Complete && state != AttachmentState::Failed
        && state != AttachmentState::Cancelled)
        return Result<bool, RepositoryError>::failure(
            error(RepositoryErrorCode::InvalidInput, QStringLiteral("attachment.state.input")));
    return m_database.withConnection([&](sqlite3 *database) {
        Statement statement(database,
                            "UPDATE message_attachments SET state=?1, reason=?2 "
                            "WHERE message_id=?3 AND state=0");
        if (!statement.isValid() || !statement.bindInt(1, int(state))
            || !statement.bindInt(2, int(reason)) || !statement.bindBlob(3, messageId.bytes())
            || sqlite3_step(statement.get()) != SQLITE_DONE)
            return Result<bool, RepositoryError>::failure(
                internalError(QStringLiteral("attachment.state")));
        return Result<bool, RepositoryError>::success(sqlite3_changes(database) == 1);
    });
}

Result<bool, RepositoryError>
SqlCipherAttachmentRepository::failIncoming(const MessageId &messageId, AttachmentFailure reason)
{
    using Ret = Result<bool, RepositoryError>;
    return m_database.withConnection([&](sqlite3 *database) {
        if (!begin(database))
            return Ret::failure(internalError(QStringLiteral("attachment.fail.begin")));
        Statement read(database,
                       "SELECT conversation_id, sender_device_id, attachment_id "
                       "FROM message_attachments WHERE message_id=?1 AND state=0");
        if (!read.isValid() || !read.bindBlob(1, messageId.bytes())) {
            rollback(database);
            return Ret::failure(internalError(QStringLiteral("attachment.fail.read")));
        }
        const int step = sqlite3_step(read.get());
        if (step == SQLITE_DONE) { // unknown, or already finished
            rollback(database);
            return Ret::success(false);
        }
        const auto ref = step == SQLITE_ROW ? decodeRef(read.get(), 0) : std::nullopt;
        if (!ref) {
            rollback(database);
            return Ret::failure(internalError(QStringLiteral("attachment.fail.read")));
        }
        Statement update(database,
                         "UPDATE message_attachments SET state=2, reason=?1 "
                         "WHERE message_id=?2 AND state=0");
        Statement forget(database,
                         "DELETE FROM attachment_transfers "
                         "WHERE conversation_id=?1 AND sender_device_id=?2 AND attachment_id=?3");
        if (!update.isValid() || !update.bindInt(1, int(reason))
            || !update.bindBlob(2, messageId.bytes()) || sqlite3_step(update.get()) != SQLITE_DONE
            || !forget.isValid() || !bindRef(forget, 1, *ref)
            || sqlite3_step(forget.get()) != SQLITE_DONE || !commit(database)) {
            rollback(database);
            return Ret::failure(internalError(QStringLiteral("attachment.fail")));
        }
        return Ret::success(true);
    });
}

Result<void, RepositoryError>
SqlCipherAttachmentRepository::recordFrameSent(const MessageId &messageId, int partsSent,
                                               bool previewSent)
{
    if (partsSent < 0)
        return Result<void, RepositoryError>::failure(
            error(RepositoryErrorCode::InvalidInput, QStringLiteral("attachment.sent.input")));
    return m_database.withConnection([&](sqlite3 *database) {
        Statement statement(database,
                            "UPDATE message_attachments SET "
                            "parts_sent = MIN(part_count, MAX(parts_sent, ?1)), "
                            "preview_sent = MAX(preview_sent, ?2) WHERE message_id=?3");
        if (!statement.isValid() || !statement.bindInt(1, partsSent)
            || !statement.bindInt(2, previewSent ? 1 : 0)
            || !statement.bindBlob(3, messageId.bytes())
            || sqlite3_step(statement.get()) != SQLITE_DONE)
            return Result<void, RepositoryError>::failure(
                internalError(QStringLiteral("attachment.sent")));
        if (sqlite3_changes(database) == 0)
            return Result<void, RepositoryError>::failure(
                error(RepositoryErrorCode::NotFound, QStringLiteral("attachment.sent.missing")));
        return Result<void, RepositoryError>::success();
    });
}

Result<bool, RepositoryError>
SqlCipherAttachmentRepository::setPreview(const MessageId &messageId, QByteArrayView jpeg)
{
    if (jpeg.isEmpty() || jpeg.size() > AttachmentLimits::maxPreviewBytes)
        return Result<bool, RepositoryError>::failure(
            error(RepositoryErrorCode::InvalidInput, QStringLiteral("attachment.preview.input")));
    return m_database.withConnection([&](sqlite3 *database) {
        Statement statement(database,
                            "UPDATE message_attachments SET preview=?1 WHERE message_id=?2");
        if (!statement.isValid() || !statement.bindBlob(1, jpeg)
            || !statement.bindBlob(2, messageId.bytes())
            || sqlite3_step(statement.get()) != SQLITE_DONE)
            return Result<bool, RepositoryError>::failure(
                internalError(QStringLiteral("attachment.preview")));
        return Result<bool, RepositoryError>::success(sqlite3_changes(database) == 1);
    });
}

Result<QByteArray, RepositoryError>
SqlCipherAttachmentRepository::preview(const MessageId &messageId)
{
    using Ret = Result<QByteArray, RepositoryError>;
    return m_database.withConnection([&](sqlite3 *database) {
        Statement statement(database,
                            "SELECT preview FROM message_attachments WHERE message_id=?1");
        if (!statement.isValid() || !statement.bindBlob(1, messageId.bytes()))
            return Ret::failure(internalError(QStringLiteral("attachment.preview.prepare")));
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE)
            return Ret::success(QByteArray());
        if (step != SQLITE_ROW)
            return Ret::failure(internalError(QStringLiteral("attachment.preview.read")));
        return Ret::success(RepositorySql::blob(statement.get(), 0));
    });
}

Result<std::optional<AttachmentTransferRecord>, RepositoryError>
SqlCipherAttachmentRepository::transfer(const AttachmentRef &ref)
{
    using Ret = Result<std::optional<AttachmentTransferRecord>, RepositoryError>;
    return m_database.withConnection([&](sqlite3 *database) {
        const QByteArray sql =
            QByteArray(transferSelect)
            + "WHERE t.conversation_id=?1 AND t.sender_device_id=?2 AND t.attachment_id=?3";
        Statement statement(database, sql.constData());
        if (!statement.isValid() || !bindRef(statement, 1, ref))
            return Ret::failure(internalError(QStringLiteral("attachment.transfer.prepare")));
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE)
            return Ret::success(std::nullopt);
        if (step != SQLITE_ROW)
            return Ret::failure(internalError(QStringLiteral("attachment.transfer.read")));
        auto record = decodeTransfer(statement.get());
        if (!record)
            return Ret::failure(error(RepositoryErrorCode::IntegrityFailure,
                                      QStringLiteral("attachment.transfer.decode")));
        return Ret::success(std::move(record));
    });
}

Result<PartArrival, RepositoryError>
SqlCipherAttachmentRepository::recordPartArrived(const AttachmentRef &ref, int index, qint64 bytes,
                                                 qint64 nowMs)
{
    if (index < 0 || index >= AttachmentLimits::maxParts || bytes < 0
        || bytes > AttachmentLimits::maxFrameBytes)
        return Result<PartArrival, RepositoryError>::failure(
            error(RepositoryErrorCode::InvalidInput, QStringLiteral("attachment.part.input")));
    return m_database.withConnection([&](sqlite3 *database) {
        return changePart(database, ref, index, bytes, nowMs, true,
                          QStringLiteral("attachment.part"));
    });
}

Result<PartArrival, RepositoryError>
SqlCipherAttachmentRepository::clearPart(const AttachmentRef &ref, int index, qint64 bytes,
                                         qint64 nowMs)
{
    if (index < 0 || index >= AttachmentLimits::maxParts || bytes < 0
        || bytes > AttachmentLimits::maxFrameBytes)
        return Result<PartArrival, RepositoryError>::failure(
            error(RepositoryErrorCode::InvalidInput, QStringLiteral("attachment.clear.input")));
    return m_database.withConnection([&](sqlite3 *database) {
        return changePart(database, ref, index, bytes, nowMs, false,
                          QStringLiteral("attachment.clear"));
    });
}

Result<void, RepositoryError>
SqlCipherAttachmentRepository::setSealedPreview(const AttachmentRef &ref,
                                                QByteArrayView sealedBody, qint64 nowMs)
{
    if (sealedBody.size() > maxSealedPreviewBytes)
        return Result<void, RepositoryError>::failure(error(
            RepositoryErrorCode::InvalidInput, QStringLiteral("attachment.sealedPreview.input")));
    return m_database.withConnection([&](sqlite3 *database) {
        // Clearing never starts a row; keeping one does.
        Statement statement(
            database,
            sealedBody.isEmpty()
                ? "UPDATE attachment_transfers SET sealed_preview=NULL "
                  "WHERE conversation_id=?1 AND sender_device_id=?2 AND attachment_id=?3"
                : "INSERT INTO attachment_transfers(conversation_id, sender_device_id, "
                  "attachment_id, first_seen_ms, updated_at_ms, present, sealed_preview) "
                  "VALUES(?1, ?2, ?3, ?4, ?4, ?5, ?6) "
                  "ON CONFLICT(conversation_id, sender_device_id, attachment_id) "
                  "DO UPDATE SET sealed_preview=excluded.sealed_preview");
        const bool bound = statement.isValid() && bindRef(statement, 1, ref)
                           && (sealedBody.isEmpty()
                               || (statement.bindInt64(4, nowMs)
                                   && statement.bindBlob(5, QByteArray(bitmapBytes, '\0'))
                                   && statement.bindBlob(6, sealedBody)));
        if (!bound || sqlite3_step(statement.get()) != SQLITE_DONE)
            return Result<void, RepositoryError>::failure(
                internalError(QStringLiteral("attachment.sealedPreview")));
        return Result<void, RepositoryError>::success();
    });
}

Result<void, RepositoryError>
SqlCipherAttachmentRepository::recordRequest(const AttachmentRef &ref, qint64 nowMs)
{
    return m_database.withConnection([&](sqlite3 *database) {
        Statement statement(database,
                            "INSERT INTO attachment_transfers(conversation_id, sender_device_id, "
                            "attachment_id, first_seen_ms, updated_at_ms, present, "
                            "last_request_ms, requests_sent) VALUES(?1, ?2, ?3, ?4, ?4, ?5, ?4, 1) "
                            "ON CONFLICT(conversation_id, sender_device_id, attachment_id) "
                            "DO UPDATE SET last_request_ms=excluded.last_request_ms, "
                            "requests_sent=requests_sent + 1");
        if (!statement.isValid() || !bindRef(statement, 1, ref) || !statement.bindInt64(4, nowMs)
            || !statement.bindBlob(5, QByteArray(bitmapBytes, '\0'))
            || sqlite3_step(statement.get()) != SQLITE_DONE)
            return Result<void, RepositoryError>::failure(
                internalError(QStringLiteral("attachment.request")));
        return Result<void, RepositoryError>::success();
    });
}

Result<QVector<AttachmentTransferRecord>, RepositoryError>
SqlCipherAttachmentRepository::orphans(qint64 olderThanMs)
{
    using Ret = Result<QVector<AttachmentTransferRecord>, RepositoryError>;
    return m_database.withConnection([&](sqlite3 *database) {
        // The NOT EXISTS is served by the descriptors' UNIQUE key.
        const QByteArray sql =
            QByteArray(transferSelect)
            + "WHERE t.first_seen_ms < ?1 AND NOT EXISTS (SELECT 1 FROM message_attachments a "
              "WHERE a.conversation_id = t.conversation_id "
              "AND a.sender_device_id = t.sender_device_id "
              "AND a.attachment_id = t.attachment_id) ORDER BY t.first_seen_ms";
        Statement statement(database, sql.constData());
        if (!statement.isValid() || !statement.bindInt64(1, olderThanMs))
            return Ret::failure(internalError(QStringLiteral("attachment.orphans.prepare")));
        QVector<AttachmentTransferRecord> rows;
        int step = SQLITE_ROW;
        while ((step = sqlite3_step(statement.get())) == SQLITE_ROW) {
            auto record = decodeTransfer(statement.get());
            if (!record)
                return Ret::failure(error(RepositoryErrorCode::IntegrityFailure,
                                          QStringLiteral("attachment.orphans.decode")));
            rows.push_back(std::move(*record));
        }
        if (step != SQLITE_DONE)
            return Ret::failure(internalError(QStringLiteral("attachment.orphans.read")));
        return Ret::success(std::move(rows));
    });
}

Result<QVector<AttachmentRef>, RepositoryError> SqlCipherAttachmentRepository::abandonedTransfers()
{
    using Ret = Result<QVector<AttachmentRef>, RepositoryError>;
    return m_database.withConnection([&](sqlite3 *database) {
        // state 2 = Failed, 3 = Cancelled.
        Statement statement(database,
                            "SELECT t.conversation_id, t.sender_device_id, t.attachment_id "
                            "FROM attachment_transfers t JOIN message_attachments a "
                            "ON a.conversation_id = t.conversation_id "
                            "AND a.sender_device_id = t.sender_device_id "
                            "AND a.attachment_id = t.attachment_id WHERE a.state IN (2,3)");
        if (!statement.isValid())
            return Ret::failure(internalError(QStringLiteral("attachment.abandoned.prepare")));
        QVector<AttachmentRef> refs;
        int step = SQLITE_ROW;
        while ((step = sqlite3_step(statement.get())) == SQLITE_ROW) {
            const auto ref = decodeRef(statement.get(), 0);
            if (!ref)
                return Ret::failure(error(RepositoryErrorCode::IntegrityFailure,
                                          QStringLiteral("attachment.abandoned.decode")));
            refs.push_back(*ref);
        }
        if (step != SQLITE_DONE)
            return Ret::failure(internalError(QStringLiteral("attachment.abandoned.read")));
        return Ret::success(std::move(refs));
    });
}

Result<void, RepositoryError> SqlCipherAttachmentRepository::deleteTransfer(const AttachmentRef &ref)
{
    return m_database.withConnection([&](sqlite3 *database) {
        Statement statement(database,
                            "DELETE FROM attachment_transfers "
                            "WHERE conversation_id=?1 AND sender_device_id=?2 AND attachment_id=?3");
        if (!statement.isValid() || !bindRef(statement, 1, ref)
            || sqlite3_step(statement.get()) != SQLITE_DONE)
            return Result<void, RepositoryError>::failure(
                internalError(QStringLiteral("attachment.transfer.delete")));
        return Result<void, RepositoryError>::success();
    });
}

Result<qint64, RepositoryError> SqlCipherAttachmentRepository::receivedBytes()
{
    return m_database.withConnection([&](sqlite3 *database) {
        Statement statement(database,
                            "SELECT COALESCE(SUM(have_bytes), 0) FROM attachment_transfers");
        if (!statement.isValid() || sqlite3_step(statement.get()) != SQLITE_ROW)
            return Result<qint64, RepositoryError>::failure(
                internalError(QStringLiteral("attachment.bytes")));
        return Result<qint64, RepositoryError>::success(sqlite3_column_int64(statement.get(), 0));
    });
}

Result<qint64, RepositoryError>
SqlCipherAttachmentRepository::orphanBytes(const ConversationId &conversation,
                                           const DeviceId &sender)
{
    return m_database.withConnection([&](sqlite3 *database) {
        Statement statement(database,
                            "SELECT COALESCE(SUM(t.have_bytes), 0) FROM attachment_transfers t "
                            "WHERE t.conversation_id=?1 AND t.sender_device_id=?2 "
                            "AND NOT EXISTS (SELECT 1 FROM message_attachments a "
                            "WHERE a.conversation_id = t.conversation_id "
                            "AND a.sender_device_id = t.sender_device_id "
                            "AND a.attachment_id = t.attachment_id)");
        if (!statement.isValid() || !statement.bindBlob(1, conversation.bytes())
            || !statement.bindBlob(2, sender.bytes())
            || sqlite3_step(statement.get()) != SQLITE_ROW)
            return Result<qint64, RepositoryError>::failure(
                internalError(QStringLiteral("attachment.orphanBytes")));
        return Result<qint64, RepositoryError>::success(sqlite3_column_int64(statement.get(), 0));
    });
}

Result<bool, RepositoryError>
SqlCipherAttachmentRepository::conversationIsLive(const ConversationId &conversation)
{
    using Ret = Result<bool, RepositoryError>;
    return m_database.withConnection([&](sqlite3 *database) {
        Statement statement(database, "SELECT left_at_ms FROM conversations WHERE id=?1");
        if (!statement.isValid() || !statement.bindBlob(1, conversation.bytes()))
            return Ret::failure(internalError(QStringLiteral("attachment.live.prepare")));
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE)
            return Ret::success(false);
        if (step != SQLITE_ROW)
            return Ret::failure(internalError(QStringLiteral("attachment.live.read")));
        return Ret::success(sqlite3_column_int64(statement.get(), 0) == 0);
    });
}

} // namespace OpenChat
