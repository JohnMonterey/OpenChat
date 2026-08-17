#pragma once

#include "core/Result.h"
#include "domain/Contact.h"
#include "repositories/RepositoryError.h"

#include <QByteArrayView>
#include <QVector>

#include <optional>

namespace OpenChat {

// Durable, per-profile contact roster: the peers the local user is connected to
// or has a pending connection with, driven by an explicit ContactState machine.
// The surface exposes intention-revealing mutations rather than a generic
// upsert, in the same spirit as SyncRepository. Time is injected on every
// mutation so behaviour stays deterministic under test.
class ContactRepository
{
public:
    virtual ~ContactRepository() = default;

    // All contacts in a deterministic order (createdAtMs, then accountId).
    [[nodiscard]] virtual Result<QVector<ContactRecord>, RepositoryError> contacts() = 0;

    // A single contact, or std::nullopt when the peer is unknown.
    [[nodiscard]] virtual Result<std::optional<ContactRecord>, RepositoryError>
    find(const AccountId &accountId) = 0;

    // Record an outbound connection request, moving the peer to PendingOutgoing
    // (createdAtMs/updatedAtMs are taken from the record). A Blocked peer is
    // never overwritten: the row is left Blocked and Conflict is returned.
    [[nodiscard]] virtual Result<void, RepositoryError>
    recordOutgoingRequest(const ContactRecord &contact) = 0;

    // Record an inbound connection request. A new peer becomes PendingIncoming.
    // Security-critical: a Blocked peer is never resurrected -- the row stays
    // Blocked and success is returned without any state change. An already
    // Accepted or PendingOutgoing peer is never regressed by an untrusted
    // incoming request; only a still-PendingIncoming row refreshes its metadata.
    [[nodiscard]] virtual Result<void, RepositoryError>
    recordIncomingRequest(const ContactRecord &contact) = 0;

    // Promote a pending contact to Accepted and record its MLS group. Returns
    // NotFound when the peer is unknown and Conflict when it is Blocked (a
    // Blocked peer must be unblocked before it can be accepted).
    [[nodiscard]] virtual Result<void, RepositoryError>
    markAccepted(const AccountId &accountId, const ConversationId &conversationId,
                 qint64 updatedAtMs) = 0;

    // Block a peer (idempotent). Creates the row if the peer is unknown, so a
    // peer can be pre-emptively blocked before any request is exchanged.
    [[nodiscard]] virtual Result<void, RepositoryError>
    block(const AccountId &accountId, qint64 updatedAtMs) = 0;

    // Explicitly revoke a block, forgetting the peer (the roster has no neutral
    // state, so unblocking returns the peer to a stranger). Returns NotFound when
    // the peer is unknown and Conflict when the peer exists but is not Blocked.
    [[nodiscard]] virtual Result<void, RepositoryError> unblock(const AccountId &accountId) = 0;

    // Delete any contact from the roster (idempotent).
    [[nodiscard]] virtual Result<void, RepositoryError> remove(const AccountId &accountId) = 0;

    // Record the local user's out-of-band safety-number verification assertion for
    // a contact. Returns NotFound when the peer is unknown.
    [[nodiscard]] virtual Result<void, RepositoryError>
    setVerified(const AccountId &accountId, bool verified, qint64 updatedAtMs) = 0;

    // Backfill a contact's authenticated peer signing key. Idempotent set-if-NULL:
    // an already-known key is never overwritten (no-op success). Returns NotFound
    // when the peer is unknown and InvalidInput when the key is not 32 bytes.
    [[nodiscard]] virtual Result<void, RepositoryError>
    setPeerSigningKey(const AccountId &accountId, QByteArrayView key32) = 0;
};

} // namespace OpenChat
