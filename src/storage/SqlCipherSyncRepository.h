#pragma once

#include "repositories/SyncRepository.h"

namespace OpenChat {

class SqlCipherDatabase;

class SqlCipherSyncRepository final : public SyncRepository
{
public:
    explicit SqlCipherSyncRepository(SqlCipherDatabase &database);

    [[nodiscard]] Result<SyncCursor, RepositoryError> cursor(const DeviceId &deviceId) override;
    [[nodiscard]] Result<bool, RepositoryError> hasSeen(const EnvelopeId &envelopeId) override;
    [[nodiscard]] Result<void, RepositoryError>
    recordSeen(const EnvelopeId &envelopeId, const DeviceId &senderDeviceId,
               qint64 receivedAtMs) override;
    [[nodiscard]] Result<void, RepositoryError>
    advanceWatermark(const DeviceId &deviceId, quint64 watermark, qint64 updatedAtMs) override;

private:
    SqlCipherDatabase &m_database;
};

} // namespace OpenChat
