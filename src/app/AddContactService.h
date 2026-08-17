#pragma once

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
// peer (by handle or by redeeming a one-time invite), claim one of their device
// KeyPackages, create a fresh MLS group, add the peer (producing a Welcome),
// record a PendingOutgoing roster entry, and ship the Welcome to that device as a
// MlsHandshake envelope through the SyncEngine.
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

    void claimCurrentDevice();
    void succeed(const ConversationId &conversation, const AccountId &peer);
    void fail(Error error);
    void teardown();

    ProfileSession &m_session;
    RelayClient &m_relay;
    SyncEngine &m_engine;

    QString m_handle; // empty for the invite path
    std::optional<RelayDirectoryEntry> m_entry;
    int m_deviceIndex = 0;

    State m_state = State::Idle;
    QList<QMetaObject::Connection> m_connections;
};

} // namespace OpenChat
