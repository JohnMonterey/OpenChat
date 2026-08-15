#pragma once

#include "repositories/OutboxRepository.h"

namespace OpenChat {

class SqlCipherDatabase;

class SqlCipherOutboxRepository final : public OutboxRepository
{
public:
    explicit SqlCipherOutboxRepository(SqlCipherDatabase &database);

    [[nodiscard]] Result<QVector<OutboxRecord>, RepositoryError>
    claimDue(qint64 nowMs, int limit, qint64 leaseUntilMs) override;
    [[nodiscard]] Result<void, RepositoryError>
    markAccepted(const EnvelopeId &envelopeId) override;
    [[nodiscard]] Result<void, RepositoryError>
    scheduleRetry(const EnvelopeId &envelopeId, int attemptCount, qint64 nextAttemptMs) override;

private:
    SqlCipherDatabase &m_database;
};

} // namespace OpenChat
