#pragma once

#include "core/Result.h"
#include "domain/Attachment.h"
#include "domain/ChatTypes.h"
#include "domain/Identifiers.h"
#include "repositories/RepositoryError.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QList>
#include <QVector>

#include <optional>

namespace OpenChat {

// Names one attachment's bytes: the conversation, the device that sent them
// and that device's attachment id. Frames are only ever matched on all three,
// so one member of a group can never add to another member's attachment.
struct AttachmentRef final {
    ConversationId conversationId;
    DeviceId senderDeviceId;
    AttachmentId attachmentId;

    friend bool operator==(const AttachmentRef &, const AttachmentRef &) = default;
};

// One attachment message as stored (message_attachments, migration 018), with
// what the transfer needs to know about the message it belongs to.
struct StoredAttachment final {
    MessageId messageId;
    AttachmentRef ref;
    MessageFlow flow = MessageFlow::Incoming;
    // The message's own delivery: the bytes of an outgoing attachment start
    // only once its message is Sent, and stop if it Failed.
    DeliveryState deliveryState = DeliveryState::Draft;
    AttachmentDescriptor descriptor; // the key included
    AttachmentState state = AttachmentState::Transferring;
    AttachmentFailure reason = AttachmentFailure::None;
    // Outgoing only: parts and preview frames handed to the engine so far,
    // and the devices the message went to (nobody else ever gets its bytes).
    int partsSent = 0;
    bool previewSent = false;
    QList<DeviceId> recipients;
    qint64 createdAtMs = 0;
};

// What has arrived for one attachment (attachment_transfers), whether or not
// its message has: frames can overtake the message that describes them.
struct AttachmentTransferRecord final {
    AttachmentRef ref;
    int haveCount = 0;       // parts held
    qint64 haveBytes = 0;    // their sealed bytes, as stored
    qint64 firstSeenMs = 0;
    qint64 updatedAtMs = 0;  // the last part that arrived (or was dropped)
    qint64 lastRequestMs = 0;
    int requestsSent = 0;
    QByteArray present;      // bit i of byte i / 8, least significant first
    QByteArray sealedPreview; // a Preview frame's sealed body that came first

    [[nodiscard]] bool hasPart(int index) const noexcept
    {
        return index >= 0 && index / 8 < present.size() && (quint8(present[index / 8]) >> (index % 8)) & 1;
    }
};

// The counts after recordPartArrived or clearPart.
struct PartArrival final {
    bool changed = false; // false: the part was already held (or already gone)
    int haveCount = 0;
    qint64 haveBytes = 0;
};

// The bookkeeping behind chat attachments (docs/chat-attachments.md): the
// descriptor rows the engine writes with each attachment message
// (SyncStore::commitAttachmentSend, commitReceive), and what arrives or is
// sent for them. The bytes themselves are kept elsewhere, sealed; nothing
// here ever holds a part.
class AttachmentRepository
{
public:
    virtual ~AttachmentRepository() = default;

    // --- Descriptors

    // Nothing when no descriptor is stored for the message, or the stored
    // one is not one this version can use (see isValidDescriptor).
    [[nodiscard]] virtual Result<std::optional<StoredAttachment>, RepositoryError>
    descriptorFor(const MessageId &messageId) = 0;
    [[nodiscard]] virtual Result<std::optional<StoredAttachment>, RepositoryError>
    descriptorByRef(const AttachmentRef &ref) = 0;
    // Still Transferring, sent from `localDevice` / from anyone else; oldest
    // first. Rows this version cannot read are left out.
    [[nodiscard]] virtual Result<QVector<StoredAttachment>, RepositoryError>
    outgoingActive(const DeviceId &localDevice) = 0;
    [[nodiscard]] virtual Result<QVector<StoredAttachment>, RepositoryError>
    incomingActive(const DeviceId &localDevice) = 0;
    // Moves a Transferring attachment to `state` (Complete, Failed or
    // Cancelled) for `reason`. A finished one never changes again: false.
    [[nodiscard]] virtual Result<bool, RepositoryError>
    setState(const MessageId &messageId, AttachmentState state, AttachmentFailure reason) = 0;
    // An incoming attachment that cannot finish: Failed for `reason`, and
    // what had arrived forgotten, in one transaction (the caller removes the
    // stored bytes). False when it had already finished.
    [[nodiscard]] virtual Result<bool, RepositoryError>
    failIncoming(const MessageId &messageId, AttachmentFailure reason) = 0;
    // Outgoing progress, which only moves forward and never past the
    // attachment's own part count.
    [[nodiscard]] virtual Result<void, RepositoryError>
    recordFrameSent(const MessageId &messageId, int partsSent, bool previewSent) = 0;
    // The checked preview JPEG (previewIsAcceptable) of an attachment, and
    // reading it back; empty when there is none. setPreview returns false
    // for an unknown message.
    [[nodiscard]] virtual Result<bool, RepositoryError>
    setPreview(const MessageId &messageId, QByteArrayView jpeg) = 0;
    [[nodiscard]] virtual Result<QByteArray, RepositoryError> preview(const MessageId &messageId) = 0;

    // --- What arrived

    [[nodiscard]] virtual Result<std::optional<AttachmentTransferRecord>, RepositoryError>
    transfer(const AttachmentRef &ref) = 0;
    // Marks part `index` (< AttachmentLimits::maxParts) held, `bytes` of it
    // stored, starting the transfer row if this is the first thing to arrive.
    // Idempotent: a part already held changes nothing.
    [[nodiscard]] virtual Result<PartArrival, RepositoryError>
    recordPartArrived(const AttachmentRef &ref, int index, qint64 bytes, qint64 nowMs) = 0;
    // The reverse, for a part found to be damaged: `bytes` is what
    // recordPartArrived was told.
    [[nodiscard]] virtual Result<PartArrival, RepositoryError>
    clearPart(const AttachmentRef &ref, int index, qint64 bytes, qint64 nowMs) = 0;
    // A Preview frame's sealed body (at most maxPreviewBytes + sealOverhead)
    // kept until its descriptor arrives; an empty body clears it.
    [[nodiscard]] virtual Result<void, RepositoryError>
    setSealedPreview(const AttachmentRef &ref, QByteArrayView sealedBody, qint64 nowMs) = 0;
    // A request for missing parts went out now.
    [[nodiscard]] virtual Result<void, RepositoryError>
    recordRequest(const AttachmentRef &ref, qint64 nowMs) = 0;
    // Transfers first seen before `olderThanMs` whose descriptor never came.
    [[nodiscard]] virtual Result<QVector<AttachmentTransferRecord>, RepositoryError>
    orphans(qint64 olderThanMs) = 0;
    // Transfers whose attachment Failed or was Cancelled: their bytes can go.
    [[nodiscard]] virtual Result<QVector<AttachmentRef>, RepositoryError> abandonedTransfers() = 0;
    [[nodiscard]] virtual Result<void, RepositoryError> deleteTransfer(const AttachmentRef &ref) = 0;
    // Sealed bytes stored for every transfer / for the transfers from
    // `sender` in `conversation` that no descriptor names yet.
    [[nodiscard]] virtual Result<qint64, RepositoryError> receivedBytes() = 0;
    [[nodiscard]] virtual Result<qint64, RepositoryError>
    orphanBytes(const ConversationId &conversation, const DeviceId &sender) = 0;

    // The conversation exists and was not left: frames for anything else are
    // dropped unread.
    [[nodiscard]] virtual Result<bool, RepositoryError>
    conversationIsLive(const ConversationId &conversation) = 0;
};

} // namespace OpenChat
