#include "DirectoryService.h"

#include "RelayCrypto.h"

#include <QSqlQuery>
#include <QVariant>

namespace OpenChat::Relay {

namespace {

template <typename T>
Result<T, RelayError> fail(RelayError error)
{
    return Result<T, RelayError>::failure(error);
}

} // namespace

DirectoryService::DirectoryService(PostgresStore &store)
    : DirectoryService(store, Policy{})
{
}

DirectoryService::DirectoryService(PostgresStore &store, Policy policy)
    : m_store(store)
    , m_policy(policy)
{
}

Result<AccountDirectoryEntry, RelayError> DirectoryService::loadActiveDevices(const AccountId &account)
{
    QSqlQuery devices(m_store.database());
    // Active devices only: a revoked device is excluded from discovery so a
    // resolved handle never routes to a key its owner has retired. Deterministic
    // ordering keeps the response stable across calls.
    devices.prepare(QStringLiteral(
        "SELECT device_id, signing_key FROM devices "
        "WHERE account_id = ? AND revoked_at_ms IS NULL "
        "ORDER BY created_at_ms, device_id"));
    devices.addBindValue(account.bytes());
    if (!devices.exec())
        return fail<AccountDirectoryEntry>(RelayError::Internal);

    AccountDirectoryEntry entry{account, {}};
    while (devices.next()) {
        const auto deviceId = DeviceId::fromBytes(devices.value(0).toByteArray());
        if (!deviceId)
            continue; // malformed row; skip rather than surface it
        entry.devices.append(DirectoryDevice{*deviceId, devices.value(1).toByteArray()});
    }
    if (entry.devices.isEmpty())
        return fail<AccountDirectoryEntry>(RelayError::NotFound);
    return Result<AccountDirectoryEntry, RelayError>::success(entry);
}

Result<AccountDirectoryEntry, RelayError> DirectoryService::resolveHandle(const QString &handle)
{
    // Exact match only. A handle outside the stored length bounds can never match
    // a registered handle, so treat it as simply not found (no oracle, no probe).
    if (handle.isEmpty() || handle.size() > 64)
        return fail<AccountDirectoryEntry>(RelayError::NotFound);

    QSqlQuery account(m_store.database());
    account.prepare(QStringLiteral("SELECT account_id FROM accounts WHERE handle = ?"));
    account.addBindValue(handle);
    if (!account.exec())
        return fail<AccountDirectoryEntry>(RelayError::Internal);
    if (!account.next())
        return fail<AccountDirectoryEntry>(RelayError::NotFound);

    const auto accountId = AccountId::fromBytes(account.value(0).toByteArray());
    if (!accountId)
        return fail<AccountDirectoryEntry>(RelayError::Internal);
    return loadActiveDevices(*accountId);
}

Result<QByteArray, RelayError> DirectoryService::createInvite(const AccountId &account,
                                                              qint64 ttlMs)
{
    if (ttlMs <= 0 || ttlMs > m_policy.maxInviteTtlMs)
        return fail<QByteArray>(RelayError::InvalidRequest);

    const QByteArray token = randomBytes(32);
    if (token.size() != 32)
        return fail<QByteArray>(RelayError::Internal);

    const qint64 now = m_store.nowMs();
    QSqlQuery insert(m_store.database());
    insert.prepare(QStringLiteral(
        "INSERT INTO invites (token_hash, account_id, created_at_ms, expires_at_ms) "
        "VALUES (?, ?, ?, ?)"));
    insert.addBindValue(sha256(token)); // store only the hash; the plaintext never persists
    insert.addBindValue(account.bytes());
    insert.addBindValue(now);
    insert.addBindValue(now + ttlMs);
    if (!insert.exec())
        return fail<QByteArray>(RelayError::Internal);

    return Result<QByteArray, RelayError>::success(token);
}

Result<QByteArray, RelayError> DirectoryService::createInvite(const AccountId &account)
{
    return createInvite(account, m_policy.inviteTtlMs);
}

Result<AccountDirectoryEntry, RelayError> DirectoryService::redeemInvite(const QByteArray &token)
{
    // A malformed token cannot match any stored hash; treat it exactly like an
    // unknown one so redemption never becomes an oracle.
    if (token.isEmpty())
        return fail<AccountDirectoryEntry>(RelayError::NotFound);

    const qint64 now = m_store.nowMs();
    // Single-use, expiry-guarded consume: the WHERE consumed_at_ms IS NULL AND
    // expires_at_ms > now guard plus RETURNING makes the token redeemable exactly
    // once and never after expiry. A second redeem, an expired token, and an
    // unknown token all match zero rows and are indistinguishable to the caller.
    QSqlQuery consume(m_store.database());
    consume.prepare(QStringLiteral(
        "UPDATE invites SET consumed_at_ms = ? "
        "WHERE token_hash = ? AND consumed_at_ms IS NULL AND expires_at_ms > ? "
        "RETURNING account_id"));
    consume.addBindValue(now);
    consume.addBindValue(sha256(token));
    consume.addBindValue(now);
    if (!consume.exec())
        return fail<AccountDirectoryEntry>(RelayError::Internal);
    if (!consume.next())
        return fail<AccountDirectoryEntry>(RelayError::NotFound);

    const auto account = AccountId::fromBytes(consume.value(0).toByteArray());
    if (!account)
        return fail<AccountDirectoryEntry>(RelayError::Internal);
    return loadActiveDevices(*account);
}

} // namespace OpenChat::Relay
