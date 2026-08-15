#include "AuthService.h"

#include "RelayCrypto.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace OpenChat::Relay {

namespace {

template <typename T>
Result<T, RelayError> fail(RelayError error)
{
    return Result<T, RelayError>::failure(error);
}

QByteArray newTokenString()
{
    return randomBytes(32).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

bool isUniqueViolation(const QSqlQuery &query)
{
    return query.lastError().nativeErrorCode() == QLatin1String("23505");
}

} // namespace

AuthService::AuthService(PostgresStore &store)
    : AuthService(store, Policy{})
{
}

AuthService::AuthService(PostgresStore &store, Policy policy)
    : m_store(store)
    , m_policy(policy)
{
}

Result<void, RelayError> AuthService::registerAccount(const AccountId &accountId,
                                                      const QString &handle,
                                                      const DeviceId &deviceId,
                                                      QByteArrayView signingKey,
                                                      QByteArrayView credential)
{
    if (handle.trimmed().isEmpty() || handle.size() > 64 || signingKey.size() != 32
        || credential.isEmpty() || credential.size() > 65536)
        return fail<void>(RelayError::InvalidRequest);

    QSqlDatabase &db = m_store.database();
    if (!db.transaction())
        return fail<void>(RelayError::Internal);

    QSqlQuery account(db);
    account.prepare(QStringLiteral(
        "INSERT INTO accounts (account_id, handle, created_at_ms) VALUES (?, ?, ?)"));
    account.addBindValue(accountId.bytes());
    account.addBindValue(handle);
    account.addBindValue(m_store.nowMs());
    if (!account.exec()) {
        const bool conflict = isUniqueViolation(account);
        db.rollback();
        return fail<void>(conflict ? RelayError::Conflict : RelayError::Internal);
    }

    QSqlQuery device(db);
    device.prepare(QStringLiteral(
        "INSERT INTO devices (device_id, account_id, signing_key, credential, created_at_ms) "
        "VALUES (?, ?, ?, ?, ?)"));
    device.addBindValue(deviceId.bytes());
    device.addBindValue(accountId.bytes());
    device.addBindValue(signingKey.toByteArray());
    device.addBindValue(credential.toByteArray());
    device.addBindValue(m_store.nowMs());
    if (!device.exec()) {
        const bool conflict = isUniqueViolation(device);
        db.rollback();
        return fail<void>(conflict ? RelayError::Conflict : RelayError::Internal);
    }

    if (!db.commit()) {
        db.rollback();
        return fail<void>(RelayError::Internal);
    }
    return Result<void, RelayError>::success();
}

Result<QByteArray, RelayError> AuthService::issueChallenge(const AccountId &accountId,
                                                           const DeviceId &deviceId,
                                                           int protocolVersion)
{
    QSqlDatabase &db = m_store.database();

    QSqlQuery device(db);
    device.prepare(QStringLiteral(
        "SELECT revoked_at_ms FROM devices WHERE device_id = ? AND account_id = ?"));
    device.addBindValue(deviceId.bytes());
    device.addBindValue(accountId.bytes());
    if (!device.exec())
        return fail<QByteArray>(RelayError::Internal);
    if (!device.next())
        return fail<QByteArray>(RelayError::NotFound);
    if (!device.value(0).isNull())
        return fail<QByteArray>(RelayError::Revoked);

    const QByteArray challenge = randomBytes(32);
    if (challenge.size() != 32)
        return fail<QByteArray>(RelayError::Internal);

    const qint64 now = m_store.nowMs();

    // Opportunistically sweep expired challenges so the table cannot grow
    // without bound. (Request-rate abuse control itself is enforced upstream at
    // the reverse proxy and is tracked for the hardening phase.)
    QSqlQuery sweep(db);
    sweep.prepare(QStringLiteral("DELETE FROM auth_challenges WHERE expires_at_ms < ?"));
    sweep.addBindValue(now);
    sweep.exec();

    QSqlQuery insert(db);
    insert.prepare(QStringLiteral(
        "INSERT INTO auth_challenges (challenge, account_id, device_id, protocol_version, "
        "created_at_ms, expires_at_ms) VALUES (?, ?, ?, ?, ?, ?)"));
    insert.addBindValue(challenge);
    insert.addBindValue(accountId.bytes());
    insert.addBindValue(deviceId.bytes());
    insert.addBindValue(protocolVersion);
    insert.addBindValue(now);
    insert.addBindValue(now + m_policy.challengeTtlMs);
    if (!insert.exec())
        return fail<QByteArray>(RelayError::Internal);

    return Result<QByteArray, RelayError>::success(challenge);
}

Result<AuthTokens, RelayError> AuthService::completeChallenge(const AccountId &accountId,
                                                             const DeviceId &deviceId,
                                                             QByteArrayView challenge,
                                                             QByteArrayView signature,
                                                             QByteArrayView context)
{
    // Bound the client-supplied context to match DeviceIdentity::signChallenge's
    // contract (non-empty, at most 1024 bytes) so an unbounded blob cannot be
    // fed into signature reconstruction.
    if (challenge.size() != 32 || signature.size() != 64 || context.isEmpty()
        || context.size() > 1024)
        return fail<AuthTokens>(RelayError::InvalidRequest);

    QSqlDatabase &db = m_store.database();

    // Fetch the device signing key (and revocation) up front.
    QSqlQuery device(db);
    device.prepare(QStringLiteral(
        "SELECT signing_key, revoked_at_ms FROM devices WHERE device_id = ? AND account_id = ?"));
    device.addBindValue(deviceId.bytes());
    device.addBindValue(accountId.bytes());
    if (!device.exec())
        return fail<AuthTokens>(RelayError::Internal);
    if (!device.next())
        return fail<AuthTokens>(RelayError::NotFound);
    const QByteArray signingKey = device.value(0).toByteArray();
    if (!device.value(1).isNull())
        return fail<AuthTokens>(RelayError::Revoked);

    // Validate the challenge row (exists, bound, unconsumed, unexpired).
    QSqlQuery row(db);
    row.prepare(QStringLiteral(
        "SELECT expires_at_ms, consumed_at_ms FROM auth_challenges "
        "WHERE challenge = ? AND account_id = ? AND device_id = ?"));
    row.addBindValue(challenge.toByteArray());
    row.addBindValue(accountId.bytes());
    row.addBindValue(deviceId.bytes());
    if (!row.exec())
        return fail<AuthTokens>(RelayError::Internal);
    if (!row.next())
        return fail<AuthTokens>(RelayError::Unauthorized);
    const qint64 expiresAt = row.value(0).toLongLong();
    if (!row.value(1).isNull())
        return fail<AuthTokens>(RelayError::Unauthorized); // already consumed
    if (m_store.nowMs() >= expiresAt)
        return fail<AuthTokens>(RelayError::Expired);

    // Verify the signature over the exact bytes the device signed.
    const QByteArray message = challengeSigningMessage(challenge, context);
    if (!verifyEd25519(signingKey, message, signature))
        return fail<AuthTokens>(RelayError::Unauthorized);

    if (!db.transaction())
        return fail<AuthTokens>(RelayError::Internal);

    // Single-use consumption: the WHERE ... IS NULL guard plus rows-affected==1
    // makes a replayed challenge lose the race.
    QSqlQuery consume(db);
    consume.prepare(QStringLiteral(
        "UPDATE auth_challenges SET consumed_at_ms = ? "
        "WHERE challenge = ? AND consumed_at_ms IS NULL"));
    consume.addBindValue(m_store.nowMs());
    consume.addBindValue(challenge.toByteArray());
    if (!consume.exec() || consume.numRowsAffected() != 1) {
        db.rollback();
        return fail<AuthTokens>(RelayError::Unauthorized);
    }

    auto tokens = issueFamilyTokens(accountId, deviceId, std::nullopt);
    if (!tokens.hasValue()) {
        db.rollback();
        return tokens;
    }
    if (!db.commit()) {
        db.rollback();
        return fail<AuthTokens>(RelayError::Internal);
    }
    return tokens;
}

Result<AuthTokens, RelayError> AuthService::refresh(QByteArrayView refreshToken)
{
    if (refreshToken.isEmpty())
        return fail<AuthTokens>(RelayError::InvalidRequest);

    QSqlDatabase &db = m_store.database();
    const QByteArray hash = sha256(refreshToken);

    QSqlQuery row(db);
    row.prepare(QStringLiteral(
        "SELECT rt.family_id, rt.device_id, rt.used_at_ms, rt.expires_at_ms, tf.revoked, "
        "d.revoked_at_ms, tf.account_id "
        "FROM refresh_tokens rt "
        "JOIN token_families tf ON tf.family_id = rt.family_id "
        "JOIN devices d ON d.device_id = rt.device_id "
        "WHERE rt.token_hash = ?"));
    row.addBindValue(hash);
    if (!row.exec())
        return fail<AuthTokens>(RelayError::Internal);
    if (!row.next())
        return fail<AuthTokens>(RelayError::Unauthorized);

    const QByteArray familyId = row.value(0).toByteArray();
    const QByteArray deviceBytes = row.value(1).toByteArray();
    const bool used = !row.value(2).isNull();
    const qint64 expiresAt = row.value(3).toLongLong();
    const bool familyRevoked = row.value(4).toBool();
    const bool deviceRevoked = !row.value(5).isNull();
    const QByteArray accountBytes = row.value(6).toByteArray();

    auto revokeFamily = [&] {
        QSqlQuery revoke(db);
        revoke.prepare(QStringLiteral(
            "UPDATE token_families SET revoked = TRUE WHERE family_id = ?"));
        revoke.addBindValue(familyId);
        revoke.exec();
    };

    if (deviceRevoked)
        return fail<AuthTokens>(RelayError::Revoked);
    if (familyRevoked)
        return fail<AuthTokens>(RelayError::Unauthorized);
    if (used) {
        // Reuse of a rotated refresh token: treat the family as compromised.
        revokeFamily();
        return fail<AuthTokens>(RelayError::TokenReuse);
    }
    if (m_store.nowMs() >= expiresAt)
        return fail<AuthTokens>(RelayError::Expired);

    const auto accountId = AccountId::fromBytes(accountBytes);
    const auto deviceId = DeviceId::fromBytes(deviceBytes);
    if (!accountId || !deviceId)
        return fail<AuthTokens>(RelayError::Internal);

    if (!db.transaction())
        return fail<AuthTokens>(RelayError::Internal);

    QSqlQuery rotate(db);
    rotate.prepare(QStringLiteral(
        "UPDATE refresh_tokens SET used_at_ms = ? WHERE token_hash = ? AND used_at_ms IS NULL"));
    rotate.addBindValue(m_store.nowMs());
    rotate.addBindValue(hash);
    if (!rotate.exec()) {
        // A genuine database error is not evidence of theft: roll back and fail.
        db.rollback();
        return fail<AuthTokens>(RelayError::Internal);
    }
    if (rotate.numRowsAffected() != 1) {
        // The token was already rotated concurrently: treat as reuse.
        revokeFamily();
        db.commit();
        return fail<AuthTokens>(RelayError::TokenReuse);
    }

    auto tokens = issueFamilyTokens(*accountId, *deviceId, familyId);
    if (!tokens.hasValue()) {
        db.rollback();
        return tokens;
    }
    if (!db.commit()) {
        db.rollback();
        return fail<AuthTokens>(RelayError::Internal);
    }
    return tokens;
}

Result<AuthTokens, RelayError>
AuthService::issueFamilyTokens(const AccountId &accountId, const DeviceId &deviceId,
                               std::optional<QByteArray> existingFamily)
{
    QSqlDatabase &db = m_store.database();
    const qint64 now = m_store.nowMs();

    QByteArray familyId;
    if (existingFamily) {
        familyId = *existingFamily;
    } else {
        familyId = randomBytes(16);
        if (familyId.size() != 16)
            return fail<AuthTokens>(RelayError::Internal);
        QSqlQuery family(db);
        family.prepare(QStringLiteral(
            "INSERT INTO token_families (family_id, device_id, account_id, created_at_ms) "
            "VALUES (?, ?, ?, ?)"));
        family.addBindValue(familyId);
        family.addBindValue(deviceId.bytes());
        family.addBindValue(accountId.bytes());
        family.addBindValue(now);
        if (!family.exec())
            return fail<AuthTokens>(RelayError::Internal);
    }

    AuthTokens tokens;
    tokens.accessToken = newTokenString();
    tokens.refreshToken = newTokenString();
    tokens.accessExpiresAtMs = now + m_policy.accessTtlMs;
    tokens.refreshExpiresAtMs = now + m_policy.refreshTtlMs;
    if (tokens.accessToken.isEmpty() || tokens.refreshToken.isEmpty())
        return fail<AuthTokens>(RelayError::Internal);

    QSqlQuery access(db);
    access.prepare(QStringLiteral(
        "INSERT INTO access_tokens (token_hash, family_id, device_id, account_id, created_at_ms, "
        "expires_at_ms) VALUES (?, ?, ?, ?, ?, ?)"));
    access.addBindValue(sha256(tokens.accessToken));
    access.addBindValue(familyId);
    access.addBindValue(deviceId.bytes());
    access.addBindValue(accountId.bytes());
    access.addBindValue(now);
    access.addBindValue(tokens.accessExpiresAtMs);
    if (!access.exec())
        return fail<AuthTokens>(RelayError::Internal);

    QSqlQuery refreshRow(db);
    refreshRow.prepare(QStringLiteral(
        "INSERT INTO refresh_tokens (token_hash, family_id, device_id, created_at_ms, "
        "expires_at_ms) VALUES (?, ?, ?, ?, ?)"));
    refreshRow.addBindValue(sha256(tokens.refreshToken));
    refreshRow.addBindValue(familyId);
    refreshRow.addBindValue(deviceId.bytes());
    refreshRow.addBindValue(now);
    refreshRow.addBindValue(tokens.refreshExpiresAtMs);
    if (!refreshRow.exec())
        return fail<AuthTokens>(RelayError::Internal);

    return Result<AuthTokens, RelayError>::success(tokens);
}

std::optional<AuthenticatedDevice> AuthService::authenticate(QByteArrayView accessToken)
{
    if (accessToken.isEmpty())
        return std::nullopt;

    QSqlDatabase &db = m_store.database();
    QSqlQuery row(db);
    row.prepare(QStringLiteral(
        "SELECT at.device_id, at.account_id, at.expires_at_ms, tf.revoked, d.revoked_at_ms "
        "FROM access_tokens at "
        "JOIN token_families tf ON tf.family_id = at.family_id "
        "JOIN devices d ON d.device_id = at.device_id "
        "WHERE at.token_hash = ?"));
    row.addBindValue(sha256(accessToken));
    if (!row.exec() || !row.next())
        return std::nullopt;

    const qint64 expiresAt = row.value(2).toLongLong();
    if (m_store.nowMs() >= expiresAt)
        return std::nullopt;
    if (row.value(3).toBool())
        return std::nullopt; // family revoked
    if (!row.value(4).isNull())
        return std::nullopt; // device revoked

    const auto deviceId = DeviceId::fromBytes(row.value(0).toByteArray());
    const auto accountId = AccountId::fromBytes(row.value(1).toByteArray());
    if (!deviceId || !accountId)
        return std::nullopt;
    return AuthenticatedDevice{*accountId, *deviceId};
}

Result<void, RelayError> AuthService::revokeDevice(const DeviceId &deviceId)
{
    QSqlDatabase &db = m_store.database();
    if (!db.transaction())
        return fail<void>(RelayError::Internal);

    QSqlQuery device(db);
    device.prepare(QStringLiteral(
        "UPDATE devices SET revoked_at_ms = ? WHERE device_id = ? AND revoked_at_ms IS NULL"));
    device.addBindValue(m_store.nowMs());
    device.addBindValue(deviceId.bytes());
    if (!device.exec()) {
        db.rollback();
        return fail<void>(RelayError::Internal);
    }

    QSqlQuery families(db);
    families.prepare(QStringLiteral(
        "UPDATE token_families SET revoked = TRUE WHERE device_id = ?"));
    families.addBindValue(deviceId.bytes());
    if (!families.exec()) {
        db.rollback();
        return fail<void>(RelayError::Internal);
    }

    if (!db.commit()) {
        db.rollback();
        return fail<void>(RelayError::Internal);
    }
    return Result<void, RelayError>::success();
}

bool AuthService::isDeviceRevoked(const DeviceId &deviceId)
{
    QSqlQuery row(m_store.database());
    row.prepare(QStringLiteral("SELECT revoked_at_ms FROM devices WHERE device_id = ?"));
    row.addBindValue(deviceId.bytes());
    if (!row.exec() || !row.next())
        return true; // unknown device is treated as unusable
    return !row.value(0).isNull();
}

} // namespace OpenChat::Relay
