#pragma once

#include "repositories/AttachmentRepository.h"

namespace OpenChat {

class SqlCipherDatabase;

// AttachmentRepository over the profile's SQLCipher database (migration 018).
// The descriptor rows it reads are written by SqlCipherSyncStore in the same
// transaction as their message; this class only moves their state along and
// keeps the per-attachment arrival bookkeeping.
class SqlCipherAttachmentRepository final : public AttachmentRepository
{
public:
    explicit SqlCipherAttachmentRepository(SqlCipherDatabase &database);

    [[nodiscard]] Result<std::optional<StoredAttachment>, RepositoryError>
    descriptorFor(const MessageId &messageId) override;
    [[nodiscard]] Result<std::optional<StoredAttachment>, RepositoryError>
    descriptorByRef(const AttachmentRef &ref) override;
    [[nodiscard]] Result<QVector<StoredAttachment>, RepositoryError>
    outgoingActive(const DeviceId &localDevice) override;
    [[nodiscard]] Result<QVector<StoredAttachment>, RepositoryError>
    incomingActive(const DeviceId &localDevice) override;
    [[nodiscard]] Result<bool, RepositoryError>
    setState(const MessageId &messageId, AttachmentState state, AttachmentFailure reason) override;
    [[nodiscard]] Result<bool, RepositoryError>
    failIncoming(const MessageId &messageId, AttachmentFailure reason) override;
    [[nodiscard]] Result<void, RepositoryError>
    recordFrameSent(const MessageId &messageId, int partsSent, bool previewSent) override;
    [[nodiscard]] Result<bool, RepositoryError>
    setPreview(const MessageId &messageId, QByteArrayView jpeg) override;
    [[nodiscard]] Result<QByteArray, RepositoryError> preview(const MessageId &messageId) override;

    [[nodiscard]] Result<std::optional<AttachmentTransferRecord>, RepositoryError>
    transfer(const AttachmentRef &ref) override;
    [[nodiscard]] Result<PartArrival, RepositoryError>
    recordPartArrived(const AttachmentRef &ref, int index, qint64 bytes, qint64 nowMs) override;
    [[nodiscard]] Result<PartArrival, RepositoryError>
    clearPart(const AttachmentRef &ref, int index, qint64 bytes, qint64 nowMs) override;
    [[nodiscard]] Result<void, RepositoryError>
    setSealedPreview(const AttachmentRef &ref, QByteArrayView sealedBody, qint64 nowMs) override;
    [[nodiscard]] Result<void, RepositoryError>
    recordRequest(const AttachmentRef &ref, qint64 nowMs) override;
    [[nodiscard]] Result<QVector<AttachmentTransferRecord>, RepositoryError>
    orphans(qint64 olderThanMs) override;
    [[nodiscard]] Result<QVector<AttachmentRef>, RepositoryError> abandonedTransfers() override;
    [[nodiscard]] Result<void, RepositoryError> deleteTransfer(const AttachmentRef &ref) override;
    [[nodiscard]] Result<qint64, RepositoryError> receivedBytes() override;
    [[nodiscard]] Result<qint64, RepositoryError>
    orphanBytes(const ConversationId &conversation, const DeviceId &sender) override;

    [[nodiscard]] Result<bool, RepositoryError>
    conversationIsLive(const ConversationId &conversation) override;

private:
    SqlCipherDatabase &m_database;
};

} // namespace OpenChat
