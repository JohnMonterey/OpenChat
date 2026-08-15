#pragma once

#include "repositories/ChatRepository.h"

namespace OpenChat {

class SqlCipherDatabase;

class SqlCipherChatRepository final : public ChatRepository
{
public:
    explicit SqlCipherChatRepository(SqlCipherDatabase &database);

    [[nodiscard]] Result<QVector<ConversationRecord>, RepositoryError> conversations() override;
    [[nodiscard]] Result<void, RepositoryError>
    upsertConversation(const ConversationRecord &conversation) override;
    [[nodiscard]] Result<QVector<MessageRecord>, RepositoryError>
    messages(const ConversationId &conversationId, int limit,
             const std::optional<MessageId> &before) override;
    [[nodiscard]] Result<void, RepositoryError>
    saveOutgoing(const MessageRecord &message, const OutboxRecord &outbox) override;
    [[nodiscard]] Result<void, RepositoryError>
    applyIncoming(const MessageRecord &message, const EnvelopeId &envelopeId,
                  quint64 watermark) override;
    [[nodiscard]] Result<void, RepositoryError>
    advanceDeliveryState(const MessageId &messageId, DeliveryState state) override;

private:
    SqlCipherDatabase &m_database;
};

} // namespace OpenChat
