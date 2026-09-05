#pragma once

#include "domain/Identifiers.h"

#include <QByteArray>
#include <QString>

#include <optional>

namespace OpenChat {

// The lifecycle of a peer in the local contact roster. Distinct from the UI
// presentation struct in src/models/Contact.h: this is the durable, storage-
// facing identity of a connection, keyed by the peer's directory AccountId.
enum class ContactState {
    // We initiated a connection request to this peer (a Welcome we will send in
    // a later phase); awaiting their acceptance.
    PendingOutgoing,
    // This peer sent us a connection request (an unknown Welcome we surfaced);
    // awaiting our decision.
    PendingIncoming,
    // A mutual, active contact backed by an MLS group.
    Accepted,
    // We blocked this peer. Blocked is sticky: no incoming request may move it to
    // another state. Only an explicit local unblock (or removal) leaves Blocked.
    Blocked,
};

struct ContactRecord final {
    AccountId accountId;
    QString handle;
    QString displayName;
    ContactState state = ContactState::PendingOutgoing;
    std::optional<ConversationId> conversationId;
    qint64 createdAtMs = 0;
    qint64 updatedAtMs = 0;
    std::optional<QByteArray> peerSigningKey{};  // peer's 32-byte Ed25519 identity key, when known
    bool verified = false;                        // local user assertion after comparing the safety number
    // The peer device the 2-party MLS group was formed with: the device whose
    // KeyPackage we claimed (sender side) or the device that shipped the Welcome
    // (receiver side). It is the recipient of every envelope in the
    // conversation; NULL for rows that predate migration 011 or were blocked
    // before any exchange.
    std::optional<DeviceId> peerDeviceId{};
};

} // namespace OpenChat
