#pragma once

#include "domain/Contact.h"
#include "domain/Identifiers.h"
#include "network/RelayClient.h"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>

#include <optional>

namespace OpenChat {

class ProfileSession;
class SyncEngine;

// Drives the SEND side of the content-blind MLS add-contact handshake: resolve a
// peer (by handle or by redeeming a one-time invite), record a PendingOutgoing
// roster entry with a fresh conversation and MLS group, claim one of their device
// KeyPackages, add the peer (producing a Welcome), and ship the Welcome to that
// device as a MlsHandshake envelope through the SyncEngine.
//
// The local rows are written BEFORE the claim, so a peer whose one-time
// KeyPackage would be wasted by a local failure is never asked for one. That
// makes the claim the last thing that can fail, and when it does the rows are
// rolled back: a failed add leaves no "request sent" behind for a request that
// never left, and the handle can be asked again at once.
//
// A single-shot QObject state machine modelled on AccountBootstrap. Every relay
// outcome is wired before the first call; each handler is guarded by the current
// state plus a terminal-once flag, so a duplicated or out-of-order relay signal
// cannot skip, repeat, or resurrect a step. Exactly one terminal signal is
// emitted: succeeded() or failed().
//
//   Idle  --start*-->  Resolving  --resolved-->  Claiming  --claimed-->  Terminated
//
// Lifetimes: the session, relay and engine are borrowed by reference and MUST
// outlive this object. The session must be unlocked with live networking
// (startNetworking already succeeded) for the send to leave the device.
class AddContactService final : public QObject
{
    Q_OBJECT

public:
    enum class Error {
        NotFound,    // the handle/invite did not resolve to an account
        SelfContact, // the resolved account is this device's own account
        NoDevice,    // the resolved account advertises no devices
        NoKeyPackage, // no device had a claimable KeyPackage left
        Blocked,     // the peer is locally blocked; nothing was sent
        Mls,         // group creation or member addition failed
        Storage,     // a local persistence failure
        Transport,   // a relay transport-level or auth failure
    };
    Q_ENUM(Error)

    AddContactService(ProfileSession &session, RelayClient &relay, SyncEngine &engine,
                      QObject *parent = nullptr);
    ~AddContactService() override;

    AddContactService(const AddContactService &) = delete;
    AddContactService &operator=(const AddContactService &) = delete;

    // Begins the flow by resolving a directory handle. Ignored unless Idle.
    void startByHandle(const QString &handle);
    // Begins the flow by redeeming a one-time invite token. Ignored unless Idle.
    void startByInvite(const QByteArray &inviteToken);

signals:
    void succeeded(const OpenChat::ConversationId &conversation, const OpenChat::AccountId &peer);
    void failed(OpenChat::AddContactService::Error error);

private:
    enum class State { Idle, Resolving, Claiming, Terminated };

    void connectRelay();

    void onResolved(const RelayDirectoryEntry &entry);
    void onResolutionFailed(RelayDirectoryError error);
    void onKeyPackageClaimed(const QByteArray &keyPackage);
    void onKeyPackageClaimFailed(RelayClaimError error);
    void onAuthExpired();
    void onTransportError(RelayTransportError error);

    // Writes the roster row, conversation and MLS group, once per add.
    bool prepareLocal();
    // Points the roster row at the current device and asks for its KeyPackage.
    void claimCurrentDevice();
    // Undoes prepareLocal() after a failure, restoring any row it replaced.
    void rollbackLocal(Error error);
    void succeed(const ConversationId &conversation, const AccountId &peer);
    void fail(Error error);
    void teardown();

    ProfileSession &m_session;
    RelayClient &m_relay;
    SyncEngine &m_engine;

    QString m_handle; // empty for the invite path
    std::optional<RelayDirectoryEntry> m_entry;
    int m_deviceIndex = 0;
    std::optional<ConversationId> m_conversation;
    // The roster row as it stood before this add replaced it (an earlier
    // outgoing request, say), so a rollback can put it back rather than delete.
    std::optional<ContactRecord> m_priorContact;
    bool m_prepared = false;

    State m_state = State::Idle;
    QList<QMetaObject::Connection> m_connections;
};

} // namespace OpenChat
