#pragma once

#include <QVector>

#include "core/Result.h"
#include "domain/ChatTypes.h"
#include "repositories/RepositoryError.h"

namespace OpenChat {

[[nodiscard]] constexpr qint64 retryDelayMs(int attemptCount, int jitterMs) noexcept
{
    const int safeAttempt = attemptCount < 0 ? 0 : attemptCount;
    const qint64 exponential = safeAttempt >= 9 ? 300'000 : (qint64(1'000) << safeAttempt);
    const qint64 capped = exponential > 300'000 ? 300'000 : exponential;
    const int safeJitter = jitterMs < 0 ? 0 : (jitterMs > 1'000 ? 1'000 : jitterMs);
    return capped + safeJitter;
}

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
