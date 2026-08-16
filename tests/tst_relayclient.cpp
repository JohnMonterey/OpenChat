#include "relay/RelayTestSupport.h"

#include "network/BackoffPolicy.h"
#include "network/RelayClient.h"
#include "protocol/CanonicalCborCodec.h"
#include "protocol/CiphertextEnvelope.h"
#include "security/DeviceIdentity.h"

#include "RelayCrypto.h"

#include <QCborArray>
#include <QCborMap>
#include <QCborValue>
#include <QCryptographicHash>
#include <QSignalSpy>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslSocket>
#include <QUrlQuery>
#include <QWebSocket>
#include <QtTest/QtTest>

#include <chrono>
#include <optional>

using namespace OpenChat;
using namespace std::chrono;

// RelaySession has default-constructible members, so it is a valid metatype.
// CiphertextEnvelopeV1 and StrongId are intentionally not default-constructible,
// so those signal payloads are captured through lambdas instead of QSignalSpy.
Q_DECLARE_METATYPE(OpenChat::RelayTransportError)
Q_DECLARE_METATYPE(OpenChat::RelaySession)

namespace {

CiphertextEnvelopeV1 makeEnvelope(const DeviceId &recipient, const QByteArray &ciphertext = "hello")
{
    const QByteArray hash = QCryptographicHash::hash(ciphertext, QCryptographicHash::Sha256);
    return CiphertextEnvelopeV1{
        1,
        EnvelopeId::generate(),
        AccountId::generate(),
        DeviceId::generate(),
        recipient,
        ConversationId::generate(),
        EnvelopeMessageKind::MlsPrivateMessage,
        1'700'000'000'000,
        1'700'000'060'000,
        EnvelopeId::generate(),
        ciphertext,
        hash,
        QByteArray(64, '\x01')};
}

// A backoff whose next delay is far beyond any test window, so a negative test
// never sees a spurious reconnect attempt.
BackoffPolicy slowBackoff()
{
    BackoffPolicy::Config config;
    config.base = minutes{10};
    config.cap = minutes{10};
    return BackoffPolicy(config, [] { return 0.999; });
}

RelayCredentials fixedCredentials(const QByteArray &access, const QByteArray &refresh)
{
    RelayCredentials credentials;
    credentials.accessToken = [access] { return access; };
    credentials.refreshToken = [refresh] { return refresh; };
    return credentials;
}

QByteArray sessionCbor(const QByteArray &access, const QByteArray &refresh, qint64 expiresAtMs)
{
    QCborMap map;
    map.insert(QLatin1StringView("access"), access);
    map.insert(QLatin1StringView("refresh"), refresh);
    map.insert(QLatin1StringView("expiresAtMs"), expiresAtMs);
    return map.toCborValue().toCbor();
}

QByteArray catchUpCbor(quint64 watermark, const QList<CiphertextEnvelopeV1> &envelopes)
{
    // Flat [watermark, seq, envelope, seq, envelope, ...].
    QCborArray array;
    array.append(static_cast<qint64>(watermark));
    qint64 sequence = 1;
    for (const CiphertextEnvelopeV1 &envelope : envelopes) {
        array.append(sequence++);
        array.append(encodeCanonical(envelope));
    }
    return array.toCborValue().toCbor();
}

RelayEndpoints wssOnly(const QUrl &live)
{
    RelayEndpoints endpoints;
    endpoints.live = live;
    return endpoints;
}

// Bundles the signal observation needed by the transport tests. Envelope and
// EnvelopeId payloads are captured through lambdas because their types are not
// default-constructible (a deliberate domain invariant).
struct Observer final {
    explicit Observer(RelayClient *client)
        : errors(client, &RelayClient::transportError)
        , connects(client, &RelayClient::connected)
    {
        QObject::connect(client, &RelayClient::envelopeReceived, client,
                         [this](const CiphertextEnvelopeV1 &envelope, quint64 serverSequence) {
                             ++envelopeCount;
                             lastEnvelope = envelope;
                             lastSequence = serverSequence;
                         });
        QObject::connect(client, &RelayClient::relayAccepted, client,
                         [this](const EnvelopeId &id, quint64 sequence) {
                             acceptedIds.append(id.bytes());
                             acceptedSequences.append(sequence);
                         });
    }

    [[nodiscard]] RelayTransportError firstError() const
    {
        return errors.first().at(0).value<RelayTransportError>();
    }

    QSignalSpy errors;
    QSignalSpy connects;
    int envelopeCount = 0;
    std::optional<CiphertextEnvelopeV1> lastEnvelope;
    quint64 lastSequence = 0;
    QList<QByteArray> acceptedIds;
    QList<quint64> acceptedSequences;
};

// Builds a server -> client Delivery control frame: [4, seq, canonicalEnvelope].
QByteArray deliveryFrame(const CiphertextEnvelopeV1 &envelope, quint64 serverSequence)
{
    QCborArray frame;
    frame.append(4);
    frame.append(static_cast<qint64>(serverSequence));
    frame.append(encodeCanonical(envelope));
    return frame.toCborValue().toCbor();
}

} // namespace

class RelayClientTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void hostnameMismatchIsTls();
    void untrustedCaIsTls();
    void expiredCertificateIsTls();
    void keyCertMismatchIsTls();

    void missingSubprotocolIsRejected();
    void textFrameIsRejected();
    void oversizeFrameIsRejected();
    void malformedCborIsRejected();
    void wrongRecipientIsRejected();

    void validCiphertextIsDelivered();
    void connectLiveResumesFromWatermark();
    void sendEnvelopeIsAcknowledged();
    void sendEnvelopeBeforeConnectFails();

    void authRequestBodiesMatchRelay();
    void emptyContextFailsClosed();

    void serializedRefreshSucceedsOnce();
    void refreshExhaustionExpiresAuth();
    void refreshCycleResetsAfterSuccess();
    void httpsTlsFailureIsTls();
    void insecureEndpointsAreRejected();

    void backoffRespectsFullJitterBounds();
    void backoffSaturatesAtCap();
};

void RelayClientTest::initTestCase()
{
    qRegisterMetaType<RelayTransportError>();
    qRegisterMetaType<RelaySession>();
    QVERIFY2(QSslSocket::supportsSsl(), "TLS backend must be available for these tests");
}

// --- TLS verification -------------------------------------------------------

void RelayClientTest::hostnameMismatchIsTls()
{
    RelayTest::CertAuthority ca;
    RelayTest::FakeWssServer server(RelayTest::serverConfig(ca.wrongHostLeaf()),
                                    {QString::fromLatin1(relaySubprotocol)});
    QVERIFY(server.isListening());

    RelayClient client(DeviceId::generate(), AccountId::generate(), wssOnly(server.liveUrl("localhost")),
                       fixedCredentials("a", "r"), RelayLimits{}, slowBackoff());
    client.setTlsConfiguration(RelayTest::clientConfigTrusting(ca.caCertPem()));
    Observer observer(&client);

    client.connectLive(0);
    QTRY_VERIFY(observer.errors.count() >= 1);
    QCOMPARE(observer.firstError(), RelayTransportError::Tls);
    QCOMPARE(observer.connects.count(), 0);
    QCOMPARE(observer.envelopeCount, 0);
    QVERIFY(!client.isConnected());
}

void RelayClientTest::untrustedCaIsTls()
{
    RelayTest::CertAuthority serverCa;
    RelayTest::CertAuthority clientCa;
    RelayTest::FakeWssServer server(RelayTest::serverConfig(serverCa.localhostLeaf()),
                                    {QString::fromLatin1(relaySubprotocol)});
    QVERIFY(server.isListening());

    RelayClient client(DeviceId::generate(), AccountId::generate(), wssOnly(server.liveUrl("localhost")),
                       fixedCredentials("a", "r"), RelayLimits{}, slowBackoff());
    client.setTlsConfiguration(RelayTest::clientConfigTrusting(clientCa.caCertPem()));
    Observer observer(&client);

    client.connectLive(0);
    QTRY_VERIFY(observer.errors.count() >= 1);
    QCOMPARE(observer.firstError(), RelayTransportError::Tls);
    QCOMPARE(observer.connects.count(), 0);
    QCOMPARE(observer.envelopeCount, 0);
}

void RelayClientTest::expiredCertificateIsTls()
{
    RelayTest::CertAuthority ca;
    RelayTest::FakeWssServer server(RelayTest::serverConfig(ca.expiredLeaf()),
                                    {QString::fromLatin1(relaySubprotocol)});
    QVERIFY(server.isListening());

    RelayClient client(DeviceId::generate(), AccountId::generate(), wssOnly(server.liveUrl("localhost")),
                       fixedCredentials("a", "r"), RelayLimits{}, slowBackoff());
    client.setTlsConfiguration(RelayTest::clientConfigTrusting(ca.caCertPem()));
    Observer observer(&client);

    client.connectLive(0);
    QTRY_VERIFY(observer.errors.count() >= 1);
    QCOMPARE(observer.firstError(), RelayTransportError::Tls);
    QCOMPARE(observer.connects.count(), 0);
    QCOMPARE(observer.envelopeCount, 0);
}

void RelayClientTest::keyCertMismatchIsTls()
{
    // A server presenting a certificate whose private key does not match cannot
    // complete the handshake. Depending on when the peer aborts, the client sees
    // either an SSL handshake failure (Tls) or a dropped connection (Network) —
    // both are terminal and must fail closed without connecting or delivering an
    // envelope. The security-relevant invariant is fail-closed, not the subtype.
    RelayTest::CertAuthority ca;
    const auto certLeaf = ca.localhostLeaf();
    const auto otherLeaf = ca.localhostLeaf();
    QSslConfiguration serverCfg;
    serverCfg.setLocalCertificate(QSslCertificate::fromData(certLeaf.certPem, QSsl::Pem).first());
    serverCfg.setPrivateKey(QSslKey(otherLeaf.keyPem, QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey));
    serverCfg.setPeerVerifyMode(QSslSocket::VerifyNone);

    RelayTest::FakeWssServer server(serverCfg, {QString::fromLatin1(relaySubprotocol)});
    QVERIFY(server.isListening());

    RelayClient client(DeviceId::generate(), AccountId::generate(), wssOnly(server.liveUrl("localhost")),
                       fixedCredentials("a", "r"), RelayLimits{}, slowBackoff());
    client.setTlsConfiguration(RelayTest::clientConfigTrusting(ca.caCertPem()));
    Observer observer(&client);

    client.connectLive(0);
    QTRY_VERIFY(observer.errors.count() >= 1);
    const RelayTransportError error = observer.firstError();
    QVERIFY2(error == RelayTransportError::Tls || error == RelayTransportError::Network,
             "broken server TLS material must fail closed");
    QCOMPARE(observer.connects.count(), 0);
    QCOMPARE(observer.envelopeCount, 0);
}

// --- WebSocket protocol / frame hostility -----------------------------------

void RelayClientTest::missingSubprotocolIsRejected()
{
    RelayTest::CertAuthority ca;
    RelayTest::FakeWssServer server(RelayTest::serverConfig(ca.localhostLeaf()), {});
    QVERIFY(server.isListening());

    RelayClient client(DeviceId::generate(), AccountId::generate(), wssOnly(server.liveUrl("localhost")),
                       fixedCredentials("a", "r"), RelayLimits{}, slowBackoff());
    client.setTlsConfiguration(RelayTest::clientConfigTrusting(ca.caCertPem()));
    Observer observer(&client);

    client.connectLive(0);
    QTRY_VERIFY(observer.errors.count() >= 1);
    QCOMPARE(observer.firstError(), RelayTransportError::ProtocolMismatch);
    QCOMPARE(observer.connects.count(), 0);
    QCOMPARE(observer.envelopeCount, 0);
    QVERIFY(!client.isConnected());
}

void RelayClientTest::textFrameIsRejected()
{
    RelayTest::CertAuthority ca;
    RelayTest::FakeWssServer server(RelayTest::serverConfig(ca.localhostLeaf()),
                                    {QString::fromLatin1(relaySubprotocol)});
    server.onConnected = [](QWebSocket *socket) { socket->sendTextMessage(QStringLiteral("hi")); };
    QVERIFY(server.isListening());

    RelayClient client(DeviceId::generate(), AccountId::generate(), wssOnly(server.liveUrl("localhost")),
                       fixedCredentials("a", "r"), RelayLimits{}, slowBackoff());
    client.setTlsConfiguration(RelayTest::clientConfigTrusting(ca.caCertPem()));
    Observer observer(&client);

    client.connectLive(0);
    QTRY_VERIFY(observer.errors.count() >= 1);
    QCOMPARE(observer.firstError(), RelayTransportError::TextFrameRejected);
    QCOMPARE(observer.envelopeCount, 0);
}

void RelayClientTest::oversizeFrameIsRejected()
{
    RelayTest::CertAuthority ca;
    RelayTest::FakeWssServer server(RelayTest::serverConfig(ca.localhostLeaf()),
                                    {QString::fromLatin1(relaySubprotocol)});
    server.onConnected = [](QWebSocket *socket) {
        socket->sendBinaryMessage(QByteArray(8192, '\0'));
    };
    QVERIFY(server.isListening());

    RelayLimits limits;
    limits.maxIncomingFrameBytes = 4096;
    limits.maxIncomingMessageBytes = 4096;

    RelayClient client(DeviceId::generate(), AccountId::generate(), wssOnly(server.liveUrl("localhost")),
                       fixedCredentials("a", "r"), limits, slowBackoff());
    client.setTlsConfiguration(RelayTest::clientConfigTrusting(ca.caCertPem()));
    Observer observer(&client);

    client.connectLive(0);
    QTRY_VERIFY(observer.errors.count() >= 1);
    QCOMPARE(observer.firstError(), RelayTransportError::FrameTooLarge);
    QCOMPARE(observer.envelopeCount, 0);
}

void RelayClientTest::malformedCborIsRejected()
{
    RelayTest::CertAuthority ca;
    RelayTest::FakeWssServer server(RelayTest::serverConfig(ca.localhostLeaf()),
                                    {QString::fromLatin1(relaySubprotocol)});
    server.onConnected = [](QWebSocket *socket) {
        // A well-formed Delivery frame carrying a payload that is not a valid
        // envelope: the bounded decoder rejects it as malformed.
        QCborArray frame;
        frame.append(4);
        frame.append(qint64(1));
        frame.append(QByteArray("not-a-canonical-envelope"));
        socket->sendBinaryMessage(frame.toCborValue().toCbor());
    };
    QVERIFY(server.isListening());

    RelayClient client(DeviceId::generate(), AccountId::generate(), wssOnly(server.liveUrl("localhost")),
                       fixedCredentials("a", "r"), RelayLimits{}, slowBackoff());
    client.setTlsConfiguration(RelayTest::clientConfigTrusting(ca.caCertPem()));
    Observer observer(&client);

    client.connectLive(0);
    QTRY_VERIFY(observer.errors.count() >= 1);
    QCOMPARE(observer.firstError(), RelayTransportError::MalformedEnvelope);
    QCOMPARE(observer.envelopeCount, 0);
}

void RelayClientTest::wrongRecipientIsRejected()
{
    RelayTest::CertAuthority ca;
    RelayTest::FakeWssServer server(RelayTest::serverConfig(ca.localhostLeaf()),
                                    {QString::fromLatin1(relaySubprotocol)});
    const DeviceId someoneElse = DeviceId::generate();
    server.onConnected = [someoneElse](QWebSocket *socket) {
        socket->sendBinaryMessage(deliveryFrame(makeEnvelope(someoneElse), 7));
    };
    QVERIFY(server.isListening());

    RelayClient client(DeviceId::generate(), AccountId::generate(), wssOnly(server.liveUrl("localhost")),
                       fixedCredentials("a", "r"), RelayLimits{}, slowBackoff());
    client.setTlsConfiguration(RelayTest::clientConfigTrusting(ca.caCertPem()));
    Observer observer(&client);

    client.connectLive(0);
    QTRY_VERIFY(observer.errors.count() >= 1);
    QCOMPARE(observer.firstError(), RelayTransportError::WrongRecipient);
    QCOMPARE(observer.envelopeCount, 0);
}

// --- Happy paths ------------------------------------------------------------

void RelayClientTest::validCiphertextIsDelivered()
{
    RelayTest::CertAuthority ca;
    RelayTest::FakeWssServer server(RelayTest::serverConfig(ca.localhostLeaf()),
                                    {QString::fromLatin1(relaySubprotocol)});
    const DeviceId localDevice = DeviceId::generate();
    const CiphertextEnvelopeV1 envelope = makeEnvelope(localDevice, "ciphertext-payload");
    server.onConnected = [envelope](QWebSocket *socket) {
        socket->sendBinaryMessage(deliveryFrame(envelope, 42));
    };
    QVERIFY(server.isListening());

    RelayClient client(localDevice, AccountId::generate(), wssOnly(server.liveUrl("localhost")),
                       fixedCredentials("access", "refresh"), RelayLimits{}, slowBackoff());
    client.setTlsConfiguration(RelayTest::clientConfigTrusting(ca.caCertPem()));
    Observer observer(&client);

    client.connectLive(0);
    QTRY_COMPARE(observer.connects.count(), 1);
    QTRY_COMPARE(observer.envelopeCount, 1);
    QCOMPARE(observer.errors.count(), 0);
    QVERIFY(client.isConnected());
    QVERIFY(observer.lastEnvelope.has_value());
    QCOMPARE(*observer.lastEnvelope, envelope);
    QCOMPARE(observer.lastSequence, quint64(42));
}

void RelayClientTest::connectLiveResumesFromWatermark()
{
    RelayTest::CertAuthority ca;
    RelayTest::FakeWssServer server(RelayTest::serverConfig(ca.localhostLeaf()),
                                    {QString::fromLatin1(relaySubprotocol)});
    QVERIFY(server.isListening());

    RelayClient client(DeviceId::generate(), AccountId::generate(), wssOnly(server.liveUrl("localhost")),
                       fixedCredentials("access", "refresh"), RelayLimits{}, slowBackoff());
    client.setTlsConfiguration(RelayTest::clientConfigTrusting(ca.caCertPem()));
    Observer observer(&client);

    client.connectLive(0xABCD);
    QTRY_COMPARE(observer.connects.count(), 1);

    const QUrlQuery query(server.observedResumeUrl());
    QCOMPARE(query.queryItemValue(QStringLiteral("since")), QString::number(0xABCD));
}

void RelayClientTest::sendEnvelopeIsAcknowledged()
{
    RelayTest::CertAuthority ca;
    RelayTest::FakeWssServer server(RelayTest::serverConfig(ca.localhostLeaf()),
                                    {QString::fromLatin1(relaySubprotocol)});
    server.onConnected = [](QWebSocket *socket) {
        QObject::connect(socket, &QWebSocket::binaryMessageReceived, socket,
                         [socket](const QByteArray &message) {
                             const auto decoded = decodeEnvelope(message);
                             if (!decoded.hasValue())
                                 return;
                             QCborArray ack;
                             ack.append(1); // RelayAccepted
                             ack.append(decoded.value().envelopeId.bytes());
                             ack.append(static_cast<qint64>(77));
                             socket->sendBinaryMessage(ack.toCborValue().toCbor());
                         });
    };
    QVERIFY(server.isListening());

    const DeviceId localDevice = DeviceId::generate();
    RelayClient client(localDevice, AccountId::generate(), wssOnly(server.liveUrl("localhost")),
                       fixedCredentials("access", "refresh"), RelayLimits{}, slowBackoff());
    client.setTlsConfiguration(RelayTest::clientConfigTrusting(ca.caCertPem()));
    Observer observer(&client);

    client.connectLive(0);
    QTRY_COMPARE(observer.connects.count(), 1);

    const CiphertextEnvelopeV1 envelope = makeEnvelope(DeviceId::generate());
    const auto result = client.sendEnvelope(envelope);
    QVERIFY(result.hasValue());

    QTRY_COMPARE(observer.acceptedIds.count(), 1);
    QCOMPARE(observer.acceptedIds.first(), envelope.envelopeId.bytes());
    QCOMPARE(observer.acceptedSequences.first(), quint64(77));
}

void RelayClientTest::sendEnvelopeBeforeConnectFails()
{
    RelayClient client(DeviceId::generate(), AccountId::generate(),
                       wssOnly(QUrl(QStringLiteral("wss://localhost:1/live"))),
                       fixedCredentials("access", "refresh"), RelayLimits{}, slowBackoff());

    const auto result = client.sendEnvelope(makeEnvelope(DeviceId::generate()));
    QVERIFY(!result.hasValue());
    QCOMPARE(result.error(), RelayCallError::NotConnected);
}

// --- Device authentication request composition ------------------------------

void RelayClientTest::authRequestBodiesMatchRelay()
{
    RelayTest::CertAuthority ca;
    RelayTest::FakeHttpsServer server(RelayTest::serverConfig(ca.localhostLeaf()));
    QVERIFY(server.isListening());

    // A real device identity: the client signs the challenge with it, and the
    // relay's own verifier must accept the exact bytes the client transmits.
    auto identityResult = DeviceIdentity::generate();
    QVERIFY(identityResult.hasValue());
    const DeviceIdentity identity = std::move(identityResult).value();
    const DevicePublicCredential credential = identity.publicCredential();
    const DeviceId localDevice = credential.deviceId;
    const AccountId localAccount = AccountId::generate();

    // Canned relay responses: a 32-byte challenge, then a token pair.
    const QByteArray challenge(32, '\x5a');
    QCborMap challengeResponse;
    challengeResponse.insert(QLatin1StringView("challenge"), challenge);
    server.enqueue(QStringLiteral("/auth/challenge"),
                   {200, challengeResponse.toCborValue().toCbor(), -1});
    server.enqueue(QStringLiteral("/auth/complete"),
                   {200, sessionCbor("access-token", "refresh-token", 1'700'000'999'000), -1});

    RelayEndpoints endpoints;
    endpoints.authChallenge = server.url(QStringLiteral("/auth/challenge"));
    endpoints.authComplete = server.url(QStringLiteral("/auth/complete"));
    endpoints.authRefresh = server.url(QStringLiteral("/auth/refresh"));
    endpoints.sync = server.url(QStringLiteral("/sync"));
    endpoints.live = QUrl(QStringLiteral("wss://localhost:1/live"));

    RelayClient client(localDevice, localAccount, endpoints,
                       fixedCredentials(QByteArray(), QByteArray()), RelayLimits{}, slowBackoff());
    client.setTlsConfiguration(RelayTest::clientConfigTrusting(ca.caCertPem()));

    QSignalSpy authenticated(&client, &RelayClient::authenticated);
    QSignalSpy errors(&client, &RelayClient::transportError);
    QSignalSpy expired(&client, &RelayClient::authExpired);

    // The signer is the real device signer over the bound (challenge, context).
    const ChallengeSigner signer = [&identity](QByteArrayView ch, QByteArrayView ctx) -> QByteArray {
        auto sig = identity.signChallenge(ch, ctx);
        return sig.hasValue() ? sig.value() : QByteArray{};
    };
    const QByteArray context("account-device-bind");

    client.authenticateDevice(QByteArray("mls-credential"), signer, context);

    QTRY_COMPARE(authenticated.count(), 1);
    QCOMPARE(errors.count(), 0);
    QCOMPARE(expired.count(), 0);
    QCOMPARE(server.requestCount(QStringLiteral("/auth/challenge")), 1);
    QCOMPARE(server.requestCount(QStringLiteral("/auth/complete")), 1);

    // --- Challenge body: account_id + device_id byte fields, no credential. ---
    QCborParserError challengeErr{};
    const QCborValue challengeValue =
        QCborValue::fromCbor(server.lastBody(QStringLiteral("/auth/challenge")), &challengeErr);
    QCOMPARE(challengeErr.error, QCborError::NoError);
    QVERIFY(challengeValue.isMap());
    const QCborMap challengeMap = challengeValue.toMap();
    QVERIFY(challengeMap.value(QLatin1StringView("account_id")).isByteArray());
    QCOMPARE(challengeMap.value(QLatin1StringView("account_id")).toByteArray(),
             localAccount.bytes());
    QVERIFY(challengeMap.value(QLatin1StringView("device_id")).isByteArray());
    QCOMPARE(challengeMap.value(QLatin1StringView("device_id")).toByteArray(), localDevice.bytes());
    QVERIFY(challengeMap.value(QLatin1StringView("credential")).isUndefined());

    // --- Complete body: account_id, device_id, challenge, signature, non-empty
    //     context; no credential. ---
    QCborParserError completeErr{};
    const QCborValue completeValue =
        QCborValue::fromCbor(server.lastBody(QStringLiteral("/auth/complete")), &completeErr);
    QCOMPARE(completeErr.error, QCborError::NoError);
    QVERIFY(completeValue.isMap());
    const QCborMap completeMap = completeValue.toMap();
    QCOMPARE(completeMap.value(QLatin1StringView("account_id")).toByteArray(), localAccount.bytes());
    QCOMPARE(completeMap.value(QLatin1StringView("device_id")).toByteArray(), localDevice.bytes());
    const QByteArray sentChallenge = completeMap.value(QLatin1StringView("challenge")).toByteArray();
    const QByteArray sentSignature = completeMap.value(QLatin1StringView("signature")).toByteArray();
    const QByteArray sentContext = completeMap.value(QLatin1StringView("context")).toByteArray();
    QVERIFY(completeMap.value(QLatin1StringView("credential")).isUndefined());
    QCOMPARE(sentChallenge, challenge);
    QCOMPARE(sentContext, context);
    QVERIFY(!sentContext.isEmpty());
    QCOMPARE(sentSignature.size(), qsizetype(64));

    // --- Cross-verify with the relay's own verifier over the relay's own
    //     signing-message reconstruction: this proves the exact bytes the relay
    //     will check are what the client signed. ---
    const QByteArray message = Relay::challengeSigningMessage(sentChallenge, sentContext);
    QVERIFY(Relay::verifyEd25519(credential.signingPublicKey, message, sentSignature));

    // The authenticated session carries the relay's tokens.
    const RelaySession session = authenticated.first().at(0).value<RelaySession>();
    QCOMPARE(session.accessToken, QByteArray("access-token"));
    QCOMPARE(session.refreshToken, QByteArray("refresh-token"));
}

void RelayClientTest::emptyContextFailsClosed()
{
    RelayTest::CertAuthority ca;
    RelayTest::FakeHttpsServer server(RelayTest::serverConfig(ca.localhostLeaf()));
    QVERIFY(server.isListening());

    auto identityResult = DeviceIdentity::generate();
    QVERIFY(identityResult.hasValue());
    const DeviceIdentity identity = std::move(identityResult).value();

    RelayEndpoints endpoints;
    endpoints.authChallenge = server.url(QStringLiteral("/auth/challenge"));
    endpoints.authComplete = server.url(QStringLiteral("/auth/complete"));
    endpoints.live = QUrl(QStringLiteral("wss://localhost:1/live"));

    RelayClient client(identity.publicCredential().deviceId, AccountId::generate(), endpoints,
                       fixedCredentials(QByteArray(), QByteArray()), RelayLimits{}, slowBackoff());
    client.setTlsConfiguration(RelayTest::clientConfigTrusting(ca.caCertPem()));

    QSignalSpy expired(&client, &RelayClient::authExpired);
    QSignalSpy errors(&client, &RelayClient::transportError);

    bool signerCalled = false;
    const ChallengeSigner signer = [&](QByteArrayView ch, QByteArrayView ctx) -> QByteArray {
        signerCalled = true;
        auto sig = identity.signChallenge(ch, ctx);
        return sig.hasValue() ? sig.value() : QByteArray{};
    };

    // An empty context must fail closed: no request issued, signer never invoked.
    client.authenticateDevice(QByteArray("mls-credential"), signer, QByteArray());

    QCOMPARE(expired.count(), 1); // emitted synchronously by the guard
    // Give any (erroneously) dispatched request time to reach the server, then
    // prove none was ever made.
    QTest::qWait(100);
    QCOMPARE(server.requestCount(QStringLiteral("/auth/challenge")), 0);
    QCOMPARE(server.requestCount(QStringLiteral("/auth/complete")), 0);
    QCOMPARE(errors.count(), 0);
    QVERIFY(!signerCalled);
}

// --- HTTPS auth / refresh ---------------------------------------------------

void RelayClientTest::serializedRefreshSucceedsOnce()
{
    RelayTest::CertAuthority ca;
    RelayTest::FakeHttpsServer server(RelayTest::serverConfig(ca.localhostLeaf()));
    QVERIFY(server.isListening());

    const DeviceId localDevice = DeviceId::generate();
    const CiphertextEnvelopeV1 envelope = makeEnvelope(localDevice);

    server.enqueue(QStringLiteral("/sync"), {401, {}, -1});
    server.enqueue(QStringLiteral("/auth/refresh"),
                   {200, sessionCbor("access2", "refresh2", 1'700'000'999'000), -1});
    server.enqueue(QStringLiteral("/sync"), {200, catchUpCbor(9, {envelope}), -1});

    int refreshCalls = 0;
    RelayCredentials credentials;
    credentials.accessToken = [] { return QByteArray("access1"); };
    credentials.refreshToken = [&refreshCalls] {
        ++refreshCalls;
        return QByteArray("refresh1");
    };

    RelayEndpoints endpoints;
    endpoints.sync = server.url(QStringLiteral("/sync"));
    endpoints.authRefresh = server.url(QStringLiteral("/auth/refresh"));
    endpoints.authChallenge = server.url(QStringLiteral("/auth/challenge"));
    endpoints.authComplete = server.url(QStringLiteral("/auth/complete"));
    endpoints.live = QUrl(QStringLiteral("wss://localhost:1/live"));

    RelayClient client(localDevice, AccountId::generate(), endpoints, credentials, RelayLimits{}, slowBackoff());
    client.setTlsConfiguration(RelayTest::clientConfigTrusting(ca.caCertPem()));

    QSignalSpy rotated(&client, &RelayClient::tokensRotated);
    QSignalSpy expired(&client, &RelayClient::authExpired);
    QSignalSpy complete(&client, &RelayClient::catchUpComplete);
    Observer observer(&client);

    client.fetchSince(3);

    QTRY_COMPARE(complete.count(), 1);
    QCOMPARE(refreshCalls, 1);
    QCOMPARE(rotated.count(), 1);
    QCOMPARE(expired.count(), 0);
    QCOMPARE(observer.envelopeCount, 1);
    QCOMPARE(complete.first().at(0).value<quint64>(), quint64(9));
    QCOMPARE(server.requestCount(QStringLiteral("/auth/refresh")), 1);
    QCOMPARE(server.requestCount(QStringLiteral("/sync")), 2);
    QCOMPARE(rotated.first().at(0).value<RelaySession>().accessToken, QByteArray("access2"));
}

void RelayClientTest::refreshExhaustionExpiresAuth()
{
    RelayTest::CertAuthority ca;
    RelayTest::FakeHttpsServer server(RelayTest::serverConfig(ca.localhostLeaf()));
    QVERIFY(server.isListening());

    server.enqueue(QStringLiteral("/sync"), {401, {}, -1});
    server.enqueue(QStringLiteral("/auth/refresh"),
                   {200, sessionCbor("access2", "refresh2", 1'700'000'999'000), -1});
    server.enqueue(QStringLiteral("/sync"), {401, {}, -1});

    int refreshCalls = 0;
    RelayCredentials credentials;
    credentials.accessToken = [] { return QByteArray("access1"); };
    credentials.refreshToken = [&refreshCalls] {
        ++refreshCalls;
        return QByteArray("refresh1");
    };

    RelayEndpoints endpoints;
    endpoints.sync = server.url(QStringLiteral("/sync"));
    endpoints.authRefresh = server.url(QStringLiteral("/auth/refresh"));
    endpoints.live = QUrl(QStringLiteral("wss://localhost:1/live"));

    RelayClient client(DeviceId::generate(), AccountId::generate(), endpoints, credentials, RelayLimits{}, slowBackoff());
    client.setTlsConfiguration(RelayTest::clientConfigTrusting(ca.caCertPem()));

    QSignalSpy rotated(&client, &RelayClient::tokensRotated);
    QSignalSpy expired(&client, &RelayClient::authExpired);

    client.fetchSince(3);

    QTRY_COMPARE(expired.count(), 1);
    QCOMPARE(refreshCalls, 1);    // exactly one refresh, never a second
    QCOMPARE(rotated.count(), 1); // the single refresh itself succeeded
    QCOMPARE(server.requestCount(QStringLiteral("/auth/refresh")), 1);
}

void RelayClientTest::refreshCycleResetsAfterSuccess()
{
    // A successful authorized round-trip ends the refresh cycle, so a genuinely
    // new 401 later triggers a second refresh (the cycle does not wedge).
    RelayTest::CertAuthority ca;
    RelayTest::FakeHttpsServer server(RelayTest::serverConfig(ca.localhostLeaf()));
    QVERIFY(server.isListening());

    const DeviceId localDevice = DeviceId::generate();
    server.enqueue(QStringLiteral("/sync"), {401, {}, -1});
    server.enqueue(QStringLiteral("/auth/refresh"),
                   {200, sessionCbor("access2", "refresh2", 1'700'000'999'000), -1});
    server.enqueue(QStringLiteral("/sync"), {200, catchUpCbor(11, {}), -1});
    server.enqueue(QStringLiteral("/sync"), {401, {}, -1});
    server.enqueue(QStringLiteral("/auth/refresh"),
                   {200, sessionCbor("access3", "refresh3", 1'700'001'999'000), -1});
    server.enqueue(QStringLiteral("/sync"), {200, catchUpCbor(22, {}), -1});

    int refreshCalls = 0;
    RelayCredentials credentials;
    credentials.accessToken = [] { return QByteArray("access1"); };
    credentials.refreshToken = [&refreshCalls] {
        ++refreshCalls;
        return QByteArray("refresh1");
    };

    RelayEndpoints endpoints;
    endpoints.sync = server.url(QStringLiteral("/sync"));
    endpoints.authRefresh = server.url(QStringLiteral("/auth/refresh"));
    endpoints.live = QUrl(QStringLiteral("wss://localhost:1/live"));

    RelayClient client(localDevice, AccountId::generate(), endpoints, credentials, RelayLimits{}, slowBackoff());
    client.setTlsConfiguration(RelayTest::clientConfigTrusting(ca.caCertPem()));

    QSignalSpy rotated(&client, &RelayClient::tokensRotated);
    QSignalSpy complete(&client, &RelayClient::catchUpComplete);

    client.fetchSince(3);
    QTRY_COMPARE(complete.count(), 1);
    client.fetchSince(11);
    QTRY_COMPARE(complete.count(), 2);

    QCOMPARE(refreshCalls, 2);
    QCOMPARE(rotated.count(), 2);
    QCOMPARE(server.requestCount(QStringLiteral("/auth/refresh")), 2);
}

void RelayClientTest::httpsTlsFailureIsTls()
{
    // A TLS verification failure on the HTTPS path surfaces as the terminal Tls
    // diagnostic, not a generic HTTP status.
    RelayTest::CertAuthority serverCa;
    RelayTest::CertAuthority clientCa; // client trusts a different CA only
    RelayTest::FakeHttpsServer server(RelayTest::serverConfig(serverCa.localhostLeaf()));
    server.enqueue(QStringLiteral("/sync"), {200, catchUpCbor(1, {}), -1});
    QVERIFY(server.isListening());

    RelayEndpoints endpoints;
    endpoints.sync = server.url(QStringLiteral("/sync"));
    endpoints.live = QUrl(QStringLiteral("wss://localhost:1/live"));

    RelayClient client(DeviceId::generate(), AccountId::generate(), endpoints, fixedCredentials("a", "r"),
                       RelayLimits{}, slowBackoff());
    client.setTlsConfiguration(RelayTest::clientConfigTrusting(clientCa.caCertPem()));

    QSignalSpy errors(&client, &RelayClient::transportError);
    QSignalSpy complete(&client, &RelayClient::catchUpComplete);

    client.fetchSince(0);
    QTRY_VERIFY(errors.count() >= 1);
    QCOMPARE(errors.first().at(0).value<RelayTransportError>(), RelayTransportError::Tls);
    QCOMPARE(complete.count(), 0);
}

void RelayClientTest::insecureEndpointsAreRejected()
{
    RelayEndpoints secure;
    secure.authChallenge = QUrl(QStringLiteral("https://relay.example/auth/challenge"));
    secure.authComplete = QUrl(QStringLiteral("https://relay.example/auth/complete"));
    secure.authRefresh = QUrl(QStringLiteral("https://relay.example/auth/refresh"));
    secure.sync = QUrl(QStringLiteral("https://relay.example/sync"));
    secure.live = QUrl(QStringLiteral("wss://relay.example/live"));
    QVERIFY(secure.isSecure());

    RelayEndpoints wsDowngrade = secure;
    wsDowngrade.live = QUrl(QStringLiteral("ws://relay.example/live"));
    QVERIFY(!wsDowngrade.isSecure());

    RelayEndpoints httpDowngrade = secure;
    httpDowngrade.sync = QUrl(QStringLiteral("http://relay.example/sync"));
    QVERIFY(!httpDowngrade.isSecure());

    RelayEndpoints httpSync;
    httpSync.sync = QUrl(QStringLiteral("http://relay.example/sync"));
    RelayClient client(DeviceId::generate(), AccountId::generate(), httpSync, fixedCredentials("a", "r"));
    QSignalSpy errors(&client, &RelayClient::transportError);
    client.fetchSince(0);
    QTRY_COMPARE(errors.count(), 1);
    QCOMPARE(errors.first().at(0).value<RelayTransportError>(),
             RelayTransportError::InsecureEndpoint);
}

// --- Backoff ----------------------------------------------------------------

void RelayClientTest::backoffRespectsFullJitterBounds()
{
    BackoffPolicy::Config config;
    config.base = seconds{1};
    config.cap = minutes{5};
    config.multiplier = 2.0;

    for (double fraction : {0.0, 0.5, 0.999}) {
        BackoffPolicy policy(config, [fraction] { return fraction; });
        for (int attempt = 0; attempt <= 25; ++attempt) {
            const auto ceiling = policy.ceilingForAttempt(attempt);
            const auto delay = policy.delayForAttempt(attempt);
            QVERIFY(delay.count() >= 0);
            QVERIFY(delay <= ceiling);
            QVERIFY(ceiling <= minutes{5});
        }
    }

    BackoffPolicy zero(config, [] { return 0.0; });
    QCOMPARE(zero.delayForAttempt(5).count(), 0);
    BackoffPolicy high(config, [] { return 0.999999; });
    QVERIFY(high.delayForAttempt(3) <= high.ceilingForAttempt(3));
    QVERIFY(high.delayForAttempt(3).count() > 0);
}

void RelayClientTest::backoffSaturatesAtCap()
{
    BackoffPolicy::Config config;
    config.base = seconds{1};
    config.cap = minutes{5};
    config.multiplier = 2.0;
    BackoffPolicy policy(config, [] { return 0.999999; });

    QCOMPARE(policy.ceilingForAttempt(40), duration_cast<milliseconds>(minutes{5}));
    QCOMPARE(policy.ceilingForAttempt(1000), duration_cast<milliseconds>(minutes{5}));
    QCOMPARE(policy.ceilingForAttempt(0), duration_cast<milliseconds>(seconds{1}));
}

QTEST_GUILESS_MAIN(RelayClientTest)
#include "tst_relayclient.moc"
