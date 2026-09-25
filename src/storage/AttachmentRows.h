#pragma once

#include "domain/Attachment.h"
#include "domain/ChatTypes.h"
#include "storage/RepositorySql.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QList>

#include <optional>

// message_attachments rows (migration 018) as SqlCipherSyncStore,
// SqlCipherChatRepository and SqlCipherAttachmentRepository write and read
// them. Internal to openchat_storage.
namespace OpenChat::AttachmentRows {

// The descriptor's columns of a row aliased `a`, in the order
// decodeDescriptor reads them.
inline constexpr char descriptorColumns[] =
    "a.attachment_id, a.kind, a.byte_count, a.part_count, a.width, a.height, a.duration_ms, "
    "a.has_preview, a.mime_type, a.file_name, a.sha256, a.attachment_key, a.peaks";
inline constexpr int descriptorColumnCount = 13;

// Recipients are stored as their 16-byte ids back to back.
[[nodiscard]] inline QByteArray encodeRecipients(const QList<DeviceId> &recipients)
{
    QByteArray bytes;
    bytes.reserve(recipients.size() * DeviceId::byteCount);
    for (const DeviceId &device : recipients)
        bytes.append(device.bytes());
    return bytes;
}

// Nothing unless every id is whole and valid.
[[nodiscard]] inline std::optional<QList<DeviceId>> decodeRecipients(QByteArrayView bytes)
{
    if (bytes.size() % DeviceId::byteCount != 0)
        return std::nullopt;
    QList<DeviceId> recipients;
    recipients.reserve(bytes.size() / DeviceId::byteCount);
    for (qsizetype at = 0; at < bytes.size(); at += DeviceId::byteCount) {
        const auto device = DeviceId::fromBytes(bytes.sliced(at, DeviceId::byteCount));
        if (!device)
            return std::nullopt;
        recipients.push_back(*device);
    }
    return recipients;
}

// The descriptor in columns first … first + descriptorColumnCount - 1, or
// nothing when there is no row (a LEFT JOIN's NULLs) or it is not one this
// version can use: a kind it does not know (written by a newer build before a
// downgrade) or anything isValidDescriptor refuses. Never an error: a message
// whose attachment cannot be read still shows, as unavailable.
[[nodiscard]] inline std::optional<AttachmentDescriptor> decodeDescriptor(sqlite3_stmt *statement, int first)
{
    if (sqlite3_column_type(statement, first) == SQLITE_NULL)
        return std::nullopt;
    const auto attachmentId = AttachmentId::fromBytes(RepositorySql::blob(statement, first));
    const qint64 kind = sqlite3_column_int64(statement, first + 1);
    const qint64 partCount = sqlite3_column_int64(statement, first + 3);
    const qint64 width = sqlite3_column_int64(statement, first + 4);
    const qint64 height = sqlite3_column_int64(statement, first + 5);
    // Bounded before any narrowing to int.
    if (!attachmentId || kind < qint64(AttachmentKind::Image) || kind > qint64(AttachmentKind::File)
        || partCount < 1 || partCount > AttachmentLimits::maxParts || width < 0
        || width > AttachmentLimits::maxImageDimension || height < 0
        || height > AttachmentLimits::maxImageDimension)
        return std::nullopt;

    AttachmentDescriptor descriptor;
    descriptor.attachmentId = *attachmentId;
    descriptor.kind = AttachmentKind(kind);
    descriptor.byteCount = sqlite3_column_int64(statement, first + 2);
    descriptor.partCount = int(partCount);
    descriptor.width = int(width);
    descriptor.height = int(height);
    descriptor.durationMs = sqlite3_column_int64(statement, first + 6);
    descriptor.hasPreview = sqlite3_column_int(statement, first + 7) != 0;
    descriptor.mimeType = RepositorySql::text(statement, first + 8);
    descriptor.fileName = RepositorySql::text(statement, first + 9);
    descriptor.sha256 = RepositorySql::blob(statement, first + 10);
    descriptor.key = RepositorySql::blob(statement, first + 11);
    descriptor.peaks = RepositorySql::blob(statement, first + 12);
    if (!isValidDescriptor(descriptor))
        return std::nullopt;
    return descriptor;
}

// The descriptor row of `message` (whose attachment must be set), Transferring
// and with nothing sent yet. `orIgnore` leaves an existing row with the same
// (conversation, sender, attachment id) as it is and inserts nothing: a
// received message that reuses an id keeps no descriptor at all.
[[nodiscard]] inline bool insertDescriptor(sqlite3 *database, const MessageRecord &message,
                                           QByteArrayView recipients, qint64 createdAtMs, bool orIgnore)
{
    if (!message.attachment)
        return false;
    const AttachmentDescriptor &descriptor = *message.attachment;
    RepositorySql::Statement statement(
        database, orIgnore ? "INSERT OR IGNORE INTO message_attachments(message_id, conversation_id, "
                             "sender_device_id, attachment_id, kind, byte_count, part_count, width, height, "
                             "duration_ms, has_preview, created_at_ms, mime_type, file_name, sha256, "
                             "attachment_key, recipients, peaks) "
                             "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, "
                             "?17, ?18)"
                           : "INSERT INTO message_attachments(message_id, conversation_id, "
                             "sender_device_id, attachment_id, kind, byte_count, part_count, width, height, "
                             "duration_ms, has_preview, created_at_ms, mime_type, file_name, sha256, "
                             "attachment_key, recipients, peaks) "
                             "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, "
                             "?17, ?18)");
    return statement.isValid() && statement.bindBlob(1, message.id.bytes())
           && statement.bindBlob(2, message.conversationId.bytes())
           && statement.bindBlob(3, message.senderDeviceId.bytes())
           && statement.bindBlob(4, descriptor.attachmentId.bytes())
           && statement.bindInt(5, int(descriptor.kind)) && statement.bindInt64(6, descriptor.byteCount)
           && statement.bindInt(7, descriptor.partCount) && statement.bindInt(8, descriptor.width)
           && statement.bindInt(9, descriptor.height) && statement.bindInt64(10, descriptor.durationMs)
           && statement.bindInt(11, descriptor.hasPreview ? 1 : 0) && statement.bindInt64(12, createdAtMs)
           && statement.bindText(13, descriptor.mimeType) && statement.bindText(14, descriptor.fileName)
           && statement.bindBlob(15, descriptor.sha256) && statement.bindBlob(16, descriptor.key)
           && (recipients.isEmpty() ? statement.bindNull(17) : statement.bindBlob(17, recipients))
           && (descriptor.peaks.isEmpty() ? statement.bindNull(18) : statement.bindBlob(18, descriptor.peaks))
           && sqlite3_step(statement.get()) == SQLITE_DONE;
}

} // namespace OpenChat::AttachmentRows
