#include "app/DeviceLink.h"

#include "app/ProfileSession.h"
#include "network/RelayClient.h"

#include <QTimer>

#include <algorithm>

namespace OpenChat {

namespace {

constexpr int initialRetryMs = 2'000;
constexpr int maximumRetryMs = 60'000;
constexpr char deviceBindContext[] = "account-device-bind";

} // namespace

DeviceLink::DeviceLink(ProfileSession &session, RelayClient &relay, QObject *parent)
    : QObject(parent), m_session(session), m_relay(relay)
{
    m_connections << connect(&m_relay, &RelayClient::authenticated, this,
                             &DeviceLink::onAuthenticated);
    m_connections << connect(&m_relay, &RelayClient::authExpired, this,
                             &DeviceLink::onAuthExpired);
    m_connections << connect(&m_relay, &RelayClient::transportError, this,
                             &DeviceLink::onTransportError);
}

DeviceLink::~DeviceLink()
{
    for (const QMetaObject::Connection &connection : m_connections)
        QObject::disconnect(connection);
    m_connections.clear();
}

void DeviceLink::start(Start mode)
{
    if (mode == Start::AlreadyLive) {
        // Bootstrap already authenticated and opened the stream; only a later
        // authExpired brings this object into play.
        m_authenticated = true;
        return;
    }
    authenticate();
}

void DeviceLink::authenticate()
{
    if (m_authenticating)
        return;
    const auto credential = m_session.publicCredential();
    if (!credential.hasValue()) {
        emit authenticationFailed();
        scheduleRetry();
        return;
    }
    m_deviceCredential = credential.value().serialize();
    m_authenticating = true;
    m_authenticated = false;
    // The signer keeps the private key inside the session (see AccountBootstrap);
    // it runs synchronously inside the challenge-reply handler while this object
    // is alive.
    ChallengeSigner signer = [this](QByteArrayView challenge, QByteArrayView context) -> QByteArray {
        auto signature = m_session.signChallenge(challenge, context);
        return signature.hasValue() ? signature.value() : QByteArray();
    };
    m_relay.authenticateDevice(m_deviceCredential, signer, QByteArray(deviceBindContext));
}

void DeviceLink::scheduleRetry()
{
    const int delay = m_retryDelayMs;
    m_retryDelayMs = std::min(maximumRetryMs, m_retryDelayMs * 2);
    QTimer::singleShot(delay, this, [this] {
        if (!m_authenticated)
            authenticate();
    });
}

void DeviceLink::onAuthenticated(const RelaySession &session)
{
    if (!m_authenticating)
        return; // bootstrap's own authentication; it installs the tokens itself
    m_authenticating = false;
    m_authenticated = true;
    m_retryDelayMs = initialRetryMs;
    m_relay.setTokens(session.accessToken, session.refreshToken);
    // Resume from zero: the relay only redelivers unacknowledged envelopes and
    // the durable store's replay guard drops anything already applied.
    m_relay.connectLive(0);
    emit linked();
}

void DeviceLink::onAuthExpired()
{
    // Either the challenge/response itself was refused, or a later refresh was
    // exhausted. Both mean the device must run the full authentication again.
    m_authenticating = false;
    m_authenticated = false;
    emit authenticationFailed();
    scheduleRetry();
}

void DeviceLink::onTransportError(RelayTransportError error)
{
    (void)error;
    if (!m_authenticating)
        return; // the live socket's reconnect policy owns its own errors
    m_authenticating = false;
    emit authenticationFailed();
    scheduleRetry();
}

} // namespace OpenChat
