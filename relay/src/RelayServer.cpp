#include "RelayServer.h"

#include "RelayTypes.h"

#include "protocol/CanonicalCborCodec.h"
#include "protocol/CiphertextEnvelope.h"

#include <QCborArray>
#include <QCborMap>
#include <QCborValue>
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QHttpServerWebSocketUpgradeResponse>
#include <QNetworkRequest>
#include <QTcpServer>
#include <QUrlQuery>
#include <QWebSocket>

#include <optional>

namespace OpenChat::Relay {

namespace {

using StatusCode = QHttpServerResponder::StatusCode;

StatusCode statusFor(RelayError error)
{
    switch (error) {
    case RelayError::Ok:
        return StatusCode::Ok;
    case RelayError::InvalidRequest:
        return StatusCode::BadRequest;
    case RelayError::NotFound:
        return StatusCode::NotFound;
    case RelayError::Unauthorized:
    case RelayError::TokenReuse:
        return StatusCode::Unauthorized;
    case RelayError::Revoked:
        return StatusCode::Forbidden;
    case RelayError::Conflict:
        return StatusCode::Conflict;
    case RelayError::Expired:
        return StatusCode::Unauthorized;
    case RelayError::RateLimited:
        return StatusCode::TooManyRequests;
    case RelayError::Internal:
        break;
    }
    return StatusCode::InternalServerError;
}

QHttpServerResponse cbor(const QCborValue &value, StatusCode status = StatusCode::Ok)
{
    return QHttpServerResponse(QByteArrayLiteral("application/cbor"), value.toCbor(), status);
}

QHttpServerResponse errorResponse(RelayError error)
{
    return QHttpServerResponse(statusFor(error));
}

QByteArray bearerFromHeader(const QByteArray &header)
{
    if (!header.startsWith("Bearer "))
        return {};
    return header.mid(7).trimmed();
}

QByteArray bearerToken(const QHttpServerRequest &request)
{
    return bearerFromHeader(
        request.headers().value(QHttpHeaders::WellKnownHeader::Authorization).toByteArray());
}

QByteArray bearerToken(const QNetworkRequest &request)
{
    return bearerFromHeader(request.rawHeader(QByteArrayLiteral("Authorization")));
}

// NOTE: QHttpServer buffers the entire request body before invoking the handler,
// so this app-level bound rejects an oversize body but cannot prevent its
// allocation. The hard pre-read cap is enforced by the TLS-terminating reverse
// proxy (e.g. client_max_body_size); this check is defense in depth.
std::optional<QCborMap> boundedCborMap(const QHttpServerRequest &request, qint64 maxBytes)
{
    const auto declared =
        request.headers().value(QHttpHeaders::WellKnownHeader::ContentLength).toByteArray();
    bool ok = false;
    const qlonglong length = declared.toLongLong(&ok);
    if (ok && length > maxBytes)
        return std::nullopt;
    const QByteArray body = request.body();
    if (body.size() > maxBytes)
        return std::nullopt;
    QCborParserError parseError{};
    const QCborValue value = QCborValue::fromCbor(body, &parseError);
    if (parseError.error != QCborError::NoError || parseError.offset != body.size()
        || !value.isMap())
        return std::nullopt;
    return value.toMap();
}

template <typename Id>
std::optional<Id> idField(const QCborMap &map, QLatin1StringView key)
{
    const QCborValue value = map.value(key);
    if (!value.isByteArray())
        return std::nullopt;
    return Id::fromBytes(value.toByteArray());
}

int cborMajorType(const QByteArray &frame)
{
    if (frame.isEmpty())
        return -1;
    return (static_cast<unsigned char>(frame.at(0)) >> 5) & 0x07;
}

// Serializes a resolved directory entry to the shared discovery wire shape:
// { account_id, devices: [ { device_id, signing_key }, ... ] }. Only public
// routing/verification material is ever emitted.
QCborValue directoryCbor(const AccountDirectoryEntry &entry)
{
    QCborArray devices;
    for (const DirectoryDevice &device : entry.devices) {
        QCborMap deviceMap;
        deviceMap.insert(QLatin1StringView("device_id"), device.deviceId.bytes());
        deviceMap.insert(QLatin1StringView("signing_key"), device.signingKey);
        devices.append(deviceMap);
    }
    QCborMap response;
    response.insert(QLatin1StringView("account_id"), entry.accountId.bytes());
    response.insert(QLatin1StringView("devices"), devices);
    return response.toCborValue();
}

} // namespace

RelayServer::RelayServer(PostgresStore &store, AuthService &auth, EnvelopeService &envelopes,
                         KeyPackageService &keyPackages, DirectoryService &directory,
                         QObject *parent)
    : RelayServer(store, auth, envelopes, keyPackages, directory, Limits{}, parent)
{
}

RelayServer::RelayServer(PostgresStore &store, AuthService &auth, EnvelopeService &envelopes,
                         KeyPackageService &keyPackages, DirectoryService &directory, Limits limits,
                         QObject *parent)
    : QObject(parent)
    , m_store(store)
    , m_auth(auth)
    , m_envelopes(envelopes)
    , m_keyPackages(keyPackages)
    , m_directory(directory)
    , m_limits(limits)
{
}

RelayServer::~RelayServer() = default;

quint16 RelayServer::start(const QHostAddress &address, quint16 port)
{
    registerRoutes();

    m_http.addWebSocketUpgradeVerifier(
        this, [](const QHttpServerRequest &request) {
            if (request.url().path() == QLatin1String("/v1/live"))
                return QHttpServerWebSocketUpgradeResponse::accept();
            return QHttpServerWebSocketUpgradeResponse::passToNext();
        });
    connect(&m_http, &QAbstractHttpServer::newWebSocketConnection, this,
            &RelayServer::onWebSocketConnection);

    m_tcp = new QTcpServer(this);
    if (!m_tcp->listen(address, port))
        return 0;
    if (!m_http.bind(m_tcp))
        return 0;
    return m_tcp->serverPort();
}

void RelayServer::registerRoutes()
{
    m_http.route(
        QStringLiteral("/v1/accounts"), QHttpServerRequest::Method::Post,
        [this](const QHttpServerRequest &request) -> QHttpServerResponse {
            const auto map = boundedCborMap(request, m_limits.maxRequestBytes);
            if (!map)
                return QHttpServerResponse(StatusCode::BadRequest);
            const auto account = idField<AccountId>(*map, QLatin1StringView("account_id"));
            const auto device = idField<DeviceId>(*map, QLatin1StringView("device_id"));
            const QString handle = map->value(QLatin1StringView("handle")).toString();
            const QByteArray signingKey =
                map->value(QLatin1StringView("signing_key")).toByteArray();
            const QByteArray credential =
                map->value(QLatin1StringView("credential")).toByteArray();
            if (!account || !device || handle.isEmpty())
                return QHttpServerResponse(StatusCode::BadRequest);
            const auto result =
                m_auth.registerAccount(*account, handle, *device, signingKey, credential);
            if (!result.hasValue())
                return errorResponse(result.error());
            return QHttpServerResponse(StatusCode::Ok);
        });

    m_http.route(
        QStringLiteral("/v1/auth/challenge"), QHttpServerRequest::Method::Post,
        [this](const QHttpServerRequest &request) -> QHttpServerResponse {
            const auto map = boundedCborMap(request, m_limits.maxRequestBytes);
            if (!map)
                return QHttpServerResponse(StatusCode::BadRequest);
            const auto account = idField<AccountId>(*map, QLatin1StringView("account_id"));
            const auto device = idField<DeviceId>(*map, QLatin1StringView("device_id"));
            const int version =
                static_cast<int>(map->value(QLatin1StringView("protocol_version")).toInteger(1));
            if (!account || !device)
                return QHttpServerResponse(StatusCode::BadRequest);
            const auto challenge = m_auth.issueChallenge(*account, *device, version);
            if (!challenge.hasValue())
                return errorResponse(challenge.error());
            QCborMap response;
            response.insert(QLatin1StringView("challenge"), challenge.value());
            return cbor(response.toCborValue());
        });

    m_http.route(
        QStringLiteral("/v1/auth/complete"), QHttpServerRequest::Method::Post,
        [this](const QHttpServerRequest &request) -> QHttpServerResponse {
            const auto map = boundedCborMap(request, m_limits.maxRequestBytes);
            if (!map)
                return QHttpServerResponse(StatusCode::BadRequest);
            const auto account = idField<AccountId>(*map, QLatin1StringView("account_id"));
            const auto device = idField<DeviceId>(*map, QLatin1StringView("device_id"));
            const QByteArray challenge = map->value(QLatin1StringView("challenge")).toByteArray();
            const QByteArray signature = map->value(QLatin1StringView("signature")).toByteArray();
            const QByteArray context = map->value(QLatin1StringView("context")).toByteArray();
            if (!account || !device)
                return QHttpServerResponse(StatusCode::BadRequest);
            const auto tokens =
                m_auth.completeChallenge(*account, *device, challenge, signature, context);
            if (!tokens.hasValue())
                return errorResponse(tokens.error());
            QCborMap response;
            response.insert(QLatin1StringView("access"), tokens.value().accessToken);
            response.insert(QLatin1StringView("refresh"), tokens.value().refreshToken);
            response.insert(QLatin1StringView("expiresAtMs"), tokens.value().accessExpiresAtMs);
            response.insert(QLatin1StringView("refreshExpiresAtMs"), tokens.value().refreshExpiresAtMs);
            return cbor(response.toCborValue());
        });

    m_http.route(
        QStringLiteral("/v1/auth/refresh"), QHttpServerRequest::Method::Post,
        [this](const QHttpServerRequest &request) -> QHttpServerResponse {
            const auto map = boundedCborMap(request, m_limits.maxRequestBytes);
            if (!map)
                return QHttpServerResponse(StatusCode::BadRequest);
            const QByteArray refresh = map->value(QLatin1StringView("refresh")).toByteArray();
            const auto tokens = m_auth.refresh(refresh);
            if (!tokens.hasValue())
                return errorResponse(tokens.error());
            QCborMap response;
            response.insert(QLatin1StringView("access"), tokens.value().accessToken);
            response.insert(QLatin1StringView("refresh"), tokens.value().refreshToken);
            response.insert(QLatin1StringView("expiresAtMs"), tokens.value().accessExpiresAtMs);
            response.insert(QLatin1StringView("refreshExpiresAtMs"), tokens.value().refreshExpiresAtMs);
            return cbor(response.toCborValue());
        });

    m_http.route(
        QStringLiteral("/v1/sync"), QHttpServerRequest::Method::Get,
        [this](const QHttpServerRequest &request) -> QHttpServerResponse {
            const auto identity = m_auth.authenticate(bearerToken(request));
            if (!identity)
                return QHttpServerResponse(StatusCode::Unauthorized);
            const quint64 since =
                QUrlQuery(request.query()).queryItemValue(QStringLiteral("since")).toULongLong();
            const auto fetched =
                m_envelopes.fetchSince(identity->deviceId, since, m_limits.syncLimit);
            if (!fetched.hasValue())
                return errorResponse(fetched.error());
            // Flat [watermark, seq, envelope, seq, envelope, ...] so each
            // delivered envelope carries the per-recipient sequence the client
            // needs for a watermark-consistent durable receive.
            QCborArray array;
            array.append(static_cast<qint64>(fetched.value().newWatermark));
            for (const InboxItem &item : fetched.value().items) {
                array.append(static_cast<qint64>(item.serverSequence));
                array.append(item.envelope);
            }
            return cbor(array.toCborValue());
        });

    m_http.route(
        QStringLiteral("/v1/key-packages"), QHttpServerRequest::Method::Post,
        [this](const QHttpServerRequest &request) -> QHttpServerResponse {
            const auto identity = m_auth.authenticate(bearerToken(request));
            if (!identity)
                return QHttpServerResponse(StatusCode::Unauthorized);
            const auto map = boundedCborMap(request, m_limits.maxRequestBytes);
            if (!map)
                return QHttpServerResponse(StatusCode::BadRequest);
            const QByteArray keyPackage =
                map->value(QLatin1StringView("key_package")).toByteArray();
            const auto result =
                m_keyPackages.publish(identity->accountId, identity->deviceId, keyPackage);
            if (!result.hasValue())
                return errorResponse(result.error());
            return QHttpServerResponse(StatusCode::Ok);
        });

    m_http.route(
        QStringLiteral("/v1/key-packages/claim"), QHttpServerRequest::Method::Post,
        [this](const QHttpServerRequest &request) -> QHttpServerResponse {
            const auto identity = m_auth.authenticate(bearerToken(request));
            if (!identity)
                return QHttpServerResponse(StatusCode::Unauthorized);
            const auto map = boundedCborMap(request, m_limits.maxRequestBytes);
            if (!map)
                return QHttpServerResponse(StatusCode::BadRequest);
            const auto target = idField<DeviceId>(*map, QLatin1StringView("target_device_id"));
            if (!target)
                return QHttpServerResponse(StatusCode::BadRequest);
            const auto claimed = m_keyPackages.claim(*target, identity->deviceId);
            if (!claimed.hasValue())
                return errorResponse(claimed.error());
            QCborMap response;
            response.insert(QLatin1StringView("key_package"), claimed.value());
            return cbor(response.toCborValue());
        });

    // Discovery. All three endpoints require a valid bearer token: authentication
    // is the primary anti-enumeration control (anonymous lookups are refused), and
    // resolveHandle is exact-match only, so there is no listing surface to abuse.
    // Per-account request-rate limiting is enforced upstream at the reverse proxy
    // and tracked for the hardening phase, matching the auth endpoints' stance.
    m_http.route(
        QStringLiteral("/v1/directory"), QHttpServerRequest::Method::Get,
        [this](const QHttpServerRequest &request) -> QHttpServerResponse {
            const auto identity = m_auth.authenticate(bearerToken(request));
            if (!identity)
                return QHttpServerResponse(StatusCode::Unauthorized);
            const QString handle =
                QUrlQuery(request.query()).queryItemValue(QStringLiteral("handle"));
            if (handle.isEmpty())
                return QHttpServerResponse(StatusCode::BadRequest);
            const auto resolved = m_directory.resolveHandle(handle);
            if (!resolved.hasValue())
                return errorResponse(resolved.error());
            return cbor(directoryCbor(resolved.value()));
        });

    m_http.route(
        QStringLiteral("/v1/invites"), QHttpServerRequest::Method::Post,
        [this](const QHttpServerRequest &request) -> QHttpServerResponse {
            const auto identity = m_auth.authenticate(bearerToken(request));
            if (!identity)
                return QHttpServerResponse(StatusCode::Unauthorized);
            // An empty body is allowed; ttl_ms is optional and defaults to policy.
            const auto map = boundedCborMap(request, m_limits.maxRequestBytes);
            const qint64 requestedTtl =
                map ? static_cast<qint64>(map->value(QLatin1StringView("ttl_ms")).toInteger(0)) : 0;
            const qint64 ttl =
                requestedTtl > 0 ? requestedTtl : m_directory.defaultInviteTtlMs();
            const auto token = m_directory.createInvite(identity->accountId, ttl);
            if (!token.hasValue())
                return errorResponse(token.error());
            QCborMap response;
            response.insert(QLatin1StringView("token"), token.value());
            // Advisory expiry for the client; the stored expires_at_ms is what
            // redemption enforces. Same event-loop tick as the insert.
            response.insert(QLatin1StringView("expires_at_ms"), m_store.nowMs() + ttl);
            return cbor(response.toCborValue());
        });

    m_http.route(
        QStringLiteral("/v1/invites/redeem"), QHttpServerRequest::Method::Post,
        [this](const QHttpServerRequest &request) -> QHttpServerResponse {
            const auto identity = m_auth.authenticate(bearerToken(request));
            if (!identity)
                return QHttpServerResponse(StatusCode::Unauthorized);
            const auto map = boundedCborMap(request, m_limits.maxRequestBytes);
            if (!map)
                return QHttpServerResponse(StatusCode::BadRequest);
            const QByteArray token = map->value(QLatin1StringView("token")).toByteArray();
            const auto redeemed = m_directory.redeemInvite(token);
            if (!redeemed.hasValue())
                return errorResponse(redeemed.error());
            return cbor(directoryCbor(redeemed.value()));
        });

    m_http.setMissingHandler(this, [](const QHttpServerRequest &, QHttpServerResponder &responder) {
        responder.sendResponse(QHttpServerResponse(StatusCode::NotFound));
    });
}

void RelayServer::onWebSocketConnection()
{
    while (m_http.hasPendingWebSocketConnections()) {
        std::unique_ptr<QWebSocket> owned = m_http.nextPendingWebSocketConnection();
        if (!owned)
            continue;

        const auto identity = m_auth.authenticate(bearerToken(owned->request()));
        if (!identity) {
            owned->close(QWebSocketProtocol::CloseCodePolicyViolated,
                         QStringLiteral("unauthenticated"));
            continue; // unique_ptr drops the socket after the close handshake
        }

        // Hand ownership to Qt so the socket is cleaned up via deleteLater and
        // never deleted from inside its own disconnected() signal.
        QWebSocket *raw = owned.release();
        raw->setParent(this);
        raw->setMaxAllowedIncomingFrameSize(m_limits.maxFrameBytes);
        raw->setMaxAllowedIncomingMessageSize(m_limits.maxFrameBytes);
        const QByteArray key = identity->deviceId.bytes().toHex();
        m_liveByDevice.insert(key, raw);

        connect(raw, &QWebSocket::binaryMessageReceived, this,
                [this, raw, id = *identity](const QByteArray &message) {
                    handleLiveBinary(raw, id, message);
                });
        connect(raw, &QWebSocket::textMessageReceived, raw, [raw](const QString &) {
            raw->close(QWebSocketProtocol::CloseCodeDatatypeNotSupported,
                       QStringLiteral("binary only"));
        });
        connect(raw, &QWebSocket::disconnected, this, [this, raw, key]() {
            if (m_liveByDevice.value(key) == raw)
                m_liveByDevice.remove(key);
            raw->deleteLater();
        });
    }
}

void RelayServer::handleLiveBinary(QWebSocket *socket, const AuthenticatedDevice &device,
                                   const QByteArray &message)
{
    if (static_cast<quint64>(message.size()) > m_limits.maxFrameBytes) {
        socket->close(QWebSocketProtocol::CloseCodeTooMuchData, QStringLiteral("frame too large"));
        return;
    }

    const int major = cborMajorType(message);
    if (major == 5) {
        // A canonical envelope submitted by the sender.
        const auto submitted = m_envelopes.submit(device, message);
        const auto decoded = decodeEnvelope(message);
        if (!submitted.hasValue() || !decoded.hasValue())
            return; // silently drop invalid submissions; no plaintext callback
        QCborArray ack;
        ack.append(1); // RelayAccepted
        ack.append(decoded.value().envelopeId.bytes());
        ack.append(static_cast<qint64>(submitted.value().serverSequence));
        socket->sendBinaryMessage(ack.toCborValue().toCbor());

        // Best-effort real-time delivery to a connected recipient, carrying the
        // recipient's inbox sequence: [4 (Delivery), seq, envelope].
        const QByteArray recipientKey = decoded.value().recipientDeviceId.bytes().toHex();
        if (QWebSocket *recipient = m_liveByDevice.value(recipientKey)) {
            QCborArray delivery;
            delivery.append(4);
            delivery.append(static_cast<qint64>(submitted.value().serverSequence));
            delivery.append(message);
            recipient->sendBinaryMessage(delivery.toCborValue().toCbor());
        }
        return;
    }
    if (major == 4) {
        QCborParserError parseError{};
        const QCborValue value = QCborValue::fromCbor(message, &parseError);
        if (parseError.error != QCborError::NoError || !value.isArray())
            return;
        const QCborArray control = value.toArray();
        if (control.size() == 3 && control.at(0).toInteger() == 3) {
            // Acknowledge: [3, envelopeId, watermark]
            const quint64 watermark = static_cast<quint64>(control.at(2).toInteger());
            (void)m_envelopes.acknowledge(device.deviceId, watermark);
        }
        return;
    }
    // Anything else is an invalid control frame; drop it.
}

} // namespace OpenChat::Relay
