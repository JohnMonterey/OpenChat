#pragma once

#include "PostgresStore.h"
#include "RelayTypes.h"
#include "core/Result.h"
#include "domain/Identifiers.h"

#include <QByteArray>
#include <optional>

namespace OpenChat::Relay {

// Device-key authentication: account/device registration, single-use signed
// challenges, and rotating token families with reuse detection. All secrets are
// stored only as hashes; token plaintext leaves the relay exactly once.
class AuthService final
{
public:
    struct Policy final {
        qint64 challengeTtlMs = 120'000;                 // 2 minutes
        qint64 accessTtlMs = 15 * 60'000;                // 15 minutes
        qint64 refreshTtlMs = 30LL * 24 * 60 * 60'000;   // 30 days
    };

    explicit AuthService(PostgresStore &store);
    AuthService(PostgresStore &store, Policy policy);

    // Registers a new account and its first device. Public material only.
    [[nodiscard]] Result<void, RelayError>
    registerAccount(const AccountId &accountId, const QString &handle, const DeviceId &deviceId,
                    QByteArrayView signingKey, QByteArrayView credential);

    // Issues a fresh single-use challenge bound to the account/device/version.
    [[nodiscard]] Result<QByteArray, RelayError>
    issueChallenge(const AccountId &accountId, const DeviceId &deviceId, int protocolVersion);

    // Verifies a signed challenge and, on success, issues a new token family.
    [[nodiscard]] Result<AuthTokens, RelayError>
    completeChallenge(const AccountId &accountId, const DeviceId &deviceId,
                      QByteArrayView challenge, QByteArrayView signature, QByteArrayView context);

    // Rotates a refresh token. Presenting an already-used or revoked-family
    // token revokes the whole family and is rejected.
    [[nodiscard]] Result<AuthTokens, RelayError> refresh(QByteArrayView refreshToken);

    // Resolves a bearer access token to a device identity, or nullopt if the
    // token is unknown, expired, family-revoked, or the device is revoked.
    [[nodiscard]] std::optional<AuthenticatedDevice> authenticate(QByteArrayView accessToken);

    // Marks a device revoked and revokes all of its token families.
    [[nodiscard]] Result<void, RelayError> revokeDevice(const DeviceId &deviceId);

    [[nodiscard]] bool isDeviceRevoked(const DeviceId &deviceId);

private:
    [[nodiscard]] Result<AuthTokens, RelayError>
    issueFamilyTokens(const AccountId &accountId, const DeviceId &deviceId,
                      std::optional<QByteArray> existingFamily);

    PostgresStore &m_store;
    Policy m_policy;
};

} // namespace OpenChat::Relay
