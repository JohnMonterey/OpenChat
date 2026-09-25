#include "RelayServer.h"
#include "UdpMediaService.h"

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
#include <QTimer>

#include <algorithm>
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

// { claim_id, reward_id, seed, case_key }
QCborMap claimCbor(const CosmeticClaim &claim)
{
    QCborMap map;
    map.insert(QLatin1StringView("claim_id"), claim.claimId);
    map.insert(QLatin1StringView("reward_id"), claim.rewardId);
    map.insert(QLatin1StringView("seed"), static_cast<qint64>(claim.seed));
    map.insert(QLatin1StringView("case_key"), claim.caseKey);
    return map;
}

QCborMap slotsCbor(const QHash<QString, QString> &loadout)
{
    QCborMap map;
    for (auto it = loadout.cbegin(); it != loadout.cend(); ++it)
        map.insert(it.key(), it.value());
    return map;
}

// An account's own cosmetics: { drops, progress_ms, interval_ms, next_case_key,
// owned: [id...], loadout: { slot: id }, imported, last?: claim }.
QCborMap cosmeticStateCbor(const CosmeticState &state)
{
    QCborMap map;
    map.insert(QLatin1StringView("drops"), state.drops);
    map.insert(QLatin1StringView("progress_ms"), state.progressMs);
    map.insert(QLatin1StringView("interval_ms"), state.dropIntervalMs);
    map.insert(QLatin1StringView("next_case_key"), state.nextCaseKey);
    map.insert(QLatin1StringView("owned"), QCborArray::fromStringList(state.owned));
    map.insert(QLatin1StringView("loadout"), slotsCbor(state.loadout));
    map.insert(QLatin1StringView("imported"), state.imported);
    if (state.last)
        map.insert(QLatin1StringView("last"), claimCbor(*state.last));
    return map;
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

RelayServer::~RelayServer()
{
    // Shutting down: every connected account keeps the time it was here.
    if (m_cosmetics) {
        const auto sessions = m_cosmeticSessions.keys();
        for (const QByteArray &key : sessions) {
            if (const auto account = AccountId::fromBytes(QByteArray::fromHex(key)))
                creditCosmeticSession(*account, false);
        }
    }
}

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

    if (!m_udpMedia) {
        const QString mediaBind = qEnvironmentVariable("OPENCHAT_RELAY_MEDIA_BIND");
        const int mediaPort = qEnvironmentVariableIntValue("OPENCHAT_RELAY_MEDIA_PORT");
        if (!mediaBind.isEmpty() || mediaPort > 0) {
            (void)startUdpMedia(QHostAddress(!mediaBind.isEmpty() ? mediaBind : QStringLiteral("127.0.0.1")),
                                static_cast<quint16>(mediaPort > 0 ? mediaPort : 8444));
        }
    }

    return m_tcp->serverPort();
}

quint16 RelayServer::startUdpMedia(const QHostAddress &address, quint16 port)
{
    m_udpMedia = std::make_unique<UdpMediaService>(this);
    return m_udpMedia->start(address, port);
}

void RelayServer::registerTestToken(const QByteArray &token, const AuthenticatedDevice &device)
{
    m_testTokens.insert(token, device);
}

void RelayServer::registerRoutes()
{
    registerCosmeticRoutes();

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
            // The locally stretched password key; never the password itself. A
            // request without one (a pre-password client) is refused below.
            const QByteArray passwordKey =
                map->value(QLatin1StringView("password_key")).toByteArray();
            const int passwordKdf =
                static_cast<int>(map->value(QLatin1StringView("password_kdf")).toInteger(0));
            if (!account || !device || handle.isEmpty())
                return QHttpServerResponse(StatusCode::BadRequest);
            const auto result = m_auth.registerAccount(*account, handle, *device, signingKey,
                                                       credential, passwordKey, passwordKdf);
            if (!result.hasValue())
                return errorResponse(result.error());
            return QHttpServerResponse(StatusCode::Ok);
        });

    // Password login: enrolls the caller's device into an existing account. Like
    // registration it is unauthenticated by bearer token -- the password key is
    // the credential -- and it only ever yields the account id; the caller still
    // has to pass the signed device challenge to obtain tokens.
    m_http.route(
        QStringLiteral("/v1/auth/login"), QHttpServerRequest::Method::Post,
        [this](const QHttpServerRequest &request) -> QHttpServerResponse {
            const auto map = boundedCborMap(request, m_limits.maxRequestBytes);
            if (!map)
                return QHttpServerResponse(StatusCode::BadRequest);
            const auto device = idField<DeviceId>(*map, QLatin1StringView("device_id"));
            const QString handle = map->value(QLatin1StringView("handle")).toString();
            const QByteArray passwordKey =
                map->value(QLatin1StringView("password_key")).toByteArray();
            const int passwordKdf =
                static_cast<int>(map->value(QLatin1StringView("password_kdf")).toInteger(0));
            const QByteArray signingKey =
                map->value(QLatin1StringView("signing_key")).toByteArray();
            const QByteArray credential =
                map->value(QLatin1StringView("credential")).toByteArray();
            if (!device || handle.isEmpty())
                return QHttpServerResponse(StatusCode::BadRequest);
            const auto login = m_auth.loginWithPassword(handle, passwordKey, passwordKdf, *device,
                                                        signingKey, credential);
            if (!login.hasValue())
                return errorResponse(login.error());
            // The account now lives on the caller's device: drop the live stream
            // of every device the login retired so it stops receiving at once.
            for (const DeviceId &retired : login.value().retiredDevices) {
                if (QWebSocket *socket = m_liveByDevice.value(retired.bytes().toHex()))
                    socket->close(QWebSocketProtocol::CloseCodePolicyViolated,
                                  QStringLiteral("signed in elsewhere"));
            }
            QCborMap response;
            response.insert(QLatin1StringView("account_id"), login.value().accountId.bytes());
            response.insert(QLatin1StringView("handle"), login.value().handle);
            return cbor(response.toCborValue());
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
            response.insert(QLatin1StringView("availableKeyPackages"), m_keyPackages.availableCount(*device));
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
        QStringLiteral("/v1/key-packages"), QHttpServerRequest::Method::Get,
        [this](const QHttpServerRequest &request) -> QHttpServerResponse {
            const auto identity = m_auth.authenticate(bearerToken(request));
            if (!identity)
                return QHttpServerResponse(StatusCode::Unauthorized);
            const int count = m_keyPackages.availableCount(identity->deviceId);
            if (count < 0)
                return QHttpServerResponse(StatusCode::InternalServerError);
            QCborMap response;
            response.insert(QLatin1StringView("availableKeyPackages"), count);
            return cbor(response.toCborValue());
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
            QCborMap response;
            response.insert(QLatin1StringView("availableKeyPackages"),
                            m_keyPackages.availableCount(identity->deviceId));
            return cbor(response.toCborValue());
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
            // Even an empty-pool claim wakes an online owner so existing
            // exhausted accounts can recover. Old clients never get new frames.
            sendKeyPackageSupply(*target);
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
        QStringLiteral("/v1/directory/account"), QHttpServerRequest::Method::Get,
        [this](const QHttpServerRequest &request) -> QHttpServerResponse {
            const auto identity = m_auth.authenticate(bearerToken(request));
            if (!identity)
                return QHttpServerResponse(StatusCode::Unauthorized);
            const QString hex =
                QUrlQuery(request.query()).queryItemValue(QStringLiteral("account_id"));
            const auto account = AccountId::fromBytes(QByteArray::fromHex(hex.toLatin1()));
            if (hex.size() != AccountId::byteCount * 2 || !account)
                return QHttpServerResponse(StatusCode::BadRequest);
            const auto resolved = m_directory.resolveAccount(*account);
            if (!resolved.hasValue())
                return errorResponse(resolved.error());
            QCborMap response;
            response.insert(QLatin1StringView("handle"), resolved.value());
            return cbor(response.toCborValue());
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

        auto identity = m_auth.authenticate(bearerToken(owned->request()));
        if (!identity) {
            const auto it = m_testTokens.find(bearerToken(owned->request()));
            if (it != m_testTokens.end())
                identity = *it;
        }
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
        if (auto *previous = m_liveByDevice.value(key))
            previous->abort();
        m_liveByDevice.insert(key, raw);
        raw->setProperty("keyPackageSupply", QUrlQuery(raw->requestUrl())
            .queryItemValue(QStringLiteral("keyPackageSupply")) == QStringLiteral("1"));
        sendKeyPackageSupply(identity->deviceId);
        // Opted-in clients hear their cosmetics on connect and whenever they
        // change; every connection counts toward the account's case drops.
        raw->setProperty("cosmetics", QUrlQuery(raw->requestUrl())
            .queryItemValue(QStringLiteral("cosmetics")) == QStringLiteral("1"));
        beginCosmeticSession(identity->accountId, raw);
        // Long envelopes go in small frames: the client counts every frame
        // as a sign of life, so a long catch-up on a slow downlink never
        // looks like a dead link.
        raw->setOutgoingFrameSize(16 * 1024);
        // Detect vanished clients even when TCP has not reported a disconnect.
        auto *heartbeat = new QTimer(raw);
        raw->setProperty("awaitingPong", false);
        connect(raw, &QWebSocket::pong, raw, [raw] { raw->setProperty("awaitingPong", false); });
        // Any frame from the client proves it is there, not only a pong: a
        // client catching up on a long backlog over a slow downlink acks every
        // envelope as it goes, while its pong may wait behind that backlog.
        connect(raw, &QWebSocket::binaryFrameReceived, raw,
                [raw](const QByteArray &, bool) { raw->setProperty("awaitingPong", false); });
        connect(heartbeat, &QTimer::timeout, raw, [raw] {
            if (raw->property("awaitingPong").toBool()) {
                raw->abort();
                return;
            }
            raw->setProperty("awaitingPong", true);
            raw->ping();
        });
        heartbeat->start(10'000);

        // Replay the unacknowledged inbox before handling newer live traffic.
        // A client may resume from zero safely because received rows are pruned.
        quint64 cursor = QUrlQuery(raw->requestUrl()).queryItemValue(QStringLiteral("since")).toULongLong();
        while (true) {
            const auto page = m_envelopes.fetchSince(identity->deviceId, cursor, m_limits.syncLimit);
            if (!page.hasValue() || page.value().items.isEmpty())
                break;
            for (const auto &item : page.value().items) {
                QCborArray delivery;
                delivery.append(4);
                delivery.append(static_cast<qint64>(item.serverSequence));
                delivery.append(item.envelope);
                raw->sendBinaryMessage(delivery.toCborValue().toCbor());
            }
            if (page.value().newWatermark <= cursor)
                break;
            cursor = page.value().newWatermark;
        }

        connect(raw, &QWebSocket::binaryMessageReceived, this,
                [this, raw, id = *identity](const QByteArray &message) {
                    handleLiveBinary(raw, id, message);
                });
        connect(raw, &QWebSocket::textMessageReceived, raw, [raw](const QString &) {
            raw->close(QWebSocketProtocol::CloseCodeDatatypeNotSupported,
                       QStringLiteral("binary only"));
        });
        connect(raw, &QWebSocket::disconnected, this,
                [this, raw, key, devId = identity->deviceId, account = identity->accountId]() {
            if (m_liveByDevice.value(key) == raw)
                m_liveByDevice.remove(key);
            endCosmeticSession(account, raw);
            if (m_udpMedia)
                m_udpMedia->clearBinding(devId);
            raw->deleteLater();
        });
    }
}

void RelayServer::registerCosmeticRoutes()
{
    // Collectible cosmetics (migration 007). Every route needs a signed-in
    // device; an account only ever reads or changes its own state, except for
    // loadouts, which any signed-in account may read, like the directory.
    const auto authenticated = [this](const QHttpServerRequest &request) {
        return m_cosmetics ? m_auth.authenticate(bearerToken(request)) : std::nullopt;
    };

    m_http.route(
        QStringLiteral("/v1/cosmetics"), QHttpServerRequest::Method::Get,
        [this, authenticated](const QHttpServerRequest &request) -> QHttpServerResponse {
            if (!m_cosmetics)
                return QHttpServerResponse(StatusCode::NotFound);
            const auto identity = authenticated(request);
            if (!identity)
                return QHttpServerResponse(StatusCode::Unauthorized);
            const auto state = m_cosmetics->state(identity->accountId);
            if (!state.hasValue())
                return errorResponse(state.error());
            return cbor(cosmeticStateCbor(state.value()).toCborValue());
        });

    m_http.route(
        QStringLiteral("/v1/cosmetics/claim"), QHttpServerRequest::Method::Post,
        [this, authenticated](const QHttpServerRequest &request) -> QHttpServerResponse {
            if (!m_cosmetics)
                return QHttpServerResponse(StatusCode::NotFound);
            const auto identity = authenticated(request);
            if (!identity)
                return QHttpServerResponse(StatusCode::Unauthorized);
            const auto map = boundedCborMap(request, m_limits.maxRequestBytes);
            if (!map || !map->value(QLatin1StringView("request_id")).isByteArray())
                return QHttpServerResponse(StatusCode::BadRequest);
            const auto claimed = m_cosmetics->claim(
                identity->accountId, map->value(QLatin1StringView("request_id")).toByteArray());
            if (!claimed.hasValue())
                return errorResponse(claimed.error());
            pushCosmetics(identity->accountId, claimed.value().state);
            QCborMap response;
            response.insert(QLatin1StringView("state"), cosmeticStateCbor(claimed.value().state));
            response.insert(QLatin1StringView("claim"), claimCbor(claimed.value().claim));
            response.insert(QLatin1StringView("newly_claimed"), claimed.value().newlyClaimed);
            return cbor(response.toCborValue());
        });

    m_http.route(
        QStringLiteral("/v1/cosmetics/equip"), QHttpServerRequest::Method::Post,
        [this, authenticated](const QHttpServerRequest &request) -> QHttpServerResponse {
            if (!m_cosmetics)
                return QHttpServerResponse(StatusCode::NotFound);
            const auto identity = authenticated(request);
            if (!identity)
                return QHttpServerResponse(StatusCode::Unauthorized);
            const auto map = boundedCborMap(request, m_limits.maxRequestBytes);
            if (!map || !map->value(QLatin1StringView("slot")).isString()
                || !map->value(QLatin1StringView("item_id")).isString())
                return QHttpServerResponse(StatusCode::BadRequest);
            const auto state = m_cosmetics->equip(identity->accountId,
                                                  map->value(QLatin1StringView("slot")).toString(),
                                                  map->value(QLatin1StringView("item_id")).toString());
            if (!state.hasValue())
                return errorResponse(state.error());
            pushCosmetics(identity->accountId, state.value());
            return cbor(cosmeticStateCbor(state.value()).toCborValue());
        });

    m_http.route(
        QStringLiteral("/v1/cosmetics/import"), QHttpServerRequest::Method::Post,
        [this, authenticated](const QHttpServerRequest &request) -> QHttpServerResponse {
            if (!m_cosmetics)
                return QHttpServerResponse(StatusCode::NotFound);
            const auto identity = authenticated(request);
            if (!identity)
                return QHttpServerResponse(StatusCode::Unauthorized);
            const auto map = boundedCborMap(request, m_limits.maxRequestBytes);
            if (!map || !map->value(QLatin1StringView("owned")).isArray()
                || !map->value(QLatin1StringView("drops")).isInteger())
                return QHttpServerResponse(StatusCode::BadRequest);
            QStringList owned;
            for (const QCborValue &id : map->value(QLatin1StringView("owned")).toArray()) {
                if (!id.isString())
                    return QHttpServerResponse(StatusCode::BadRequest);
                owned.append(id.toString());
            }
            const qint64 drops = map->value(QLatin1StringView("drops")).toInteger();
            const auto state = m_cosmetics->importCollection(
                identity->accountId, owned, static_cast<int>(std::clamp<qint64>(drops, -1, 1'000'000)));
            if (!state.hasValue())
                return errorResponse(state.error());
            pushCosmetics(identity->accountId, state.value());
            return cbor(cosmeticStateCbor(state.value()).toCborValue());
        });

    m_http.route(
        QStringLiteral("/v1/cosmetics/loadouts"), QHttpServerRequest::Method::Post,
        [this, authenticated](const QHttpServerRequest &request) -> QHttpServerResponse {
            if (!m_cosmetics)
                return QHttpServerResponse(StatusCode::NotFound);
            const auto identity = authenticated(request);
            if (!identity)
                return QHttpServerResponse(StatusCode::Unauthorized);
            const auto map = boundedCborMap(request, m_limits.maxRequestBytes);
            if (!map || !map->value(QLatin1StringView("accounts")).isArray())
                return QHttpServerResponse(StatusCode::BadRequest);
            QList<AccountId> accounts;
            for (const QCborValue &value : map->value(QLatin1StringView("accounts")).toArray()) {
                const auto account = value.isByteArray() ? AccountId::fromBytes(value.toByteArray())
                                                         : std::nullopt;
                if (!account)
                    return QHttpServerResponse(StatusCode::BadRequest);
                accounts.append(*account);
            }
            const auto worn = m_cosmetics->loadouts(accounts);
            if (!worn.hasValue())
                return errorResponse(worn.error());
            // { loadouts: [ { account_id, slots: { slot: id } }, ... ] }
            QCborArray loadouts;
            for (auto it = worn.value().cbegin(); it != worn.value().cend(); ++it) {
                QCborMap entry;
                entry.insert(QLatin1StringView("account_id"), it.key().bytes());
                entry.insert(QLatin1StringView("slots"), slotsCbor(it.value()));
                loadouts.append(entry);
            }
            QCborMap response;
            response.insert(QLatin1StringView("loadouts"), loadouts);
            return cbor(response.toCborValue());
        });
}

void RelayServer::beginCosmeticSession(const AccountId &account, QWebSocket *socket)
{
    if (!m_cosmetics)
        return;
    const QByteArray key = account.bytes().toHex();
    CosmeticSession &session = m_cosmeticSessions[key];
    session.sockets.append(socket);
    if (session.sockets.size() == 1) {
        // The account just came online: its connected time counts from now.
        session.uncredited.start();
        session.nextDrop = new QTimer(this);
        session.nextDrop->setSingleShot(true);
        // A coarse timer may fire a little early, short of the drop it waits for.
        session.nextDrop->setTimerType(Qt::PreciseTimer);
        connect(session.nextDrop, &QTimer::timeout, this,
                [this, account] { creditCosmeticSession(account, true); });
        creditCosmeticSession(account, false);
    }
    // Tell the new connection where it stands.
    if (socket->property("cosmetics").toBool()) {
        if (const auto state = m_cosmetics->state(account); state.hasValue())
            socket->sendBinaryMessage(
                QCborArray{14, cosmeticStateCbor(state.value())}.toCborValue().toCbor());
    }
}

void RelayServer::endCosmeticSession(const AccountId &account, QWebSocket *socket)
{
    const QByteArray key = account.bytes().toHex();
    auto it = m_cosmeticSessions.find(key);
    if (it == m_cosmeticSessions.end())
        return;
    it->sockets.removeAll(socket);
    if (!it->sockets.isEmpty())
        return;
    // The last connection closed: credit the time it ran, and stop counting.
    creditCosmeticSession(account, false);
    it = m_cosmeticSessions.find(key);
    if (it == m_cosmeticSessions.end())
        return;
    if (it->nextDrop)
        it->nextDrop->deleteLater();
    m_cosmeticSessions.erase(it);
}

void RelayServer::creditCosmeticSession(const AccountId &account, bool push)
{
    if (!m_cosmetics)
        return;
    auto it = m_cosmeticSessions.find(account.bytes().toHex());
    if (it == m_cosmeticSessions.end())
        return;
    const auto state = m_cosmetics->accrue(account, it->uncredited.elapsed());
    if (!state.hasValue()) {
        // The time keeps counting and is offered again a minute on.
        if (it->nextDrop)
            it->nextDrop->start(60'000);
        qWarning("relay: could not credit an account's connected time toward its cases");
        return;
    }
    it->uncredited.restart();
    if (it->nextDrop) {
        const qint64 remaining = state.value().dropIntervalMs - state.value().progressMs;
        it->nextDrop->start(static_cast<int>(std::clamp<qint64>(remaining, 1, state.value().dropIntervalMs)));
    }
    const bool dropped = state.value().drops != it->drops;
    it->drops = state.value().drops;
    if (push && dropped)
        pushCosmetics(account, state.value());
}

void RelayServer::pushCosmetics(const AccountId &account, const CosmeticState &state)
{
    const auto it = m_cosmeticSessions.constFind(account.bytes().toHex());
    if (it == m_cosmeticSessions.cend())
        return;
    const QByteArray frame = QCborArray{14, cosmeticStateCbor(state)}.toCborValue().toCbor();
    for (QWebSocket *socket : it->sockets) {
        if (socket->property("cosmetics").toBool())
            socket->sendBinaryMessage(frame);
    }
}

void RelayServer::sendKeyPackageSupply(const DeviceId &device)
{
    auto *socket = m_liveByDevice.value(device.bytes().toHex());
    if (!socket || !socket->property("keyPackageSupply").toBool())
        return;
    const int count = m_keyPackages.availableCount(device);
    if (count >= 0)
        socket->sendBinaryMessage(QCborArray{10, count}.toCborValue().toCbor());
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
        const auto decoded = decodeEnvelope(message);
        if (!decoded.hasValue())
            return;
        // Stored whether or not the recipient is connected: an offline device
        // receives it from its inbox replay when it next connects.
        const auto submitted = m_envelopes.submit(device, message);
        if (!submitted.hasValue()) {
            // A device that does not exist or was retired (a login elsewhere)
            // will never take it: [9 (RecipientUnavailable), envelopeId], so
            // the sender stops retrying at once. Anything else goes
            // unanswered and is retried.
            if (submitted.error() == RelayError::NotFound || submitted.error() == RelayError::Revoked) {
                QCborArray refused;
                refused.append(9);
                refused.append(decoded.value().envelopeId.bytes());
                socket->sendBinaryMessage(refused.toCborValue().toCbor());
            }
            return;
        }
        QCborArray ack;
        ack.append(1); // RelayAccepted
        ack.append(decoded.value().envelopeId.bytes());
        ack.append(static_cast<qint64>(submitted.value().serverSequence));
        socket->sendBinaryMessage(ack.toCborValue().toCbor());

        // Real-time delivery to a connected recipient, carrying the recipient's
        // inbox sequence: [4 (Delivery), seq, envelope]. Best-effort only; the
        // stored row is the delivery guarantee.
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
        if (control.size() == 2 && control.at(0).toInteger() == 7 && control.at(1).isArray()) {
            const auto ids = control.at(1).toArray();
            if (ids.size() > 256)
                return;
            QCborArray rows;
            for (const auto &value : ids) {
                if (!value.isByteArray())
                    return;
                const auto id = DeviceId::fromBytes(value.toByteArray());
                if (!id)
                    return;
                QCborArray row;
                row.append(id->bytes());
                const auto *peer = m_liveByDevice.value(id->bytes().toHex());
                row.append(peer && peer->state() == QAbstractSocket::ConnectedState);
                rows.append(row);
            }
            QCborArray result;
            result.append(8);
            result.append(rows);
            socket->sendBinaryMessage(result.toCborValue().toCbor());
            return;
        }
        if (control.size() == 3 && control.at(0).toInteger() == 3) {
            // Acknowledge: [3, envelopeId, watermark]
            const quint64 watermark = static_cast<quint64>(control.at(2).toInteger());
            (void)m_envelopes.acknowledge(device.deviceId, watermark);
            return;
        }
        if (control.size() == 2 && control.at(0).toInteger() == 5 && control.at(1).isByteArray()) {
            // Datagram submit: [5, envelope]. Fully authenticated like a durable
            // submission — same decode, same sender check, same signature check —
            // but never written to an inbox and never sequenced or acknowledged.
            // A recipient that is not connected right now simply misses it; that
            // is the point of the path, and it is why real-time media can use it
            // without turning every call into thousands of stored rows.
            const QByteArray envelopeBytes = control.at(1).toByteArray();
            const auto validated = m_envelopes.validate(device, envelopeBytes);
            if (!validated.hasValue())
                return;
            const QByteArray recipientKey = validated.value().recipientDeviceId.bytes().toHex();
            QWebSocket *recipient = m_liveByDevice.value(recipientKey);
            if (recipient == nullptr)
                return;
            QCborArray delivery;
            delivery.append(6); // DatagramDelivery
            delivery.append(envelopeBytes);
            recipient->sendBinaryMessage(delivery.toCborValue().toCbor());
            return;
        }
        if (control.size() == 1 && control.at(0).toInteger() == 12) {
            // MediaTokenRequest: [12] -> mint token -> [13, token]
            if (m_udpMedia) {
                const QByteArray token = m_udpMedia->mintToken(device.deviceId);
                QCborArray reply;
                reply.append(13); // MediaToken
                reply.append(token);
                socket->sendBinaryMessage(reply.toCborValue().toCbor());
            }
            return;
        }
        return;
    }
    // Anything else is an invalid control frame; drop it.
}

} // namespace OpenChat::Relay
