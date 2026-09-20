#pragma once

#include "domain/Identifiers.h"

#include <QByteArray>
#include <QObject>
#include <QString>

namespace OpenChat {

class ProfileSession;
class RelayClient;
class SyncTransport;
struct RelaySession;
enum class RelayRegistrationError;
enum class RelayLoginError;
enum class RelayTransportError;

// Turns a freshly-created, unlocked ProfileSession into a registered,
// KeyPackage-publishing, live account against the relay. A single-shot QObject
// state machine that drives RelayClient's asynchronous handshake. It has two
// entry points that differ only in their first step: start() creates a new
// account under a username and password, startLogin() signs in to an existing
// one and enrolls this installation as its device.
//
//   Registering  --accountRegistered-->  Authenticating      (start)
//   LoggingIn    --accountLoggedIn-->    Authenticating      (startLogin; the
//                                        session adopts the relay's account id)
//   Authenticating  --authenticated-->   Publishing (tokens installed)
//   Publishing  --N x keyPackagePublished-->  Connecting (MLS state persisted)
//   Connecting  -->  succeeded()
//
// Every step advances only on the success signal of the prior step, and each
// handler is guarded by the current state plus a terminal-once flag, so a
// duplicated or out-of-order relay signal cannot skip, repeat, or resurrect a
// step. Exactly one terminal signal is emitted: succeeded() or failed().
//
// Lifetimes: the session, relay and transport are borrowed by reference and
// MUST outlive this object. The transport is a RelayTransport over the same
// relay; because startNetworking() hands it to the session's SyncEngine, it must
// additionally outlive the session's networking (i.e. until the session is
// locked). The recovery code is orthogonal to bootstrap and is taken by the
// caller via ProfileSession::takeRecoveryCode() (typically right after create(),
// independent of when this runs); it is deliberately NOT surfaced here.
class AccountBootstrap final : public QObject
{
    Q_OBJECT

public:
    enum class Error {
        HandleUnavailable, // the requested handle is already taken (relay 409)
        InvalidCredentials, // login: wrong username or password (relay 401)
        RateLimited,       // login: too many failed attempts for that username
        Auth,              // device authentication failed or expired
        Publish,           // KeyPackage generation or publish failed
        Transport,         // registration or a transport-level failure
        Storage,           // a local session/persistence failure
    };
    Q_ENUM(Error)

    // A sensible default one-time KeyPackage pool size for a new device.
    static constexpr int defaultKeyPackageCount = 16;

    AccountBootstrap(ProfileSession &session, RelayClient &relay, SyncTransport &transport,
                     QObject *parent = nullptr);
    ~AccountBootstrap() override;

    AccountBootstrap(const AccountBootstrap &) = delete;
    AccountBootstrap &operator=(const AccountBootstrap &) = delete;

    // Begins the sign-up flow. Registers the account under `handle`, protected
    // by `passwordKey` (the locally stretched key from security/PasswordKey.h,
    // never the password), and provisions `keyPackageCount` one-time KeyPackages.
    // The key is forwarded to the relay call and not retained. Ignored if
    // already started.
    void start(const QString &handle, const QByteArray &passwordKey,
               int keyPackageCount = defaultKeyPackageCount);

    // Begins the login flow for an existing account. On success the session has
    // durably adopted the account id the username resolved to before any token
    // is requested. Ignored if already started.
    void startLogin(const QString &handle, const QByteArray &passwordKey,
                    int keyPackageCount = defaultKeyPackageCount);

    // The canonical handle of the account: as given to start(), or as confirmed
    // by the relay once a login succeeded.
    [[nodiscard]] QString handle() const;

signals:
    void succeeded();
    void failed(OpenChat::AccountBootstrap::Error error);

private:
    enum class State {
        Idle,
        Registering,
        LoggingIn,
        Authenticating,
        Publishing,
        Connecting,
        Terminated
    };

    // Reads the local identity and wires every relay outcome; false after fail().
    [[nodiscard]] bool prepare(const QString &handle, int keyPackageCount);
    void authenticate();
    void onAccountRegistered();
    void onAccountRegistrationFailed(RelayRegistrationError error);
    void onAccountLoggedIn(const AccountId &account, const QString &handle);
    void onAccountLoginFailed(RelayLoginError error);
    void onAuthenticated(const RelaySession &session);
    void onAuthExpired();
    void onKeyPackagePublished();
    void onKeyPackagePublishFailed();
    void onTransportError(RelayTransportError error);

    void publishNextKeyPackage();
    void goLiveAndSucceed();
    void succeed();
    void fail(Error error);
    void teardown();

    ProfileSession &m_session;
    RelayClient &m_relay;
    SyncTransport &m_transport;

    QString m_handle;
    QByteArray m_deviceCredential; // MLS credential blob, signed at registration
    QByteArray m_context;          // fixed non-empty device-bind context

    int m_keyPackageCount = defaultKeyPackageCount;
    int m_publishedCount = 0;

    State m_state = State::Idle;
    QList<QMetaObject::Connection> m_connections;
};

} // namespace OpenChat
