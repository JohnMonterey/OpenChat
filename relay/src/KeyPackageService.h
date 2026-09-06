#pragma once

#include "PostgresStore.h"
#include "RelayTypes.h"
#include "core/Result.h"
#include "domain/Identifiers.h"

#include <QByteArray>

namespace OpenChat::Relay {

// One-time KeyPackage publication and claim. Each published package is handed
// out to at most one claimer; the claim runs in a serializable transaction so
// concurrent claims cannot both succeed.
class KeyPackageService final
{
public:
    struct Policy final {
        qint64 ttlMs = 90LL * 24 * 60 * 60'000; // 90 days
        int maxPackageBytes = 262144;
    };

    explicit KeyPackageService(PostgresStore &store);
    KeyPackageService(PostgresStore &store, Policy policy);

    // Publishes a one-time KeyPackage for a device. Duplicate uploads (same
    // package bytes) are rejected as Conflict.
    [[nodiscard]] Result<void, RelayError>
    publish(const AccountId &accountId, const DeviceId &deviceId, QByteArrayView keyPackage);

    // Claims (and consumes) one unclaimed, unexpired KeyPackage for the target
    // device. Returns NotFound when none are available.
    [[nodiscard]] Result<QByteArray, RelayError>
    claim(const DeviceId &targetDeviceId, const DeviceId &claimingDeviceId);

    // Count of unclaimed, unexpired packages (for supply checks and metrics).
    [[nodiscard]] int availableCount(const DeviceId &targetDeviceId);

private:
    PostgresStore &m_store;
    Policy m_policy;
};

} // namespace OpenChat::Relay
