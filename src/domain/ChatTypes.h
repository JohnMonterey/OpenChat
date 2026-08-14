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
    std::optional<MessageId> replyToId;
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
