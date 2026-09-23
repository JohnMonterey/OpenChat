#pragma once

#include <QByteArray>
#include <QString>

#include <optional>

#include "domain/Identifiers.h"

namespace OpenChat {

enum class ConversationKind {
    Direct,
    Group,
};

enum class ContentKind {
    Text,
    Emoji,
    Attachment,
    System,
};

enum class MessageFlow {
    Incoming,
    Outgoing,
};

enum class DeliveryState {
    Draft,
    Queued,
    Sending,
    Sent,
    Delivered,
    Read,
    Failed,
};

enum class VerificationState {
    Unverified,
    Verified,
    Changed,
    Revoked,
};

enum class OutboxState {
    Pending,
    Leased,
    Accepted,
    Failed,
};

[[nodiscard]] constexpr bool canTransition(DeliveryState from, DeliveryState to) noexcept
{
    if (from == to)
        return true;

    switch (from) {
    case DeliveryState::Draft:
        return to == DeliveryState::Queued || to == DeliveryState::Failed;
    case DeliveryState::Queued:
        return to == DeliveryState::Sending || to == DeliveryState::Failed;
    case DeliveryState::Sending:
        return to == DeliveryState::Sent || to == DeliveryState::Failed;
    case DeliveryState::Sent:
        return to == DeliveryState::Delivered;
    case DeliveryState::Delivered:
        return to == DeliveryState::Read;
    case DeliveryState::Read:
    case DeliveryState::Failed:
        return false;
    }
    return false;
}

struct ConversationRecord final {
    ConversationId id;
    QByteArray mlsGroupId;
    QString title;
    ConversationKind kind = ConversationKind::Direct;
    qint64 createdAtMs = 0;
    // Non-zero once the local user left this (group) conversation. The row is
    // kept, hidden, so late envelopes from members who have not yet heard can
    // still be stored; it is never shown again.
    qint64 leftAtMs = 0;
};

// One other member of a group conversation: the device every envelope into the
// group is addressed to, the account it belongs to, and the name the inviter
// knew them by (shown only when they are not a local contact).
struct GroupMemberRecord final {
    ConversationId conversationId;
    AccountId accountId;
    DeviceId deviceId;
    QString displayName;
    qint64 joinedAtMs = 0;
};

struct MessageRecord final {
    MessageId id;
    ConversationId conversationId;
    DeviceId senderDeviceId;
    MessageFlow flow = MessageFlow::Incoming;
    ContentKind kind = ContentKind::Text;
    QString body;
    qint64 sentAtMs = 0;
    DeliveryState deliveryState = DeliveryState::Draft;
    std::optional<quint64> serverSequence;
    // A reply names the message it answers here, and carries who wrote that
    // message and an excerpt of it, so the quote shows even when the answered
    // message is not held locally.
    std::optional<MessageId> replyToId;
    // True when `id` is the one both ends derive from the ciphertext, so the
    // peer can refer to this message (reply to it, receive its edits). Rows
    // from before shared ids carry one the peer never saw.
    bool sharedId = false;
    // When the sender last changed the text; 0 if it never was.
    qint64 editedAtMs = 0;
    std::optional<DeviceId> quotedSenderDeviceId;
    QString quotedBody;
};

// The message a reply answers, as the reply sends it along.
struct MessageQuote final {
    MessageId target;
    DeviceId sender;
    QString body;
};

struct OutboxRecord final {
    EnvelopeId envelopeId;
    MessageId messageId;
    ConversationId conversationId;
    QByteArray envelope;
    int attemptCount = 0;
    qint64 nextAttemptMs = 0;
    qint64 leaseUntilMs = 0;
    OutboxState state = OutboxState::Pending;
};

struct SyncCursor final {
    DeviceId deviceId;
    quint64 serverWatermark = 0;
    qint64 updatedAtMs = 0;
};

} // namespace OpenChat
