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
};

} // namespace OpenChat
