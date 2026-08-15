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
    Private(DeviceId localDeviceId, RelayEndpoints endpoints, RelayCredentials credentials,
            RelayLimits limits, BackoffPolicy backoff, RelayClient *owner)
        : q(owner)
        , localDeviceId(std::move(localDeviceId))
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

    // Builds a hardened authorized HTTPS request. The access token is fetched
    // fresh from the callback and only lives inside this request; RelayClient
    // keeps no member copy.
    [[nodiscard]] QNetworkRequest authorizedRequest(const QUrl &url) const
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
        if (credentials.accessToken) {
            const QByteArray token = credentials.accessToken();
            if (!token.isEmpty())
                request.setRawHeader(QByteArrayLiteral("Authorization"),
                                     QByteArrayLiteral("Bearer ") + token);
        }
        request.setRawHeader(QByteArrayLiteral("Accept"),
                             QByteArrayLiteral("application/cbor"));
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

        const int major = cborMajorType(message);
        if (major == 5) {
            handleEnvelopeFrame(message);
            return;
        }
        if (major == 4) {
            handleControlFrame(message);
            return;
        }
        emit q->transportError(RelayTransportError::InvalidControlFrame);
    }

    void handleEnvelopeFrame(const QByteArray &message)
    {
        DecodeLimits decodeLimits;
        decodeLimits.envelopeBytes = static_cast<qsizetype>(limits.maxIncomingMessageBytes);
        const auto decoded = decodeEnvelope(message, decodeLimits);
        if (!decoded.hasValue()) {
            emit q->transportError(RelayTransportError::MalformedEnvelope);
            return;
        }

        const CiphertextEnvelopeV1 &envelope = decoded.value();
        if (envelope.recipientDeviceId != localDeviceId) {
            emit q->transportError(RelayTransportError::WrongRecipient);
            return;
        }

        emit q->envelopeReceived(envelope);
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

RelayClient::RelayClient(DeviceId localDeviceId, RelayEndpoints endpoints,
                         RelayCredentials credentials, RelayLimits limits,
                         BackoffPolicy backoff, QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Private>(std::move(localDeviceId), std::move(endpoints),
                                  std::move(credentials), limits, std::move(backoff), this))
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

void RelayClient::authenticateDevice(const QByteArray &deviceCredential,
                                     const ChallengeSigner &signer, const QByteArray &context)
{
    if (!isHttps(d->endpoints.authChallenge) || !isHttps(d->endpoints.authComplete)) {
        emit transportError(RelayTransportError::InsecureEndpoint);
        return;
    }
    if (!signer) {
        emit authExpired();
        return;
    }

    QCborMap challengeBody;
    challengeBody.insert(QLatin1StringView("credential"), deviceCredential);
    challengeBody.insert(QLatin1StringView("context"), context);

    QNetworkRequest request = d->authorizedRequest(d->endpoints.authChallenge);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/cbor"));
    QNetworkReply *reply = d->network->post(request, challengeBody.toCborValue().toCbor());
    d->guardReply(reply);

    connect(reply, &QNetworkReply::finished, this,
            [this, reply, deviceCredential, signer, context] {
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
                completeAuthentication(deviceCredential, challenge, signer, context);
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

void RelayClient::completeAuthentication(const QByteArray &deviceCredential,
                                         const QByteArray &challenge,
                                         const ChallengeSigner &signer, const QByteArray &context)
{
    const QByteArray signature = signer(challenge, context);
    if (signature.isEmpty()) {
        emit authExpired();
        return;
    }

    QCborMap body;
    body.insert(QLatin1StringView("credential"), deviceCredential);
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

void RelayClient::deliverCatchUp(const QByteArray &body)
{
    // Stream the batch rather than materializing the whole CBOR array: the item
    // count is enforced before each element is allocated, so a body full of tiny
    // elements cannot amplify into a large transient DOM. Layout is
    // [ uint watermark, bstr envelope, bstr envelope, ... ].
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
        if (reader.lastError() != QCborError::NoError || !reader.isByteArray()) {
            emit transportError(RelayTransportError::MalformedEnvelope);
            return;
        }
        if (count >= d->limits.maxCatchUpEnvelopes) {
            emit transportError(RelayTransportError::FrameTooLarge);
            return;
        }
        ++count;

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
        emit envelopeReceived(decoded.value());
        if (!guard) // a consumer slot destroyed us mid-delivery
            return;
    }

    emit catchUpComplete(newWatermark);
}

} // namespace OpenChat
