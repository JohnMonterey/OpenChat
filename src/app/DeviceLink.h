#pragma once

#include "app/KeyPackageSupply.h"

#include <QByteArray>
#include <QList>
#include <QObject>

namespace OpenChat {

class ProfileSession;
class RelayClient;
struct RelaySession;
enum class RelayTransportError;

// Keeps an unlocked profile's device authenticated and live-connected to the
// relay for the life of the session. AccountBootstrap performs the first-run
// registration and initial authentication; every LATER launch unlocks an
// existing profile with no relay tokens at all, and long-running sessions
// eventually exhaust their refresh token. This object also owns background key
// package replenishment for both newly registered and existing profiles:
//
//   start(NeedsAuthentication)  -> challenge/response -> tokens -> connectLive
//   start(AlreadyLive)          -> maintain key package supply until authExpired
//   authExpired (any time)      -> re-authenticate with backoff, reconnect live
//   transportError while authenticating -> retry with backoff
//
// The socket's own reconnect policy handles ordinary link drops; this only
// steps in when the relay no longer accepts the device's bearer tokens or when
// the device has none. It never sees a private key: signing goes through the
// session's signChallenge, exactly like AccountBootstrap.
//
// Lifetimes: the session and relay are borrowed and MUST outlive this object.
class DeviceLink final : public QObject
{
    Q_OBJECT

public:
    enum class Start { NeedsAuthentication, AlreadyLive };

    DeviceLink(ProfileSession &session, RelayClient &relay, QObject *parent = nullptr);
    ~DeviceLink() override;

    DeviceLink(const DeviceLink &) = delete;
    DeviceLink &operator=(const DeviceLink &) = delete;

    void start(Start mode);

    [[nodiscard]] bool isAuthenticated() const noexcept { return m_authenticated; }

signals:
    // The device holds valid tokens and the live stream has been requested.
    void linked();
    // An authentication attempt failed; a retry is scheduled.
    void authenticationFailed();

private:
    void authenticate();
    void scheduleRetry();
    void onAuthenticated(const RelaySession &session);
    void onAuthExpired();
    void onTransportError(RelayTransportError error);

    ProfileSession &m_session;
    RelayClient &m_relay;
    KeyPackageSupply m_supply;
    QByteArray m_deviceCredential;
    bool m_authenticating = false;
    bool m_authenticated = false;
    int m_retryDelayMs = 2'000;
    QList<QMetaObject::Connection> m_connections;
};

} // namespace OpenChat
