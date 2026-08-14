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
};

} // namespace OpenChat
