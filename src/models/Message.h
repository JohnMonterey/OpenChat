#pragma once

#include <QDate>
#include <QString>
#include <QTime>

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

    // A text or emoji, as opposed to an event row.
    [[nodiscard]] bool isConversation() const
    {
        return kind == MessageKind::Text || kind == MessageKind::Emoji;
    }
    // Whether the local user may change the text: their own message, under a
    // shared id, and already taken by the relay (None: not tracked at all, as
    // in the reference mock), so an edit can never overtake it.
    [[nodiscard]] bool isEditable() const
    {
        return direction == MessageDirection::Outgoing && isConversation() && sharedId
            && (deliveryState == MessageDeliveryState::None
                || deliveryState == MessageDeliveryState::Sent
                || deliveryState == MessageDeliveryState::Delivered
                || deliveryState == MessageDeliveryState::Read);
    }
};

} // namespace OpenChat
