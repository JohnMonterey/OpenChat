#include "KeyPackageService.h"

#include "RelayCrypto.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace OpenChat::Relay {

KeyPackageService::KeyPackageService(PostgresStore &store)
    : KeyPackageService(store, Policy{})
{
}

KeyPackageService::KeyPackageService(PostgresStore &store, Policy policy)
    : m_store(store)
    , m_policy(policy)
{
}

Result<void, RelayError> KeyPackageService::publish(const AccountId &accountId,
                                                    const DeviceId &deviceId,
                                                    QByteArrayView keyPackage)
{
    if (keyPackage.isEmpty() || keyPackage.size() > m_policy.maxPackageBytes)
        return Result<void, RelayError>::failure(RelayError::InvalidRequest);

    QSqlDatabase &db = m_store.database();

    QSqlQuery device(db);
    device.prepare(QStringLiteral("SELECT revoked_at_ms FROM devices WHERE device_id = ?"));
    device.addBindValue(deviceId.bytes());
    if (!device.exec())
        return Result<void, RelayError>::failure(RelayError::Internal);
    if (!device.next())
        return Result<void, RelayError>::failure(RelayError::NotFound);
    if (!device.value(0).isNull())
        return Result<void, RelayError>::failure(RelayError::Revoked);

    const qint64 now = m_store.nowMs();
    QSqlQuery insert(db);
    insert.prepare(QStringLiteral(
        "INSERT INTO key_packages (device_id, account_id, key_package, package_ref, "
        "created_at_ms, expires_at_ms) VALUES (?, ?, ?, ?, ?, ?)"));
    insert.addBindValue(deviceId.bytes());
    insert.addBindValue(accountId.bytes());
    insert.addBindValue(keyPackage.toByteArray());
    insert.addBindValue(sha256(keyPackage));
    insert.addBindValue(now);
    insert.addBindValue(now + m_policy.ttlMs);
    if (!insert.exec()) {
        const bool conflict = insert.lastError().nativeErrorCode() == QLatin1String("23505");
        return Result<void, RelayError>::failure(conflict ? RelayError::Conflict
                                                          : RelayError::Internal);
    }
    return Result<void, RelayError>::success();
}

Result<QByteArray, RelayError> KeyPackageService::claim(const DeviceId &targetDeviceId,
                                                        const DeviceId &claimingDeviceId)
{
    QSqlDatabase &db = m_store.database();

    QSqlQuery device(db);
    device.prepare(QStringLiteral("SELECT revoked_at_ms FROM devices WHERE device_id = ?"));
    device.addBindValue(targetDeviceId.bytes());
    if (!device.exec())
        return Result<QByteArray, RelayError>::failure(RelayError::Internal);
    if (!device.next())
        return Result<QByteArray, RelayError>::failure(RelayError::NotFound);
    if (!device.value(0).isNull())
        return Result<QByteArray, RelayError>::failure(RelayError::Revoked);

    // Atomically select-and-consume the oldest unclaimed, unexpired package in a
    // single statement. FOR UPDATE SKIP LOCKED lets concurrent claims each take a
    // distinct row (or none) without blocking, and guarantees a package is handed
    // out at most once — the standard PostgreSQL one-time-claim pattern.
    const qint64 now = m_store.nowMs();
    QSqlQuery claim(db);
    claim.prepare(QStringLiteral(
        "UPDATE key_packages SET claimed_at_ms = ?, claimed_by = ? "
        "WHERE key_package_id = ("
        "  SELECT key_package_id FROM key_packages "
        "  WHERE device_id = ? AND claimed_at_ms IS NULL AND expires_at_ms > ? "
        "  ORDER BY key_package_id LIMIT 1 FOR UPDATE SKIP LOCKED) "
        "RETURNING key_package"));
    claim.addBindValue(now);
    claim.addBindValue(claimingDeviceId.bytes());
    claim.addBindValue(targetDeviceId.bytes());
    claim.addBindValue(now);
    if (!claim.exec())
        return Result<QByteArray, RelayError>::failure(RelayError::Internal);
    if (!claim.next())
        return Result<QByteArray, RelayError>::failure(RelayError::NotFound);
    return Result<QByteArray, RelayError>::success(claim.value(0).toByteArray());
}

int KeyPackageService::availableCount(const DeviceId &targetDeviceId)
{
    QSqlQuery row(m_store.database());
    row.prepare(QStringLiteral(
        "SELECT count(*) FROM key_packages WHERE device_id = ? AND claimed_at_ms IS NULL "
        "AND expires_at_ms > ?"));
    row.addBindValue(targetDeviceId.bytes());
    row.addBindValue(m_store.nowMs());
    if (!row.exec() || !row.next())
        return -1;
    return row.value(0).toInt();
}

} // namespace OpenChat::Relay
