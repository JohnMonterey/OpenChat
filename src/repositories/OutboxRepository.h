#pragma once

#include <QVector>

#include "core/Result.h"
#include "domain/ChatTypes.h"
#include "repositories/RepositoryError.h"

namespace OpenChat {

class OutboxRepository
{
public:
    virtual ~OutboxRepository() = default;

    [[nodiscard]] virtual Result<QVector<OutboxRecord>, RepositoryError>
        claimDue(qint64 nowMs, int limit, qint64 leaseUntilMs) = 0;
    [[nodiscard]] virtual Result<void, RepositoryError>
        markAccepted(const EnvelopeId &envelopeId) = 0;
    [[nodiscard]] virtual Result<void, RepositoryError>
        scheduleRetry(const EnvelopeId &envelopeId, int attemptCount, qint64 nextAttemptMs) = 0;
};

} // namespace OpenChat
