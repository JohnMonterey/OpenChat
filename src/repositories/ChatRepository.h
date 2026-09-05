#pragma once

#include <QVector>

#include <optional>

#include "core/Result.h"
#include "domain/ChatTypes.h"
#include "repositories/RepositoryError.h"

namespace OpenChat {

class ChatRepository
{
public:
    virtual ~ChatRepository() = default;

    [[nodiscard]] virtual Result<QVector<ConversationRecord>, RepositoryError>
        conversations() = 0;
    [[nodiscard]] virtual Result<void, RepositoryError>
        upsertConversation(const ConversationRecord &conversation) = 0;
    [[nodiscard]] virtual Result<QVector<MessageRecord>, RepositoryError>
        messages(const ConversationId &conversationId, int limit,
                 const std::optional<MessageId> &before) = 0;
    [[nodiscard]] virtual Result<void, RepositoryError>
        saveOutgoing(const MessageRecord &message, const OutboxRecord &outbox) = 0;
    [[nodiscard]] virtual Result<void, RepositoryError>
        applyIncoming(const MessageRecord &message, const EnvelopeId &envelopeId,
                      quint64 watermark) = 0;
    [[nodiscard]] virtual Result<void, RepositoryError>
        advanceDeliveryState(const MessageId &messageId, DeliveryState state) = 0;

    // Group conversations: the roster of other members and the title.
    [[nodiscard]] virtual Result<QVector<GroupMemberRecord>, RepositoryError>
        groupMembers(const ConversationId &conversationId) = 0;
    // Inserts or refreshes one member row (keyed by conversation + device).
    [[nodiscard]] virtual Result<void, RepositoryError>
        upsertGroupMember(const GroupMemberRecord &member) = 0;
    [[nodiscard]] virtual Result<void, RepositoryError>
        removeGroupMember(const ConversationId &conversationId, const DeviceId &deviceId) = 0;
    [[nodiscard]] virtual Result<void, RepositoryError>
        setConversationTitle(const ConversationId &conversationId, const QString &title) = 0;
    // Hides a group the local user left. Messages and members are kept.
    [[nodiscard]] virtual Result<void, RepositoryError>
        markConversationLeft(const ConversationId &conversationId, qint64 leftAtMs) = 0;
};

} // namespace OpenChat
