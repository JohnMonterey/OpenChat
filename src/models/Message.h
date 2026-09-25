#pragma once

#include <QDate>
#include <QString>
#include <QTime>
#include <QVector>

namespace OpenChat {

enum class MessageDirection {
    Incoming,
    Outgoing,
};

enum class MessageKind {
    Text,
    Emoji,
    MembershipEvent,
    CallEvent,
    // A photo, video, audio clip or file; the body is its caption.
    Attachment,
};

// How far an attachment's bytes have come, as its bubble shows it. The first
// four mirror the stored transfer state; Unavailable is an attachment message
// whose description could not be stored or read back, which can only say so.
enum class AttachmentTransferState {
    Transferring,
    Ready,
    Failed,
    Cancelled,
    Unavailable,
};

// Durable delivery lifecycle surfaced to the UI. Mirrors the domain
// DeliveryState semantics but stays confined to the presentation model so QML
// never depends on repository types.
enum class MessageDeliveryState {
    None,
    Queued,
    Sending,
    Sent,
    Delivered,
    Read,
    Failed,
};

// Why a send could not be completed, so the UI can explain a Failed state
// without exposing internal diagnostics.
enum class MessageFailureReason {
    None,
    Network,
    StorageFull,
    Encryption,
    Expired,
};

// Security-system annotation attached to a message row (e.g. a peer device
// changed). Never carries plaintext; drives verification prompts only.
enum class MessageSecurityEvent {
    None,
    DeviceChanged,
    KeyChanged,
    Unverified,
};

struct Message {
    MessageDirection direction = MessageDirection::Incoming;
    QString body;
    QTime timestamp;
    MessageKind kind = MessageKind::Text;
    QDate date;
    QString stableId;
    MessageDeliveryState deliveryState = MessageDeliveryState::None;
    MessageFailureReason failureReason = MessageFailureReason::None;
    QString senderDevice;
    MessageSecurityEvent securityEvent = MessageSecurityEvent::None;
    // Who sent an incoming message, shown above the bubble in a group chat
    // where the bubble alone does not say. Empty in a one-to-one chat.
    QString senderName;
    // The account (hex) that sent an incoming message, so its bubble can wear
    // the sender's skin. Empty for anything this device sent.
    QString senderAccount;
    // True when stableId is the id every participant knows this message by,
    // so it can be answered and edited. History from before shared ids is not.
    bool sharedId = false;
    // The sender changed the text after sending it.
    bool edited = false;
    // A reply: the answered message's stableId, who wrote it (a display
    // name) and the excerpt it quotes. Empty for anything else.
    QString replyToId;
    QString quotedSender;
    QString quotedBody;

    // MessageKind::Attachment: what it carries as its sender described it (1
    // photo, 2 video, 3 audio, 4 file; 0 on any other kind), with the
    // sanitised name, the type, the size in bytes, the picture's size and the
    // length. Plain values only: the key and hash never leave C++.
    int attachmentKind = 0;
    QString fileName;
    QString mimeType;
    qint64 byteCount = 0;
    int mediaWidth = 0;
    int mediaHeight = 0;
    qint64 durationMs = 0;
    QVector<int> peaks; // an audio clip's waveform, 0…255 a bar
    bool hasPreview = false;
    // Where the bytes are: parts sent (outgoing) or held (incoming) of
    // transferTotal, and why they stopped (the stored failure reason:
    // 1 invalid, 2 no space, 3 cancelled by the sender, 4 could not send).
    AttachmentTransferState transferState = AttachmentTransferState::Transferring;
    int transferReason = 0;
    int transferDone = 0;
    int transferTotal = 0;
    // Who an incoming attachment's bytes come from, for "Waiting for Alice"
    // and "Alice stopped sending this".
    QString transferPeer;
    // Counts previews that arrived after the row was made, so a bubble
    // already on screen fetches the new one.
    int previewRevision = 0;

    // A text, emoji or attachment, as opposed to an event row.
    [[nodiscard]] bool isConversation() const
    {
        return kind == MessageKind::Text || kind == MessageKind::Emoji
            || kind == MessageKind::Attachment;
    }
    // Whether the local user may change the text: their own message, under a
    // shared id, and already taken by the relay (None: not tracked at all, as
    // in the reference mock), so an edit can never overtake it. A caption
    // never changes: the store only edits text rows.
    [[nodiscard]] bool isEditable() const
    {
        return direction == MessageDirection::Outgoing && isConversation()
            && kind != MessageKind::Attachment && sharedId
            && (deliveryState == MessageDeliveryState::None
                || deliveryState == MessageDeliveryState::Sent
                || deliveryState == MessageDeliveryState::Delivered
                || deliveryState == MessageDeliveryState::Read);
    }
    // An attachment this device is still sending: it can be stopped.
    [[nodiscard]] bool canCancelTransfer() const
    {
        return kind == MessageKind::Attachment && direction == MessageDirection::Outgoing
            && transferState == AttachmentTransferState::Transferring
            && deliveryState != MessageDeliveryState::Failed;
    }
    // An attachment this device sent that did not get through (its message
    // failed, or its bytes stopped): it can be sent again.
    [[nodiscard]] bool canRetryTransfer() const
    {
        return kind == MessageKind::Attachment && direction == MessageDirection::Outgoing
            && attachmentKind != 0
            && (transferState == AttachmentTransferState::Failed
                || transferState == AttachmentTransferState::Cancelled
                || (transferState == AttachmentTransferState::Transferring
                    && deliveryState == MessageDeliveryState::Failed));
    }
    // A photo or file whose bytes are all here: it can be saved.
    [[nodiscard]] bool canSaveAttachment() const
    {
        return kind == MessageKind::Attachment && (attachmentKind == 1 || attachmentKind == 4)
            && transferState == AttachmentTransferState::Ready;
    }
};

} // namespace OpenChat
