#include "network/RelayClient.h"

#include <QAbstractSocket>
#include <QCborArray>
#include <QCborMap>
#include <QCborStreamReader>
#include <QCborValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSslError>
#include <QSslSocket>
#include <QTimer>
#include <QUrlQuery>
#include <QWebSocket>
#include <QWebSocketHandshakeOptions>
#include <QWebSocketProtocol>

#include <algorithm>
#include <optional>
#include <utility>

namespace OpenChat {

namespace {

// Control-frame discriminators carried as the first element of a top-level CBOR
// array. Delivery frames are top-level CBOR maps (canonical envelopes) and are
// never handled here, so the two categories can never be confused.
enum class ControlType : int {
    RelayAccepted = 1, // server -> client: [1, bstr envelopeId(16), uint sequence]
    AuthExpired = 2,   // server -> client: [2]
    Acknowledge = 3,   // client -> server: [3, bstr envelopeId(16), uint watermark]
    Delivery = 4,      // server -> client: [4, uint serverSequence, bstr canonicalEnvelope]
};

[[nodiscard]] bool isSecureScheme(const QUrl &url, QLatin1StringView scheme)
{
    return url.isValid() && !url.host().isEmpty() && url.scheme() == scheme;
}

[[nodiscard]] bool isHttps(const QUrl &url)
{
    return isSecureScheme(url, QLatin1StringView("https"));
}

[[nodiscard]] bool isWss(const QUrl &url)
{
    return isSecureScheme(url, QLatin1StringView("wss"));
}

// CBOR major type lives in the top three bits of the first byte. Major type 5
// is a map (a candidate canonical envelope); major type 4 is an array (a
// control frame). Everything else is rejected outright.
[[nodiscard]] int cborMajorType(const QByteArray &frame)
{
    if (frame.isEmpty())
        return -1;
    return (static_cast<unsigned char>(frame.at(0)) >> 5) & 0x07;
}

// Parses a bounded control frame. The frame is already size-capped by the
// WebSocket incoming limits, so a whole-value parse is bounded. Rejects
// trailing data and non-array roots.
[[nodiscard]] std::optional<QCborArray> parseControlFrame(const QByteArray &frame)
{
    QCborParserError error{};
    const QCborValue value = QCborValue::fromCbor(frame, &error);
    if (error.error != QCborError::NoError)
        return std::nullopt;
    if (error.offset != frame.size())
        return std::nullopt; // trailing bytes
    if (!value.isArray())
        return std::nullopt;
    return value.toArray();
}

[[nodiscard]] QByteArray fixedBytes(const QCborValue &value, qsizetype expected)
{
    if (!value.isByteArray())
        return {};
    const QByteArray bytes = value.toByteArray();
    return bytes.size() == expected ? bytes : QByteArray{};
}

// Defensive bounds for a directory response from the semi-trusted relay: a
// resolved account exposes at most this many devices, each device's signing key
// is exactly an Ed25519 public key, and an invite token is bounded well below
// the whole-body ceiling so a "token" field can never be an oversize blob.
constexpr qsizetype maxDirectoryDevices = 64;
constexpr qsizetype directorySigningKeyBytes = 32;
constexpr qsizetype maxInviteTokenBytes = 4096;

} // namespace

bool RelayEndpoints::isSecure() const
{
    return isHttps(authChallenge) && isHttps(authComplete) && isHttps(authRefresh)
        && isHttps(sync) && isWss(live);
}

// ---------------------------------------------------------------------------

class RelayClient::Private
{
public:
    Private(DeviceId localDeviceId, AccountId localAccountId, RelayEndpoints endpoints,
            RelayCredentials credentials, RelayLimits limits, BackoffPolicy backoff,
            RelayClient *owner)
        : q(owner)
        , localDeviceId(std::move(localDeviceId))
        , localAccountId(std::move(localAccountId))
        , endpoints(std::move(endpoints))
        , credentials(std::move(credentials))
        , limits(limits)
        , backoff(std::move(backoff))
    {
        network = new QNetworkAccessManager(q);
        network->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
        network->setStrictTransportSecurityEnabled(true);

        reconnectTimer = new QTimer(q);
        reconnectTimer->setSingleShot(true);
        QObject::connect(reconnectTimer, &QTimer::timeout, q, [this] { openSocket(); });

        connectDeadline = new QTimer(q);
        connectDeadline->setSingleShot(true);
        QObject::connect(connectDeadline, &QTimer::timeout, q, [this] { onConnectTimeout(); });
    }

    [[nodiscard]] QSslConfiguration hardenedTls() const
    {
        QSslConfiguration config = tlsConfigured ? tlsConfig
                                                 : QSslConfiguration::defaultConfiguration();
        // Re-assert verification unconditionally so an injected test config can
        // never weaken it.
        config.setPeerVerifyMode(QSslSocket::VerifyPeer);
        config.setProtocol(QSsl::SecureProtocols);
        return config;
    }

    // Builds a hardened HTTPS request with no Authorization header. Used for the
    // unauthenticated bootstrap endpoints (account registration) and as the base
    // for authorizedRequest(); the same TLS, redirect, cache, and timeout posture
    // applies to every request the client issues.
    [[nodiscard]] QNetworkRequest hardenedRequest(const QUrl &url) const
    {
        QNetworkRequest request(url);
        request.setSslConfiguration(hardenedTls());
        request.setMaximumRedirectsAllowed(limits.maxRedirects);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::NoLessSafeRedirectPolicy);
        request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                             QNetworkRequest::AlwaysNetwork);
        request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
        request.setTransferTimeout(limits.transferTimeout);
        request.setRawHeader(QByteArrayLiteral("Accept"),
                             QByteArrayLiteral("application/cbor"));
        return request;
    }

    // Builds a hardened authorized HTTPS request. The access token is fetched
    // fresh from the callback and only lives inside this request; RelayClient
    // keeps no member copy.
    [[nodiscard]] QNetworkRequest authorizedRequest(const QUrl &url) const
    {
        QNetworkRequest request = hardenedRequest(url);
        if (credentials.accessToken) {
            const QByteArray token = credentials.accessToken();
            if (!token.isEmpty())
                request.setRawHeader(QByteArrayLiteral("Authorization"),
                                     QByteArrayLiteral("Bearer ") + token);
        }
        return request;
    }

    // Wires the standard size-bounding guards onto a reply. The read buffer is
    // kept at least as large as the body ceiling (plus slack) so the running
    // total, not socket backpressure, is the effective cap and a legitimate
    // maximum-size catch-up batch is not throttled into a timeout.
    void guardReply(QNetworkReply *reply)
    {
        const qint64 buffer = std::max<qint64>(limits.readBufferBytes,
                                               limits.maxHttpBodyBytes + 65536);
        reply->setReadBufferSize(buffer);
        // Reject a declared oversize body before downloading it.
        QObject::connect(reply, &QNetworkReply::metaDataChanged, q, [this, reply] {
            const QVariant declared = reply->header(QNetworkRequest::ContentLengthHeader);
            if (declared.isValid() && declared.toLongLong() > limits.maxHttpBodyBytes) {
                reply->setProperty("oc_oversized", true);
                reply->abort();
            }
        });
        // Backstop: abort if the running total exceeds the bound regardless of
        // (or despite a lying) Content-Length.
        QObject::connect(reply, &QNetworkReply::downloadProgress, q,
                         [this, reply](qint64 received, qint64 /*total*/) {
                             if (received > limits.maxHttpBodyBytes) {
                                 reply->setProperty("oc_oversized", true);
                                 reply->abort();
                             }
                         });
    }

    // TLS verification failures on the HTTPS paths arrive as a reply error, not
    // an sslErrors callback; surface them with the terminal Tls diagnostic
    // rather than a generic HTTP status.
    [[nodiscard]] static RelayTransportError httpFailureKind(QNetworkReply *reply)
    {
        if (reply->error() == QNetworkReply::SslHandshakeFailedError)
            return RelayTransportError::Tls;
        return RelayTransportError::HttpStatus;
    }

    [[nodiscard]] bool bodyWithinBounds(QNetworkReply *reply) const
    {
        const QVariant declared = reply->header(QNetworkRequest::ContentLengthHeader);
        if (declared.isValid() && declared.toLongLong() > limits.maxHttpBodyBytes)
            return false;
        return true;
    }

    void openSocket()
    {
        if (userClosed)
            return;
        if (!isWss(endpoints.live)) {
            emit q->transportError(RelayTransportError::InsecureEndpoint);
            return;
        }

        teardownSocket();

        socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, q);
        socket->setSslConfiguration(hardenedTls());
        socket->setMaxAllowedIncomingFrameSize(limits.maxIncomingFrameBytes);
        socket->setMaxAllowedIncomingMessageSize(limits.maxIncomingMessageBytes);

        QObject::connect(socket, &QWebSocket::connected, q, [this] { onSocketConnected(); });
        QObject::connect(socket, &QWebSocket::disconnected, q, [this] { onSocketDisconnected(); });
        QObject::connect(socket, &QWebSocket::binaryMessageReceived, q,
                         [this](const QByteArray &message) { onBinaryMessage(message); });
        QObject::connect(socket, &QWebSocket::textMessageReceived, q,
                         [this](const QString &) { onTextMessage(); });
        QObject::connect(socket, &QWebSocket::sslErrors, q,
                         [this](const QList<QSslError> &errors) { onSslErrors(errors); });
        QObject::connect(socket, &QWebSocket::errorOccurred, q,
                         [this](QAbstractSocket::SocketError error) { onSocketError(error); });

        QUrl url = endpoints.live;
        QUrlQuery query(url);
        query.removeQueryItem(QStringLiteral("since"));
        query.addQueryItem(QStringLiteral("since"), QString::number(resumeWatermark));
        url.setQuery(query);

        QNetworkRequest request(url);
        request.setSslConfiguration(hardenedTls());
        if (credentials.accessToken) {
            const QByteArray token = credentials.accessToken();
            if (!token.isEmpty())
                request.setRawHeader(QByteArrayLiteral("Authorization"),
                                     QByteArrayLiteral("Bearer ") + token);
        }

        QWebSocketHandshakeOptions options;
        options.setSubprotocols({QString::fromLatin1(relaySubprotocol)});

        subprotocolVerified = false;
        tlsErrorReported = false;
        connectDeadline->start(limits.connectTimeout);
        socket->open(request, options);
    }

    void onSocketConnected()
    {
        connectDeadline->stop();

        // Enforce exactly the negotiated subprotocol before any data flows.
        if (socket->subprotocol() != QLatin1StringView(relaySubprotocol)) {
            subprotocolVerified = false;
            emit q->transportError(RelayTransportError::ProtocolMismatch);
            // A peer that speaks the wrong subprotocol is misconfigured, not
            // transiently unavailable: close and do not loop reconnecting.
            suppressReconnect = true;
            socket->close(QWebSocketProtocol::CloseCodeProtocolError,
                          QStringLiteral("unexpected subprotocol"));
            return;
        }

        subprotocolVerified = true;
        reconnectAttempt = 0;
        refreshAttemptedThisCycle = false;
        emit q->connected();
    }

    void onSocketDisconnected()
    {
        connectDeadline->stop();
        // A frame/message that overruns the incoming bound makes QWebSocket
        // close with CloseCodeTooMuchData, often without a separate error
        // signal, so surface it here.
        if (socket && socket->closeCode() == QWebSocketProtocol::CloseCodeTooMuchData)
            emit q->transportError(RelayTransportError::FrameTooLarge);
        const bool wasConnected = subprotocolVerified;
        subprotocolVerified = false;
        if (wasConnected)
            emit q->disconnected();
        maybeScheduleReconnect();
    }

    void onConnectTimeout()
    {
        if (!socket)
            return;
        emit q->transportError(RelayTransportError::ConnectTimeout);
        socket->abort();
        maybeScheduleReconnect();
    }

    void reportTls()
    {
        // Emit at most one TLS error per connection attempt: a verification
        // failure surfaces through both sslErrors and errorOccurred.
        if (tlsErrorReported)
            return;
        tlsErrorReported = true;
        emit q->transportError(RelayTransportError::Tls);
    }

    void onSslErrors(const QList<QSslError> &)
    {
        // Terminal by design: never call ignoreSslErrors(). Report and let the
        // socket tear itself down.
        reportTls();
        if (socket)
            socket->abort();
    }

    void onSocketError(QAbstractSocket::SocketError error)
    {
        if (error == QAbstractSocket::SslHandshakeFailedError) {
            reportTls();
            return; // disconnected() will drive reconnect
        }
        // A peer that overruns the incoming frame/message bound is closed by
        // QWebSocket with CloseCodeTooMuchData; the disconnect handler reports
        // it as FrameTooLarge, so defer here to avoid a duplicate error.
        if (socket && socket->closeCode() == QWebSocketProtocol::CloseCodeTooMuchData)
            return;
        if (error == QAbstractSocket::RemoteHostClosedError && subprotocolVerified)
            return; // ordinary close handled by disconnected()
        emit q->transportError(RelayTransportError::Network);
    }

    void onTextMessage()
    {
        // Never process a frame from a connection whose subprotocol has not been
        // verified (e.g. a mismatched peer that pipelined frames before close).
        if (!subprotocolVerified)
            return;
        // Binary-only protocol: a text frame is hostile input.
        emit q->transportError(RelayTransportError::TextFrameRejected);
        if (socket)
            socket->close(QWebSocketProtocol::CloseCodeDatatypeNotSupported,
                          QStringLiteral("text frames are not accepted"));
    }

    void onBinaryMessage(const QByteArray &message)
    {
        // Reject frames on any connection whose subprotocol was not verified.
        if (!subprotocolVerified)
            return;
        if (static_cast<quint64>(message.size()) > limits.maxIncomingMessageBytes) {
            emit q->transportError(RelayTransportError::FrameTooLarge);
            return;
        }

        // All server -> client frames are top-level CBOR arrays (control frames):
        // envelope deliveries carry their server sequence, and acks/notices are
        // small typed arrays. A bare envelope map is never expected inbound.
        if (cborMajorType(message) != 4) {
            emit q->transportError(RelayTransportError::InvalidControlFrame);
            return;
        }
        handleControlFrame(message);
    }

    // Decodes an envelope payload through the bounded canonical decoder only,
    // checks the recipient, and surfaces it with its relay sequence.
    void deliverEnvelope(const QByteArray &envelopeBytes, quint64 serverSequence)
    {
        DecodeLimits decodeLimits;
        decodeLimits.envelopeBytes = static_cast<qsizetype>(limits.maxIncomingMessageBytes);
        const auto decoded = decodeEnvelope(envelopeBytes, decodeLimits);
        if (!decoded.hasValue()) {
            emit q->transportError(RelayTransportError::MalformedEnvelope);
            return;
        }
        if (decoded.value().recipientDeviceId != localDeviceId) {
            emit q->transportError(RelayTransportError::WrongRecipient);
            return;
        }
        emit q->envelopeReceived(decoded.value(), serverSequence);
    }

    void handleControlFrame(const QByteArray &message)
    {
        const auto array = parseControlFrame(message);
        if (!array || array->isEmpty() || !array->at(0).isInteger()) {
            emit q->transportError(RelayTransportError::InvalidControlFrame);
            return;
        }

        switch (static_cast<ControlType>(array->at(0).toInteger())) {
        case ControlType::RelayAccepted: {
            if (array->size() != 3 || !array->at(2).isInteger()) {
                emit q->transportError(RelayTransportError::InvalidControlFrame);
                return;
            }
            const QByteArray idBytes = fixedBytes(array->at(1), EnvelopeId::byteCount);
            const auto id = EnvelopeId::fromBytes(idBytes);
            const qint64 sequence = array->at(2).toInteger();
            if (!id || sequence < 0) {
                emit q->transportError(RelayTransportError::InvalidControlFrame);
                return;
            }
            emit q->relayAccepted(*id, static_cast<quint64>(sequence));
            return;
        }
        case ControlType::Delivery: {
            if (array->size() != 3 || !array->at(1).isInteger() || !array->at(2).isByteArray()) {
                emit q->transportError(RelayTransportError::InvalidControlFrame);
                return;
            }
            const qint64 sequence = array->at(1).toInteger();
            if (sequence < 0) {
                emit q->transportError(RelayTransportError::InvalidControlFrame);
                return;
            }
            deliverEnvelope(array->at(2).toByteArray(), static_cast<quint64>(sequence));
            return;
        }
        case ControlType::AuthExpired:
            attemptRefresh();
            return;
        case ControlType::Acknowledge:
        default:
            // Acknowledge is a client->server frame; receiving it is invalid.
            emit q->transportError(RelayTransportError::InvalidControlFrame);
            return;
        }
    }

    void maybeScheduleReconnect()
    {
        if (userClosed || suppressReconnect)
            return;
        if (reconnectTimer->isActive())
            return; // a reconnect is already pending; do not double-count attempts
        const auto delay = backoff.delayForAttempt(reconnectAttempt);
        // The ceiling saturates at the cap well before this; clamping keeps the
        // counter from growing without bound over a very long-lived session.
        if (reconnectAttempt < 1024)
            ++reconnectAttempt;
        reconnectTimer->start(delay);
    }

    // Performs at most one serialized refresh per auth-failure cycle.
    void attemptRefresh()
    {
        if (refreshAttemptedThisCycle || refreshInFlight) {
            emit q->authExpired();
            return;
        }
        if (!credentials.refreshToken || !isHttps(endpoints.authRefresh)) {
            emit q->authExpired();
            return;
        }

        const QByteArray refresh = credentials.refreshToken();
        if (refresh.isEmpty()) {
            emit q->authExpired();
            return;
        }

        refreshAttemptedThisCycle = true;
        refreshInFlight = true;

        QCborMap body;
        body.insert(QLatin1StringView("refresh"), refresh);

        QNetworkRequest request = authorizedRequest(endpoints.authRefresh);
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                          QByteArrayLiteral("application/cbor"));
        QNetworkReply *reply = network->post(request, body.toCborValue().toCbor());
        guardReply(reply);
        QObject::connect(reply, &QNetworkReply::finished, q, [this, reply] {
            refreshInFlight = false;
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError || !bodyWithinBounds(reply)) {
                emit q->authExpired();
                return;
            }
            const auto session = parseSession(reply->readAll());
            if (!session) {
                emit q->authExpired();
                return;
            }
            // A successful refresh ends this auth-failure cycle: a genuinely
            // new expiry later may refresh again. (The HTTPS fetch retry path
            // deliberately does NOT reset here — it keeps the flag set across
            // its single retry so a repeated 401 cannot loop.)
            refreshAttemptedThisCycle = false;
            emit q->tokensRotated(*session);
            // The caller updates its token storage; a subsequent operation will
            // use the rotated token through the credential callbacks.
        });
    }

    [[nodiscard]] std::optional<RelaySession> parseSession(const QByteArray &body) const
    {
        if (body.size() > limits.maxHttpBodyBytes)
            return std::nullopt;
        QCborParserError error{};
        const QCborValue value = QCborValue::fromCbor(body, &error);
        if (error.error != QCborError::NoError || error.offset != body.size() || !value.isMap())
            return std::nullopt;
        const QCborMap map = value.toMap();
        const QCborValue access = map.value(QLatin1StringView("access"));
        const QCborValue refresh = map.value(QLatin1StringView("refresh"));
        const QCborValue expires = map.value(QLatin1StringView("expiresAtMs"));
        if (!access.isByteArray() || !refresh.isByteArray() || !expires.isInteger())
            return std::nullopt;
        RelaySession session;
        session.accessToken = access.toByteArray();
        session.refreshToken = refresh.toByteArray();
        session.accessExpiresAtMs = expires.toInteger();
        if (session.accessToken.isEmpty() || session.refreshToken.isEmpty())
            return std::nullopt;
        return session;
    }

    // Defensively parses the shared directory shape
    //   { account_id (16 bytes), devices: [ { device_id (16 bytes),
    //     signing_key (32 bytes) }, ... ] }
    // from a semi-trusted relay. Every field size is checked and the device
    // array is capped before any element is trusted; a single violation rejects
    // the whole response so no partial or oversized entry is ever surfaced. The
    // caller applies the raw-body size gate first.
    [[nodiscard]] std::optional<RelayDirectoryEntry> parseDirectoryEntry(const QByteArray &body) const
    {
        QCborParserError error{};
        const QCborValue value = QCborValue::fromCbor(body, &error);
        if (error.error != QCborError::NoError || error.offset != body.size() || !value.isMap())
            return std::nullopt;
        const QCborMap map = value.toMap();

        const QByteArray accountBytes =
            fixedBytes(map.value(QLatin1StringView("account_id")), AccountId::byteCount);
        const auto accountId = AccountId::fromBytes(accountBytes);
        if (!accountId)
            return std::nullopt;

        const QCborValue devicesValue = map.value(QLatin1StringView("devices"));
        if (!devicesValue.isArray())
            return std::nullopt;
        const QCborArray devices = devicesValue.toArray();
        if (devices.size() > maxDirectoryDevices)
            return std::nullopt;

        RelayDirectoryEntry entry{*accountId, {}};
        entry.devices.reserve(devices.size());
        for (const QCborValue &deviceValue : devices) {
            if (!deviceValue.isMap())
                return std::nullopt;
            const QCborMap deviceMap = deviceValue.toMap();
            const auto deviceId = DeviceId::fromBytes(
                fixedBytes(deviceMap.value(QLatin1StringView("device_id")), DeviceId::byteCount));
            if (!deviceId)
                return std::nullopt;
            const QByteArray signingKey = fixedBytes(
                deviceMap.value(QLatin1StringView("signing_key")), directorySigningKeyBytes);
            if (signingKey.size() != directorySigningKeyBytes)
                return std::nullopt;
            entry.devices.append(RelayDirectoryDevice{*deviceId, signingKey});
        }
        return entry;
    }

    void teardownSocket()
    {
        if (!socket)
            return;
        QObject::disconnect(socket, nullptr, q, nullptr);
        socket->abort();
        socket->deleteLater();
        socket = nullptr;
    }

    RelayClient *q;
    DeviceId localDeviceId;
    AccountId localAccountId;
    RelayEndpoints endpoints;
    RelayCredentials credentials;
    RelayLimits limits;
    BackoffPolicy backoff;

    QNetworkAccessManager *network = nullptr;
    QWebSocket *socket = nullptr;
    QTimer *reconnectTimer = nullptr;
    QTimer *connectDeadline = nullptr;

    QSslConfiguration tlsConfig;
    bool tlsConfigured = false;

    // In-memory session tokens installed by setTokens(). The credential
    // callbacks are re-pointed at these so every existing token fetch (headers,
    // refresh) keeps working unchanged. Held only for the session lifetime and
    // never logged or persisted.
    QByteArray heldAccessToken;
    QByteArray heldRefreshToken;

    quint64 resumeWatermark = 0;
    int reconnectAttempt = 0;
    bool subprotocolVerified = false;
    bool userClosed = false;
    bool suppressReconnect = false;
    bool tlsErrorReported = false;
    bool refreshInFlight = false;
    bool refreshAttemptedThisCycle = false;
};

// ---------------------------------------------------------------------------

RelayClient::RelayClient(DeviceId localDeviceId, AccountId localAccountId, RelayEndpoints endpoints,
                         RelayCredentials credentials, RelayLimits limits,
                         BackoffPolicy backoff, QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Private>(std::move(localDeviceId), std::move(localAccountId),
                                  std::move(endpoints), std::move(credentials), limits,
                                  std::move(backoff), this))
{
}

RelayClient::~RelayClient()
{
    d->userClosed = true;
    d->teardownSocket();
}

void RelayClient::setTlsConfiguration(const QSslConfiguration &configuration)
{
    d->tlsConfig = configuration;
    d->tlsConfigured = true;
}

void RelayClient::setTokens(const QByteArray &accessToken, const QByteArray &refreshToken)
{
    d->heldAccessToken = accessToken;
    d->heldRefreshToken = refreshToken;
    // Re-point the credential callbacks at the held tokens. The lambdas capture
    // the Private object that owns them, so they never outlive the storage they
    // read. All existing token-fetch sites (authorizedRequest, openSocket, the
    // refresh paths) go through these callbacks and pick up the rotation.
    Private *p = d.get();
    d->credentials.accessToken = [p] { return p->heldAccessToken; };
    d->credentials.refreshToken = [p] { return p->heldRefreshToken; };
}

void RelayClient::authenticateDevice(const QByteArray &deviceCredential,
                                     const ChallengeSigner &signer, const QByteArray &context)
{
    // The relay reads the device credential only at registration; the challenge
    // and complete endpoints authenticate by account_id + device_id plus a
    // signature over the bound context, so the credential is not transmitted here.
    (void)deviceCredential;

    if (!isHttps(d->endpoints.authChallenge) || !isHttps(d->endpoints.authComplete)) {
        emit transportError(RelayTransportError::InsecureEndpoint);
        return;
    }
    if (!signer) {
        emit authExpired();
        return;
    }
    // The context is the value the device signs, and the relay rejects an empty
    // one, so fail closed here rather than emit a request the relay will refuse.
    if (context.isEmpty()) {
        emit authExpired();
        return;
    }

    QCborMap challengeBody;
    challengeBody.insert(QLatin1StringView("account_id"), d->localAccountId.bytes());
    challengeBody.insert(QLatin1StringView("device_id"), d->localDeviceId.bytes());

    QNetworkRequest request = d->authorizedRequest(d->endpoints.authChallenge);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/cbor"));
    QNetworkReply *reply = d->network->post(request, challengeBody.toCborValue().toCbor());
    d->guardReply(reply);

    connect(reply, &QNetworkReply::finished, this,
            [this, reply, signer, context] {
                reply->deleteLater();
                if (reply->error() != QNetworkReply::NoError || !d->bodyWithinBounds(reply)) {
                    emit transportError(Private::httpFailureKind(reply));
                    return;
                }
                const QByteArray body = reply->readAll();
                if (body.size() > d->limits.maxHttpBodyBytes) {
                    emit transportError(RelayTransportError::BodyTooLarge);
                    return;
                }
                QCborParserError error{};
                const QCborValue value = QCborValue::fromCbor(body, &error);
                if (error.error != QCborError::NoError || error.offset != body.size()
                    || !value.isMap()) {
                    emit transportError(RelayTransportError::MalformedEnvelope);
                    return;
                }
                const QByteArray challenge =
                    value.toMap().value(QLatin1StringView("challenge")).toByteArray();
                if (challenge.isEmpty() || challenge.size() > 128) {
                    emit transportError(RelayTransportError::MalformedEnvelope);
                    return;
                }
                completeAuthentication(challenge, signer, context);
            });
}

void RelayClient::connectLive(quint64 resumeWatermark)
{
    d->userClosed = false;
    d->suppressReconnect = false;
    d->refreshAttemptedThisCycle = false;
    d->resumeWatermark = resumeWatermark;
    d->reconnectAttempt = 0;
    d->openSocket();
}

Result<void, RelayCallError> RelayClient::sendEnvelope(const CiphertextEnvelopeV1 &envelope)
{
    if (!d->socket || !d->subprotocolVerified)
        return Result<void, RelayCallError>::failure(RelayCallError::NotConnected);

    const QByteArray frame = encodeCanonical(envelope);
    if (frame.isEmpty() || static_cast<quint64>(frame.size()) > d->limits.maxIncomingFrameBytes)
        return Result<void, RelayCallError>::failure(RelayCallError::EncodeFailure);

    d->socket->sendBinaryMessage(frame);
    return Result<void, RelayCallError>::success();
}

Result<void, RelayCallError> RelayClient::acknowledge(const EnvelopeId &envelopeId,
                                                      quint64 watermark)
{
    if (!d->socket || !d->subprotocolVerified)
        return Result<void, RelayCallError>::failure(RelayCallError::NotConnected);

    QCborArray control;
    control.append(static_cast<int>(ControlType::Acknowledge));
    control.append(envelopeId.bytes());
    control.append(static_cast<qint64>(watermark));

    d->socket->sendBinaryMessage(control.toCborValue().toCbor());
    return Result<void, RelayCallError>::success();
}

void RelayClient::fetchSince(quint64 watermark)
{
    if (!isHttps(d->endpoints.sync)) {
        emit transportError(RelayTransportError::InsecureEndpoint);
        return;
    }

    QUrl url = d->endpoints.sync;
    QUrlQuery query(url);
    query.removeQueryItem(QStringLiteral("since"));
    query.addQueryItem(QStringLiteral("since"), QString::number(watermark));
    url.setQuery(query);

    QNetworkRequest request = d->authorizedRequest(url);
    QNetworkReply *reply = d->network->get(request);
    d->guardReply(reply);

    connect(reply, &QNetworkReply::finished, this, [this, reply, watermark] {
        const bool oversized = reply->property("oc_oversized").toBool();
        reply->deleteLater();

        if (oversized) {
            emit transportError(RelayTransportError::BodyTooLarge);
            return;
        }

        const QVariant statusVar = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        const int status = statusVar.isValid() ? statusVar.toInt() : 0;
        if (status == 401) {
            // Authorization rejected: attempt exactly one serialized refresh,
            // then retry this fetch once.
            if (!d->refreshAttemptedThisCycle && !d->refreshInFlight) {
                refreshThenRetryFetch(watermark);
            } else {
                emit authExpired();
            }
            return;
        }
        if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300
            || !d->bodyWithinBounds(reply)) {
            emit transportError(Private::httpFailureKind(reply));
            return;
        }

        const QByteArray body = reply->readAll();
        if (body.size() > d->limits.maxHttpBodyBytes) {
            emit transportError(RelayTransportError::BodyTooLarge);
            return;
        }
        d->refreshAttemptedThisCycle = false; // authorized round-trip succeeded
        deliverCatchUp(body);
    });
}

void RelayClient::registerAccount(const AccountId &account, const DeviceId &device,
                                  const QString &handle, const QByteArray &signingKey,
                                  const QByteArray &credential)
{
    if (!isHttps(d->endpoints.accounts)) {
        emit transportError(RelayTransportError::InsecureEndpoint);
        return;
    }

    QCborMap body;
    body.insert(QLatin1StringView("account_id"), account.bytes());
    body.insert(QLatin1StringView("device_id"), device.bytes());
    body.insert(QLatin1StringView("handle"), handle);
    body.insert(QLatin1StringView("signing_key"), signingKey);
    body.insert(QLatin1StringView("credential"), credential);

    // Bootstrap registration is unauthenticated: issue the request through the
    // hardened path that attaches NO bearer token, so no access token ever leaks
    // onto this public endpoint even when the caller already holds one.
    QNetworkRequest request = d->hardenedRequest(d->endpoints.accounts);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/cbor"));
    QNetworkReply *reply = d->network->post(request, body.toCborValue().toCbor());
    d->guardReply(reply);

    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        const QVariant statusVar = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        const int status = statusVar.isValid() ? statusVar.toInt() : 0;
        if (status == 409) {
            emit accountRegistrationFailed(RelayRegistrationError::HandleUnavailable);
            return;
        }
        if (status == 400) {
            emit accountRegistrationFailed(RelayRegistrationError::InvalidRequest);
            return;
        }
        if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300
            || !d->bodyWithinBounds(reply)) {
            emit accountRegistrationFailed(RelayRegistrationError::Transport);
            return;
        }
        emit accountRegistered();
    });
}

void RelayClient::publishKeyPackage(const QByteArray &keyPackage)
{
    if (!isHttps(d->endpoints.keyPackages)) {
        emit transportError(RelayTransportError::InsecureEndpoint);
        return;
    }

    QCborMap body;
    body.insert(QLatin1StringView("key_package"), keyPackage);

    QNetworkRequest request = d->authorizedRequest(d->endpoints.keyPackages);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/cbor"));
    QNetworkReply *reply = d->network->post(request, body.toCborValue().toCbor());
    d->guardReply(reply);

    connect(reply, &QNetworkReply::finished, this, [this, reply, keyPackage] {
        reply->deleteLater();
        const QVariant statusVar = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        const int status = statusVar.isValid() ? statusVar.toInt() : 0;
        if (status == 401) {
            // Authorization rejected: attempt exactly one serialized refresh, then
            // retry this publish once, mirroring the fetch path.
            if (!d->refreshAttemptedThisCycle && !d->refreshInFlight) {
                refreshThenRetryPublish(keyPackage);
            } else {
                emit authExpired();
            }
            return;
        }
        if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300
            || !d->bodyWithinBounds(reply)) {
            emit keyPackagePublishFailed();
            return;
        }
        d->refreshAttemptedThisCycle = false; // authorized round-trip succeeded
        emit keyPackagePublished();
    });
}

void RelayClient::resolveHandle(const QString &handle)
{
    if (!isHttps(d->endpoints.directory)) {
        emit transportError(RelayTransportError::InsecureEndpoint);
        return;
    }

    QUrl url = d->endpoints.directory;
    QUrlQuery query(url);
    query.removeQueryItem(QStringLiteral("handle"));
    // addQueryItem percent-encodes the value, so a handle with reserved
    // characters cannot smuggle extra query items or path segments.
    query.addQueryItem(QStringLiteral("handle"), handle);
    url.setQuery(query);

    QNetworkRequest request = d->authorizedRequest(url);
    QNetworkReply *reply = d->network->get(request);
    d->guardReply(reply);

    connect(reply, &QNetworkReply::finished, this, [this, reply, handle] {
        const bool oversized = reply->property("oc_oversized").toBool();
        reply->deleteLater();
        if (oversized) {
            emit handleResolutionFailed(RelayDirectoryError::Transport);
            return;
        }

        const QVariant statusVar = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        const int status = statusVar.isValid() ? statusVar.toInt() : 0;
        if (status == 401) {
            // Authorization rejected: attempt exactly one serialized refresh, then
            // retry this lookup once, mirroring the fetch/publish paths.
            if (!d->refreshAttemptedThisCycle && !d->refreshInFlight)
                refreshThenRetry([this, handle] { resolveHandle(handle); });
            else
                emit authExpired();
            return;
        }
        if (status == 404) {
            emit handleResolutionFailed(RelayDirectoryError::NotFound);
            return;
        }
        if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300
            || !d->bodyWithinBounds(reply)) {
            emit handleResolutionFailed(RelayDirectoryError::Transport);
            return;
        }

        const QByteArray body = reply->readAll();
        if (body.size() > d->limits.maxHttpBodyBytes) {
            emit handleResolutionFailed(RelayDirectoryError::Transport);
            return;
        }
        const auto entry = d->parseDirectoryEntry(body);
        if (!entry) {
            emit handleResolutionFailed(RelayDirectoryError::Malformed);
            return;
        }
        d->refreshAttemptedThisCycle = false; // authorized round-trip succeeded
        emit handleResolved(*entry);
    });
}

void RelayClient::resolveAccount(const AccountId &account)
{
    if (!isHttps(d->endpoints.directoryAccount)) {
        emit transportError(RelayTransportError::InsecureEndpoint);
        return;
    }

    QUrl url = d->endpoints.directoryAccount;
    QUrlQuery query(url);
    query.removeQueryItem(QStringLiteral("account_id"));
    query.addQueryItem(QStringLiteral("account_id"), account.toHex());
    url.setQuery(query);

    QNetworkRequest request = d->authorizedRequest(url);
    QNetworkReply *reply = d->network->get(request);
    d->guardReply(reply);

    connect(reply, &QNetworkReply::finished, this, [this, reply, account] {
        const bool oversized = reply->property("oc_oversized").toBool();
        reply->deleteLater();
        if (oversized) {
            emit accountResolutionFailed(account, RelayDirectoryError::Transport);
            return;
        }

        const QVariant statusVar = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        const int status = statusVar.isValid() ? statusVar.toInt() : 0;
        if (status == 401) {
            if (!d->refreshAttemptedThisCycle && !d->refreshInFlight)
                refreshThenRetry([this, account] { resolveAccount(account); });
            else
                emit authExpired();
            return;
        }
        if (status == 404) {
            emit accountResolutionFailed(account, RelayDirectoryError::NotFound);
            return;
        }
        if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300
            || !d->bodyWithinBounds(reply)) {
            emit accountResolutionFailed(account, RelayDirectoryError::Transport);
            return;
        }

        const QByteArray body = reply->readAll();
        if (body.size() > d->limits.maxHttpBodyBytes) {
            emit accountResolutionFailed(account, RelayDirectoryError::Transport);
            return;
        }
        // Shape: { handle: text }. The handle is bounded exactly as the relay
        // bounds it at registration (1..64 characters, no whitespace).
        QCborParserError error{};
        const QCborValue value = QCborValue::fromCbor(body, &error);
        if (error.error != QCborError::NoError || error.offset != body.size() || !value.isMap()) {
            emit accountResolutionFailed(account, RelayDirectoryError::Malformed);
            return;
        }
        const QCborValue handleValue = value.toMap().value(QLatin1StringView("handle"));
        const QString handle = handleValue.isString() ? handleValue.toString() : QString();
        if (handle.isEmpty() || handle.size() > 64
            || std::any_of(handle.cbegin(), handle.cend(), [](QChar c) { return c.isSpace(); })) {
            emit accountResolutionFailed(account, RelayDirectoryError::Malformed);
            return;
        }
        d->refreshAttemptedThisCycle = false; // authorized round-trip succeeded
        emit accountResolved(account, handle);
    });
}

void RelayClient::createInvite(qint64 ttlMs)
{
    if (!isHttps(d->endpoints.invites)) {
        emit transportError(RelayTransportError::InsecureEndpoint);
        return;
    }

    QCborMap body;
    if (ttlMs > 0)
        body.insert(QLatin1StringView("ttl_ms"), ttlMs);

    QNetworkRequest request = d->authorizedRequest(d->endpoints.invites);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/cbor"));
    QNetworkReply *reply = d->network->post(request, body.toCborValue().toCbor());
    d->guardReply(reply);

    connect(reply, &QNetworkReply::finished, this, [this, reply, ttlMs] {
        const bool oversized = reply->property("oc_oversized").toBool();
        reply->deleteLater();
        if (oversized) {
            emit inviteCreationFailed();
            return;
        }

        const QVariant statusVar = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        const int status = statusVar.isValid() ? statusVar.toInt() : 0;
        if (status == 401) {
            if (!d->refreshAttemptedThisCycle && !d->refreshInFlight)
                refreshThenRetry([this, ttlMs] { createInvite(ttlMs); });
            else
                emit authExpired();
            return;
        }
        if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300
            || !d->bodyWithinBounds(reply)) {
            emit inviteCreationFailed();
            return;
        }

        const QByteArray rawBody = reply->readAll();
        if (rawBody.size() > d->limits.maxHttpBodyBytes) {
            emit inviteCreationFailed();
            return;
        }
        QCborParserError error{};
        const QCborValue value = QCborValue::fromCbor(rawBody, &error);
        if (error.error != QCborError::NoError || error.offset != rawBody.size()
            || !value.isMap()) {
            emit inviteCreationFailed();
            return;
        }
        const QCborMap map = value.toMap();
        const QCborValue tokenValue = map.value(QLatin1StringView("token"));
        const QCborValue expiresValue = map.value(QLatin1StringView("expires_at_ms"));
        if (!tokenValue.isByteArray() || !expiresValue.isInteger()) {
            emit inviteCreationFailed();
            return;
        }
        const QByteArray token = tokenValue.toByteArray();
        if (token.isEmpty() || token.size() > maxInviteTokenBytes) {
            emit inviteCreationFailed();
            return;
        }
        d->refreshAttemptedThisCycle = false; // authorized round-trip succeeded
        emit inviteCreated(token, expiresValue.toInteger());
    });
}

void RelayClient::redeemInvite(const QByteArray &token)
{
    if (!isHttps(d->endpoints.invitesRedeem)) {
        emit transportError(RelayTransportError::InsecureEndpoint);
        return;
    }

    QCborMap body;
    body.insert(QLatin1StringView("token"), token);

    QNetworkRequest request = d->authorizedRequest(d->endpoints.invitesRedeem);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/cbor"));
    QNetworkReply *reply = d->network->post(request, body.toCborValue().toCbor());
    d->guardReply(reply);

    connect(reply, &QNetworkReply::finished, this, [this, reply, token] {
        const bool oversized = reply->property("oc_oversized").toBool();
        reply->deleteLater();
        if (oversized) {
            emit inviteRedemptionFailed(RelayDirectoryError::Transport);
            return;
        }

        const QVariant statusVar = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        const int status = statusVar.isValid() ? statusVar.toInt() : 0;
        if (status == 401) {
            if (!d->refreshAttemptedThisCycle && !d->refreshInFlight)
                refreshThenRetry([this, token] { redeemInvite(token); });
            else
                emit authExpired();
            return;
        }
        if (status == 404) {
            emit inviteRedemptionFailed(RelayDirectoryError::NotFound);
            return;
        }
        if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300
            || !d->bodyWithinBounds(reply)) {
            emit inviteRedemptionFailed(RelayDirectoryError::Transport);
            return;
        }

        const QByteArray rawBody = reply->readAll();
        if (rawBody.size() > d->limits.maxHttpBodyBytes) {
            emit inviteRedemptionFailed(RelayDirectoryError::Transport);
            return;
        }
        const auto entry = d->parseDirectoryEntry(rawBody);
        if (!entry) {
            emit inviteRedemptionFailed(RelayDirectoryError::Malformed);
            return;
        }
        d->refreshAttemptedThisCycle = false; // authorized round-trip succeeded
        emit inviteRedeemed(*entry);
    });
}

void RelayClient::claimKeyPackage(const DeviceId &targetDevice)
{
    if (!isHttps(d->endpoints.keyPackagesClaim)) {
        emit transportError(RelayTransportError::InsecureEndpoint);
        return;
    }

    QCborMap body;
    body.insert(QLatin1StringView("target_device_id"), targetDevice.bytes());

    QNetworkRequest request = d->authorizedRequest(d->endpoints.keyPackagesClaim);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/cbor"));
    QNetworkReply *reply = d->network->post(request, body.toCborValue().toCbor());
    d->guardReply(reply);

    connect(reply, &QNetworkReply::finished, this, [this, reply, targetDevice] {
        const bool oversized = reply->property("oc_oversized").toBool();
        reply->deleteLater();
        if (oversized) {
            emit keyPackageClaimFailed(RelayClaimError::Transport);
            return;
        }

        const QVariant statusVar = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        const int status = statusVar.isValid() ? statusVar.toInt() : 0;
        if (status == 401) {
            if (!d->refreshAttemptedThisCycle && !d->refreshInFlight)
                refreshThenRetry([this, targetDevice] { claimKeyPackage(targetDevice); });
            else
                emit authExpired();
            return;
        }
        // The relay's KeyPackageService::claim maps a target device with no
        // unclaimed KeyPackage to RelayError::NotFound, i.e. 404 Not Found.
        if (status == 404) {
            emit keyPackageClaimFailed(RelayClaimError::Unavailable);
            return;
        }
        if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300
            || !d->bodyWithinBounds(reply)) {
            emit keyPackageClaimFailed(RelayClaimError::Transport);
            return;
        }

        const QByteArray rawBody = reply->readAll();
        if (rawBody.size() > d->limits.maxHttpBodyBytes) {
            emit keyPackageClaimFailed(RelayClaimError::Transport);
            return;
        }
        QCborParserError error{};
        const QCborValue value = QCborValue::fromCbor(rawBody, &error);
        if (error.error != QCborError::NoError || error.offset != rawBody.size()
            || !value.isMap()) {
            emit keyPackageClaimFailed(RelayClaimError::Malformed);
            return;
        }
        const QCborValue keyPackageValue = value.toMap().value(QLatin1StringView("key_package"));
        if (!keyPackageValue.isByteArray()) {
            emit keyPackageClaimFailed(RelayClaimError::Malformed);
            return;
        }
        // The key_package is bounded by the already-gated body size; only its
        // non-emptiness needs an explicit check.
        const QByteArray keyPackage = keyPackageValue.toByteArray();
        if (keyPackage.isEmpty()) {
            emit keyPackageClaimFailed(RelayClaimError::Malformed);
            return;
        }
        d->refreshAttemptedThisCycle = false; // authorized round-trip succeeded
        emit keyPackageClaimed(keyPackage);
    });
}

void RelayClient::disconnect()
{
    d->userClosed = true;
    d->reconnectTimer->stop();
    d->connectDeadline->stop();
    if (d->socket)
        d->socket->close(QWebSocketProtocol::CloseCodeNormal, QStringLiteral("client disconnect"));
}

bool RelayClient::isConnected() const noexcept
{
    return d->socket != nullptr && d->subprotocolVerified;
}

// ---- private helpers implemented on RelayClient for signal access ----------

void RelayClient::completeAuthentication(const QByteArray &challenge, const ChallengeSigner &signer,
                                         const QByteArray &context)
{
    const QByteArray signature = signer(challenge, context);
    if (signature.isEmpty()) {
        emit authExpired();
        return;
    }

    QCborMap body;
    body.insert(QLatin1StringView("account_id"), d->localAccountId.bytes());
    body.insert(QLatin1StringView("device_id"), d->localDeviceId.bytes());
    body.insert(QLatin1StringView("challenge"), challenge);
    body.insert(QLatin1StringView("signature"), signature);
    body.insert(QLatin1StringView("context"), context);

    QNetworkRequest request = d->authorizedRequest(d->endpoints.authComplete);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/cbor"));
    QNetworkReply *reply = d->network->post(request, body.toCborValue().toCbor());
    d->guardReply(reply);

    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError || !d->bodyWithinBounds(reply)) {
            emit transportError(Private::httpFailureKind(reply));
            return;
        }
        const auto session = d->parseSession(reply->readAll());
        if (!session) {
            emit transportError(RelayTransportError::MalformedEnvelope);
            return;
        }
        emit authenticated(*session);
    });
}

void RelayClient::refreshThenRetryFetch(quint64 watermark)
{
    if (!d->credentials.refreshToken || !isHttps(d->endpoints.authRefresh)) {
        emit authExpired();
        return;
    }
    const QByteArray refresh = d->credentials.refreshToken();
    if (refresh.isEmpty()) {
        emit authExpired();
        return;
    }

    d->refreshAttemptedThisCycle = true;
    d->refreshInFlight = true;

    QCborMap body;
    body.insert(QLatin1StringView("refresh"), refresh);
    QNetworkRequest request = d->authorizedRequest(d->endpoints.authRefresh);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/cbor"));
    QNetworkReply *reply = d->network->post(request, body.toCborValue().toCbor());
    d->guardReply(reply);

    connect(reply, &QNetworkReply::finished, this, [this, reply, watermark] {
        d->refreshInFlight = false;
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError || !d->bodyWithinBounds(reply)) {
            emit authExpired();
            return;
        }
        const auto session = d->parseSession(reply->readAll());
        if (!session) {
            emit authExpired();
            return;
        }
        emit tokensRotated(*session);
        // Retry the fetch exactly once with the rotated credentials in place.
        fetchSince(watermark);
    });
}

void RelayClient::refreshThenRetryPublish(const QByteArray &keyPackage)
{
    if (!d->credentials.refreshToken || !isHttps(d->endpoints.authRefresh)) {
        emit authExpired();
        return;
    }
    const QByteArray refresh = d->credentials.refreshToken();
    if (refresh.isEmpty()) {
        emit authExpired();
        return;
    }

    d->refreshAttemptedThisCycle = true;
    d->refreshInFlight = true;

    QCborMap body;
    body.insert(QLatin1StringView("refresh"), refresh);
    QNetworkRequest request = d->authorizedRequest(d->endpoints.authRefresh);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/cbor"));
    QNetworkReply *reply = d->network->post(request, body.toCborValue().toCbor());
    d->guardReply(reply);

    connect(reply, &QNetworkReply::finished, this, [this, reply, keyPackage] {
        d->refreshInFlight = false;
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError || !d->bodyWithinBounds(reply)) {
            emit authExpired();
            return;
        }
        const auto session = d->parseSession(reply->readAll());
        if (!session) {
            emit authExpired();
            return;
        }
        emit tokensRotated(*session);
        // Retry the publish exactly once; refreshAttemptedThisCycle stays set so a
        // repeated 401 falls through to authExpired() instead of looping.
        publishKeyPackage(keyPackage);
    });
}

void RelayClient::refreshThenRetry(std::function<void()> retry)
{
    if (!d->credentials.refreshToken || !isHttps(d->endpoints.authRefresh)) {
        emit authExpired();
        return;
    }
    const QByteArray refresh = d->credentials.refreshToken();
    if (refresh.isEmpty()) {
        emit authExpired();
        return;
    }

    d->refreshAttemptedThisCycle = true;
    d->refreshInFlight = true;

    QCborMap body;
    body.insert(QLatin1StringView("refresh"), refresh);
    QNetworkRequest request = d->authorizedRequest(d->endpoints.authRefresh);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/cbor"));
    QNetworkReply *reply = d->network->post(request, body.toCborValue().toCbor());
    d->guardReply(reply);

    connect(reply, &QNetworkReply::finished, this, [this, reply, retry = std::move(retry)] {
        d->refreshInFlight = false;
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError || !d->bodyWithinBounds(reply)) {
            emit authExpired();
            return;
        }
        const auto session = d->parseSession(reply->readAll());
        if (!session) {
            emit authExpired();
            return;
        }
        emit tokensRotated(*session);
        // Retry the original operation exactly once with the rotated credentials
        // in place; refreshAttemptedThisCycle stays set so a repeated 401 falls
        // through to authExpired() instead of looping.
        retry();
    });
}

void RelayClient::deliverCatchUp(const QByteArray &body)
{
    // Stream the batch rather than materializing the whole CBOR array: the item
    // count is enforced before each element is allocated, so a body full of tiny
    // elements cannot amplify into a large transient DOM. Layout is flat:
    // [ uint watermark, uint seq, bstr envelope, uint seq, bstr envelope, ... ].
    QCborStreamReader reader(body);
    if (reader.lastError() != QCborError::NoError || !reader.isArray()) {
        emit transportError(RelayTransportError::MalformedEnvelope);
        return;
    }
    reader.enterContainer();
    if (reader.lastError() != QCborError::NoError || !reader.hasNext()
        || !reader.isUnsignedInteger()) {
        emit transportError(RelayTransportError::MalformedEnvelope);
        return;
    }
    const quint64 newWatermark = reader.toUnsignedInteger();
    reader.next();

    DecodeLimits decodeLimits;
    decodeLimits.envelopeBytes = static_cast<qsizetype>(d->limits.maxIncomingMessageBytes);

    QPointer<RelayClient> guard(this);
    int count = 0;
    while (reader.hasNext()) {
        // Each item is a (sequence, envelope) pair.
        if (reader.lastError() != QCborError::NoError || !reader.isUnsignedInteger()) {
            emit transportError(RelayTransportError::MalformedEnvelope);
            return;
        }
        if (count >= d->limits.maxCatchUpEnvelopes) {
            emit transportError(RelayTransportError::FrameTooLarge);
            return;
        }
        ++count;
        const quint64 sequence = reader.toUnsignedInteger();
        reader.next();

        if (reader.lastError() != QCborError::NoError || !reader.isByteArray()) {
            emit transportError(RelayTransportError::MalformedEnvelope);
            return;
        }
        QByteArray envelopeBytes;
        auto chunk = reader.readByteArray();
        while (chunk.status == QCborStreamReader::Ok) {
            if (envelopeBytes.size() + chunk.data.size() > decodeLimits.envelopeBytes + 1) {
                emit transportError(RelayTransportError::FrameTooLarge);
                return;
            }
            envelopeBytes += chunk.data;
            chunk = reader.readByteArray();
        }
        if (chunk.status != QCborStreamReader::EndOfString) {
            emit transportError(RelayTransportError::MalformedEnvelope);
            return;
        }

        const auto decoded = decodeEnvelope(envelopeBytes, decodeLimits);
        if (!decoded.hasValue()) {
            emit transportError(RelayTransportError::MalformedEnvelope);
            return;
        }
        if (decoded.value().recipientDeviceId != d->localDeviceId) {
            emit transportError(RelayTransportError::WrongRecipient);
            return;
        }
        emit envelopeReceived(decoded.value(), sequence);
        if (!guard) // a consumer slot destroyed us mid-delivery
            return;
    }

    emit catchUpComplete(newWatermark);
}

} // namespace OpenChat
