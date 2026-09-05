#pragma once

#include "PostgresStore.h"
#include "RelayTypes.h"
#include "core/Result.h"
#include "domain/Identifiers.h"

#include <QByteArray>
#include <QString>

namespace OpenChat::Relay {

// Relay-side discovery: resolve a @handle to an account and its active devices,
// and mint/redeem one-time invite links. The relay stays content-blind — a
// handle resolves only by exact match (never a prefix, listing, or
// enumeration), invite tokens are stored solely as SHA-256 hashes, and
// redemption is single-use and expiry-guarded. Every redeem failure (unknown,
// expired, or already-consumed) collapses to NotFound so a caller cannot use
// the response to tell those cases apart.
class DirectoryService final
{
public:
    struct Policy final {
        qint64 inviteTtlMs = 7LL * 24 * 60 * 60'000;         // 7 days
        qint64 maxInviteTtlMs = 30LL * 24 * 60 * 60'000;     // hard cap on a requested TTL
    };

    explicit DirectoryService(PostgresStore &store);
    DirectoryService(PostgresStore &store, Policy policy);

    // Exact-match handle lookup. Returns the account and its active (non-revoked)
    // devices, or NotFound when the handle is unknown or has no active device.
    // No prefix match, listing, or enumeration is offered.
    [[nodiscard]] Result<AccountDirectoryEntry, RelayError> resolveHandle(const QString &handle);

    // Reverse lookup: the handle registered for an account id, or NotFound when
    // the account is unknown or has no active device (a fully revoked account
    // is not discoverable in either direction). Account ids are 128-bit random
    // values, so this offers no enumeration surface beyond the forward lookup.
    [[nodiscard]] Result<QString, RelayError> resolveAccount(const AccountId &account);

    // Mints a one-time invite for the account with the given TTL and returns the
    // PLAINTEXT token — the only moment it exists; only its hash is stored.
    [[nodiscard]] Result<QByteArray, RelayError> createInvite(const AccountId &account,
                                                              qint64 ttlMs);
    // Convenience overload using the policy default TTL.
    [[nodiscard]] Result<QByteArray, RelayError> createInvite(const AccountId &account);

    // Redeems (consumes) a one-time invite and returns the inviter's account and
    // active devices. Single-use and expiry-guarded; any failure maps to NotFound
    // to avoid an oracle.
    [[nodiscard]] Result<AccountDirectoryEntry, RelayError> redeemInvite(const QByteArray &token);

    // Default TTL applied when a creator does not request one (used by the HTTP
    // layer to report the resulting expiry).
    [[nodiscard]] qint64 defaultInviteTtlMs() const { return m_policy.inviteTtlMs; }

private:
    // Loads the account's active devices, or NotFound if it has none.
    [[nodiscard]] Result<AccountDirectoryEntry, RelayError>
    loadActiveDevices(const AccountId &account);

    PostgresStore &m_store;
    Policy m_policy;
};

} // namespace OpenChat::Relay
