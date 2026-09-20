#pragma once

#include "PostgresStore.h"
#include "RelayCrypto.h"
#include "RelayTypes.h"
#include "core/Result.h"
#include "domain/Identifiers.h"

#include <QByteArray>
#include <optional>

namespace OpenChat::Relay {

// Device-key authentication: account/device registration, single-use signed
// challenges, and rotating token families with reuse detection. All secrets are
// stored only as hashes; token plaintext leaves the relay exactly once.
//
// Accounts are additionally protected by a password, which the relay never sees:
// the client sends a locally stretched 32-byte password key and the relay keeps
// only a salted Argon2id hash of it. The password gates the two operations that
// bind a device key to a handle -- creating the account, and enrolling a new
// device into it (loginWithPassword). Everything after that is still proven by
// the device's Ed25519 key, exactly as before.
class AuthService final
{
public:
    struct Policy final {
        qint64 challengeTtlMs = 120'000;                 // 2 minutes
        qint64 accessTtlMs = 15 * 60'000;                // 15 minutes
        qint64 refreshTtlMs = 30LL * 24 * 60 * 60'000;   // 30 days
        // Failed password logins tolerated per handle per window before further
        // attempts are refused without being evaluated.
        int maxLoginFailures = 8;
        qint64 loginWindowMs = 15 * 60'000;              // 15 minutes
        PasswordHashParams passwordParams;               // cost for NEW hashes
    };

    // The only client-side password stretch this relay accepts (see
    // security/PasswordKey.h). Stored per account so a future version can coexist.
    static constexpr int supportedPasswordKdf = 1;

    explicit AuthService(PostgresStore &store);
    AuthService(PostgresStore &store, Policy policy);

    // Registers a new account, its password and its first device. `handle` is
    // canonicalized (domain/Handle.h); a handle that is not canonicalizable is
    // InvalidRequest and one that is taken -- by any account, in any letter case
    // -- is Conflict. `passwordKey` must be exactly passwordKeyBytes.
    [[nodiscard]] Result<void, RelayError>
    registerAccount(const AccountId &accountId, const QString &handle, const DeviceId &deviceId,
                    QByteArrayView signingKey, QByteArrayView credential,
                    QByteArrayView passwordKey, int passwordKdf = supportedPasswordKdf);

    // Password login from an installation that has no device key for the account
    // yet: verifies the password key and, atomically, enrolls `deviceId` as the
    // account's device while retiring every other device of the account (their
    // ids are reported so live sockets can be dropped). The client model is one
    // active device per account, so signing in here signs the account out
    // everywhere else.
    //
    // An unknown handle, a passwordless (pre-password) account and a wrong
    // password are indistinguishable: all are Unauthorized, all cost one hash and
    // all count against the handle's failure budget. RateLimited is returned,
    // without evaluating the password, once that budget is spent.
    [[nodiscard]] Result<PasswordLogin, RelayError>
    loginWithPassword(const QString &handle, QByteArrayView passwordKey, int passwordKdf,
                      const DeviceId &deviceId, QByteArrayView signingKey,
                      QByteArrayView credential);

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
    [[nodiscard]] bool loginIsThrottled(const QByteArray &subject);
    void recordLoginFailure(const QByteArray &subject);

    [[nodiscard]] Result<AuthTokens, RelayError>
    issueFamilyTokens(const AccountId &accountId, const DeviceId &deviceId,
                      std::optional<QByteArray> existingFamily);

    PostgresStore &m_store;
    Policy m_policy;
};

} // namespace OpenChat::Relay
