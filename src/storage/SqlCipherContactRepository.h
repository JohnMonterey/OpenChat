#pragma once

#include "repositories/ContactRepository.h"

namespace OpenChat {

class SqlCipherDatabase;

class SqlCipherContactRepository final : public ContactRepository
{
public:
    explicit SqlCipherContactRepository(SqlCipherDatabase &database);

    [[nodiscard]] Result<QVector<ContactRecord>, RepositoryError> contacts() override;
    [[nodiscard]] Result<std::optional<ContactRecord>, RepositoryError>
    find(const AccountId &accountId) override;
    [[nodiscard]] Result<void, RepositoryError>
    recordOutgoingRequest(const ContactRecord &contact) override;
    [[nodiscard]] Result<void, RepositoryError>
    recordIncomingRequest(const ContactRecord &contact) override;
    [[nodiscard]] Result<void, RepositoryError>
    markAccepted(const AccountId &accountId, const ConversationId &conversationId,
                 qint64 updatedAtMs) override;
    [[nodiscard]] Result<void, RepositoryError>
    block(const AccountId &accountId, qint64 updatedAtMs) override;
    [[nodiscard]] Result<void, RepositoryError> unblock(const AccountId &accountId) override;
    [[nodiscard]] Result<void, RepositoryError> remove(const AccountId &accountId) override;

private:
    SqlCipherDatabase &m_database;
};

} // namespace OpenChat
