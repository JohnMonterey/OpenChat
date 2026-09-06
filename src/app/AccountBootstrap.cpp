#include "app/AccountBootstrap.h"

#include "app/ProfileSession.h"
#include "crypto/MlsClient.h"
#include "network/RelayClient.h"

#include <utility>

namespace OpenChat {

AccountBootstrap::AccountBootstrap(ProfileSession &session, RelayClient &relay,
                                   SyncTransport &transport, QObject *parent)
    : QObject(parent), m_session(session), m_relay(relay), m_transport(transport)
{
}

AccountBootstrap::~AccountBootstrap() { teardown(); }

void AccountBootstrap::start(const QString &handle, int keyPackageCount)
{
    if (m_state != State::Idle)
        return;

    m_handle = handle;
    m_context = QByteArrayLiteral("account-device-bind");
    m_keyPackageCount = keyPackageCount > 0 ? keyPackageCount : defaultKeyPackageCount;
    m_publishedCount = 0;

    // Read the local identity up front. A missing account id or credential means
    // the session is not in a usable unlocked state; fail closed rather than
    // issue a malformed registration.
    const auto account = m_session.accountId();
    const auto credential = m_session.publicCredential();
    if (!account.hasValue() || !credential.hasValue() || m_session.mls() == nullptr) {
        fail(Error::Storage);
        return;
    }
    m_deviceCredential = credential.value().serialize();

    // Wire every relay outcome before issuing the first call so a synchronous
    // failure (e.g. an insecure endpoint) is caught by onTransportError.
    m_connections << connect(&m_relay, &RelayClient::accountRegistered, this,
                             &AccountBootstrap::onAccountRegistered);
    m_connections << connect(&m_relay, &RelayClient::accountRegistrationFailed, this,
                             &AccountBootstrap::onAccountRegistrationFailed);
    m_connections << connect(&m_relay, &RelayClient::authenticated, this,
                             &AccountBootstrap::onAuthenticated);
    m_connections << connect(&m_relay, &RelayClient::authExpired, this,
                             &AccountBootstrap::onAuthExpired);
    m_connections << connect(&m_relay, &RelayClient::keyPackagePublished, this,
                             &AccountBootstrap::onKeyPackagePublished);
    m_connections << connect(&m_relay, &RelayClient::keyPackagePublishFailed, this,
                             &AccountBootstrap::onKeyPackagePublishFailed);
    m_connections << connect(&m_relay, &RelayClient::transportError, this,
                             &AccountBootstrap::onTransportError);

    m_state = State::Registering;
    m_relay.registerAccount(account.value(), credential.value().deviceId, m_handle,
                            credential.value().signingPublicKey, m_deviceCredential);
}

void AccountBootstrap::onAccountRegistered()
{
    if (m_state != State::Registering)
        return;
    m_state = State::Authenticating;

    // The signer keeps the device private key inside the session and returns the
    // raw 64-byte signature over the bound (challenge, context), or an empty
    // array on failure (RelayClient then fails the handshake closed). It is
    // invoked synchronously inside the challenge-reply handler while this object
    // is alive, so capturing `this` is safe.
    ChallengeSigner signer = [this](QByteArrayView challenge, QByteArrayView context) -> QByteArray {
        auto signature = m_session.signChallenge(challenge, context);
        return signature.hasValue() ? signature.value() : QByteArray();
    };
    m_relay.authenticateDevice(m_deviceCredential, signer, m_context);
}

void AccountBootstrap::onAccountRegistrationFailed(RelayRegistrationError error)
{
    if (m_state != State::Registering)
        return;
    fail(error == RelayRegistrationError::HandleUnavailable ? Error::HandleUnavailable
                                                            : Error::Transport);
}

void AccountBootstrap::onAuthenticated(const RelaySession &session)
{
    if (m_state != State::Authenticating)
        return;
    // Install the freshly-issued tokens so the KeyPackage publishes (and the
    // live connection) are authorized with this device's bearer access token.
    m_relay.setTokens(session.accessToken, session.refreshToken);
    m_state = State::Publishing;
    publishNextKeyPackage();
}

void AccountBootstrap::onAuthExpired()
{
    if (m_state == State::Authenticating || m_state == State::Publishing)
        fail(Error::Auth);
}

void AccountBootstrap::publishNextKeyPackage()
{
    if (m_state != State::Publishing)
        return;
    if (m_publishedCount >= m_keyPackageCount) {
        goLiveAndSucceed();
        return;
    }
    // Generate one KeyPackage at a time and publish it; each generation advances
    // the in-memory MLS ratchet (captured, not yet durable). The publishes are
    // serialized so at most one is outstanding, which keeps the state machine's
    // outstanding-count bookkeeping trivial and robust.
    MlsClient *mls = m_session.mls();
    if (mls == nullptr) {
        fail(Error::Storage);
        return;
    }
    auto keyPackage = mls->generateKeyPackage();
    if (!keyPackage.hasValue()) {
        fail(Error::Publish);
        return;
    }
    if (!m_session.persistMlsState().hasValue()) {
        fail(Error::Storage);
        return;
    }
    m_relay.publishKeyPackage(keyPackage.value());
}

void AccountBootstrap::onKeyPackagePublished()
{
    if (m_state != State::Publishing)
        return;
    ++m_publishedCount;
    publishNextKeyPackage();
}

void AccountBootstrap::onKeyPackagePublishFailed()
{
    if (m_state != State::Publishing)
        return;
    fail(Error::Publish);
}

void AccountBootstrap::onTransportError(RelayTransportError error)
{
    (void)error;
    // Registration and the authenticated bootstrap calls surface some failures
    // through transportError (e.g. an insecure endpoint, TLS, or a non-2xx from
    // the auth exchange). Treat those as terminal for the bootstrap. Once we are
    // Connecting the live stream owns its own reconnect policy, so its transport
    // errors must NOT fail an account that is already registered and provisioned.
    if (m_state == State::Registering || m_state == State::Authenticating
        || m_state == State::Publishing)
        fail(Error::Transport);
}

void AccountBootstrap::goLiveAndSucceed()
{
    // Final flush before going live. Each KeyPackage's private material was
    // already committed before its upload, so even a claim during bootstrap
    // can be answered after a restart.
    auto persisted = m_session.persistMlsState();
    if (!persisted.hasValue()) {
        fail(Error::Storage);
        return;
    }

    m_state = State::Connecting;
    auto networking = m_session.startNetworking(m_transport);
    if (!networking.hasValue()) {
        fail(Error::Storage);
        return;
    }
    // Open the live stream from the start of the durable watermark. This is
    // best-effort: reconnect is automatic, and the account is already fully
    // registered and provisioned, so success does not wait on the socket.
    m_relay.connectLive(0);
    succeed();
}

void AccountBootstrap::succeed()
{
    if (m_state == State::Terminated)
        return;
    m_state = State::Terminated;
    teardown();
    emit succeeded();
}

void AccountBootstrap::fail(Error error)
{
    if (m_state == State::Terminated)
        return;
    m_state = State::Terminated;
    teardown();
    emit failed(error);
}

void AccountBootstrap::teardown()
{
    for (const QMetaObject::Connection &connection : m_connections)
        QObject::disconnect(connection);
    m_connections.clear();
}

} // namespace OpenChat
