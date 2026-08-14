#pragma once

#include "core/Result.h"
#include "domain/ChatTypes.h"
#include "repositories/RepositoryError.h"

namespace OpenChat {

class SyncRepository
{
public:
    virtual ~SyncRepository() = default;

    [[nodiscard]] virtual Result<SyncCursor, RepositoryError>
        cursor(const DeviceId &deviceId) = 0;
    [[nodiscard]] virtual Result<bool, RepositoryError>
        hasSeen(const EnvelopeId &envelopeId) = 0;
    [[nodiscard]] virtual Result<void, RepositoryError>
        recordSeen(const EnvelopeId &envelopeId, const DeviceId &senderDeviceId,
                   qint64 receivedAtMs) = 0;
    [[nodiscard]] virtual Result<void, RepositoryError>
        advanceWatermark(const DeviceId &deviceId, quint64 watermark, qint64 updatedAtMs) = 0;
};

} // namespace OpenChat
