#include "AuthService.h"
#include "DirectoryService.h"
#include "EnvelopeService.h"
#include "KeyPackageService.h"
#include "PostgresStore.h"
#include "RelayServer.h"
#include "UdpMediaService.h"
#include "protocol/UdpMediaFrame.h"

#include <QCborArray>
#include <QCborValue>
#include <QNetworkDatagram>
#include <QNetworkRequest>
#include <QSignalSpy>
#include <QUdpSocket>
#include <QWebSocket>
#include <QtTest/QtTest>

using namespace OpenChat;
using namespace OpenChat::Relay;

class UdpMediaServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void helloOkAndErr();
    void singleUseToken();
    void bothWayMediaForwarding();
    void unboundAndWrongSourceDrops();
    void expiryAndSilenceTimeout();
    void rateLimit();
    void pongRoutingAndBye();
    void webSocketDisconnectClearsBinding();
};

namespace {

struct TestContext {
    std::unique_ptr<PostgresStore> store;
    std::unique_ptr<AuthService> auth;
    std::unique_ptr<EnvelopeService> envelopes;
    std::unique_ptr<KeyPackageService> keyPackages;
    std::unique_ptr<DirectoryService> directory;
    std::unique_ptr<RelayServer> server;
    quint16 wsPort = 0;
    quint16 udpPort = 0;

    static std::unique_ptr<TestContext> create()
    {
        auto ctx = std::make_unique<TestContext>();
        ctx->store = PostgresStore::createNull(QStringLiteral("tst_udp_null_store"));
        ctx->auth = std::make_unique<AuthService>(*ctx->store);
        ctx->envelopes = std::make_unique<EnvelopeService>(*ctx->store);
        ctx->keyPackages = std::make_unique<KeyPackageService>(*ctx->store);
        ctx->directory = std::make_unique<DirectoryService>(*ctx->store);
        ctx->server = std::make_unique<RelayServer>(
            *ctx->store, *ctx->auth, *ctx->envelopes, *ctx->keyPackages, *ctx->directory);

        ctx->wsPort = ctx->server->start(QHostAddress::LocalHost, 0);
        ctx->udpPort = ctx->server->startUdpMedia(QHostAddress::LocalHost, 0);
        return ctx;
    }

    QNetworkRequest makeWsRequest(const QByteArray &accessToken) const
    {
        QNetworkRequest req(QUrl(QStringLiteral("ws://127.0.0.1:%1/v1/live?since=0").arg(wsPort)));
        req.setRawHeader("Authorization", "Bearer " + accessToken);
        return req;
    }
};

QByteArray requestTokenOverWs(QWebSocket &ws, QSignalSpy &replies)
{
    QCborArray req;
    req.append(12); // MediaTokenRequest
    ws.sendBinaryMessage(req.toCborValue().toCbor());
    if (!replies.wait(2000) && replies.isEmpty())
        return {};

    const auto responseBytes = replies.takeFirst().first().toByteArray();
    QCborParserError err{};
    const auto val = QCborValue::fromCbor(responseBytes, &err);
    if (err.error != QCborError::NoError || !val.isArray())
        return {};
    const auto arr = val.toArray();
    if (arr.size() != 2 || arr.at(0).toInteger() != 13 || !arr.at(1).isByteArray())
        return {};
    return arr.at(1).toByteArray();
}

bool waitForDatagram(QUdpSocket &sock, int timeoutMs = 2000)
{
    if (sock.hasPendingDatagrams())
        return true;
    QSignalSpy spy(&sock, &QUdpSocket::readyRead);
    return spy.wait(timeoutMs) || sock.hasPendingDatagrams();
}

} // namespace

void UdpMediaServiceTest::helloOkAndErr()
{
    auto ctx = TestContext::create();
    QVERIFY(ctx->wsPort > 0);
    QVERIFY(ctx->udpPort > 0);

    const auto device = DeviceId::generate();
    const QByteArray accessToken = "token_hello_test";
    ctx->server->registerTestToken(accessToken, AuthenticatedDevice{AccountId::generate(), device});

    QWebSocket ws;
    QSignalSpy wsReplies(&ws, &QWebSocket::binaryMessageReceived);
    ws.open(ctx->makeWsRequest(accessToken));
    QTRY_COMPARE(ws.state(), QAbstractSocket::ConnectedState);

    QUdpSocket udp;
    QVERIFY(udp.bind(QHostAddress::LocalHost, 0));
    QSignalSpy udpReads(&udp, &QUdpSocket::readyRead);

    // 1. Send Hello with fake token -> receive HelloErr
    const auto badHello = UdpMediaFrame::makeHello(device, QByteArray(32, 'X'));
    udp.writeDatagram(badHello.encode(), QHostAddress::LocalHost, ctx->udpPort);
    QTRY_COMPARE(udpReads.count(), 1);
    QNetworkDatagram reply1 = udp.receiveDatagram();
    const auto frame1 = UdpMediaFrame::decode(reply1.data());
    QVERIFY(frame1.has_value());
    QCOMPARE(frame1->type, UdpMediaFrame::Type::HelloErr);
    QCOMPARE(frame1->recipientDeviceId, device.bytes());
    QVERIFY(!ctx->server->udpMediaService()->isBound(device));

    // 2. Request real token over WebSocket
    const QByteArray validToken = requestTokenOverWs(ws, wsReplies);
    QCOMPARE(validToken.size(), 32);

    // 3. Send Hello with valid token -> receive HelloOk
    udpReads.clear();
    const auto goodHello = UdpMediaFrame::makeHello(device, validToken);
    udp.writeDatagram(goodHello.encode(), QHostAddress::LocalHost, ctx->udpPort);
    QTRY_COMPARE(udpReads.count(), 1);
    QNetworkDatagram reply2 = udp.receiveDatagram();
    const auto frame2 = UdpMediaFrame::decode(reply2.data());
    QVERIFY(frame2.has_value());
    QCOMPARE(frame2->type, UdpMediaFrame::Type::HelloOk);
    QCOMPARE(frame2->recipientDeviceId, device.bytes());
    QVERIFY(ctx->server->udpMediaService()->isBound(device));
}

void UdpMediaServiceTest::singleUseToken()
{
    auto ctx = TestContext::create();
    const auto device = DeviceId::generate();
    const QByteArray accessToken = "token_single_use";
    ctx->server->registerTestToken(accessToken, AuthenticatedDevice{AccountId::generate(), device});

    QWebSocket ws;
    QSignalSpy wsReplies(&ws, &QWebSocket::binaryMessageReceived);
    ws.open(ctx->makeWsRequest(accessToken));
    QTRY_COMPARE(ws.state(), QAbstractSocket::ConnectedState);

    QUdpSocket udp;
    QVERIFY(udp.bind(QHostAddress::LocalHost, 0));
    QSignalSpy udpReads(&udp, &QUdpSocket::readyRead);

    const QByteArray token = requestTokenOverWs(ws, wsReplies);
    QCOMPARE(token.size(), 32);

    // First use succeeds
    const auto hello = UdpMediaFrame::makeHello(device, token);
    udp.writeDatagram(hello.encode(), QHostAddress::LocalHost, ctx->udpPort);
    QTRY_COMPARE(udpReads.count(), 1);
    const auto f1 = UdpMediaFrame::decode(udp.receiveDatagram().data());
    QVERIFY(f1.has_value());
    QCOMPARE(f1->type, UdpMediaFrame::Type::HelloOk);

    // Replay of same token fails with HelloErr
    udpReads.clear();
    udp.writeDatagram(hello.encode(), QHostAddress::LocalHost, ctx->udpPort);
    QTRY_COMPARE(udpReads.count(), 1);
    const auto f2 = UdpMediaFrame::decode(udp.receiveDatagram().data());
    QVERIFY(f2.has_value());
    QCOMPARE(f2->type, UdpMediaFrame::Type::HelloErr);
}

void UdpMediaServiceTest::bothWayMediaForwarding()
{
    auto ctx = TestContext::create();
    const auto dev1 = DeviceId::generate();
    const auto dev2 = DeviceId::generate();
    ctx->server->registerTestToken("tok1", AuthenticatedDevice{AccountId::generate(), dev1});
    ctx->server->registerTestToken("tok2", AuthenticatedDevice{AccountId::generate(), dev2});

    QWebSocket ws1, ws2;
    QSignalSpy replies1(&ws1, &QWebSocket::binaryMessageReceived);
    QSignalSpy replies2(&ws2, &QWebSocket::binaryMessageReceived);
    ws1.open(ctx->makeWsRequest("tok1"));
    ws2.open(ctx->makeWsRequest("tok2"));
    QTRY_COMPARE(ws1.state(), QAbstractSocket::ConnectedState);
    QTRY_COMPARE(ws2.state(), QAbstractSocket::ConnectedState);

    QUdpSocket udp1, udp2;
    QVERIFY(udp1.bind(QHostAddress::LocalHost, 0));
    QVERIFY(udp2.bind(QHostAddress::LocalHost, 0));
    QSignalSpy udpReads1(&udp1, &QUdpSocket::readyRead);
    QSignalSpy udpReads2(&udp2, &QUdpSocket::readyRead);

    // Bind both devices via Hello
    const auto t1 = requestTokenOverWs(ws1, replies1);
    const auto t2 = requestTokenOverWs(ws2, replies2);
    udp1.writeDatagram(UdpMediaFrame::makeHello(dev1, t1).encode(), QHostAddress::LocalHost, ctx->udpPort);
    udp2.writeDatagram(UdpMediaFrame::makeHello(dev2, t2).encode(), QHostAddress::LocalHost, ctx->udpPort);
    QTRY_COMPARE(udpReads1.count(), 1);
    QTRY_COMPARE(udpReads2.count(), 1);
    (void)udp1.receiveDatagram();
    (void)udp2.receiveDatagram();
    QVERIFY(ctx->server->udpMediaService()->isBound(dev1));
    QVERIFY(ctx->server->udpMediaService()->isBound(dev2));

    // Send Media 1 -> 2
    udpReads1.clear();
    udpReads2.clear();
    const QByteArray media1to2 = "encrypted_voice_packet_1_to_2";
    const auto frame1to2 = UdpMediaFrame::makeMedia(dev1, dev2, media1to2);
    udp1.writeDatagram(frame1to2.encode(), QHostAddress::LocalHost, ctx->udpPort);
    QTRY_COMPARE(udpReads2.count(), 1);
    const auto recv2 = UdpMediaFrame::decode(udp2.receiveDatagram().data());
    QVERIFY(recv2.has_value());
    QCOMPARE(recv2->type, UdpMediaFrame::Type::Media);
    QCOMPARE(recv2->senderDeviceId, dev1.bytes());
    QCOMPARE(recv2->recipientDeviceId, dev2.bytes());
    QCOMPARE(recv2->payload, media1to2);

    // Send Media 2 -> 1
    udpReads1.clear();
    udpReads2.clear();
    const QByteArray media2to1 = "encrypted_voice_packet_2_to_1";
    const auto frame2to1 = UdpMediaFrame::makeMedia(dev2, dev1, media2to1);
    udp2.writeDatagram(frame2to1.encode(), QHostAddress::LocalHost, ctx->udpPort);
    QTRY_COMPARE(udpReads1.count(), 1);
    const auto recv1 = UdpMediaFrame::decode(udp1.receiveDatagram().data());
    QVERIFY(recv1.has_value());
    QCOMPARE(recv1->type, UdpMediaFrame::Type::Media);
    QCOMPARE(recv1->senderDeviceId, dev2.bytes());
    QCOMPARE(recv1->recipientDeviceId, dev1.bytes());
    QCOMPARE(recv1->payload, media2to1);
}

void UdpMediaServiceTest::unboundAndWrongSourceDrops()
{
    auto ctx = TestContext::create();
    const auto dev1 = DeviceId::generate();
    const auto dev2 = DeviceId::generate();
    ctx->server->registerTestToken("tok1", AuthenticatedDevice{AccountId::generate(), dev1});
    ctx->server->registerTestToken("tok2", AuthenticatedDevice{AccountId::generate(), dev2});

    QWebSocket ws1, ws2;
    QSignalSpy replies1(&ws1, &QWebSocket::binaryMessageReceived);
    QSignalSpy replies2(&ws2, &QWebSocket::binaryMessageReceived);
    ws1.open(ctx->makeWsRequest("tok1"));
    ws2.open(ctx->makeWsRequest("tok2"));
    QTRY_COMPARE(ws1.state(), QAbstractSocket::ConnectedState);
    QTRY_COMPARE(ws2.state(), QAbstractSocket::ConnectedState);

    QUdpSocket udp1, udp2;
    QVERIFY(udp1.bind(QHostAddress::LocalHost, 0));
    QVERIFY(udp2.bind(QHostAddress::LocalHost, 0));
    QSignalSpy udpReads2(&udp2, &QUdpSocket::readyRead);

    // dev2 binds, but dev1 is NOT bound yet
    const auto t2 = requestTokenOverWs(ws2, replies2);
    udp2.writeDatagram(UdpMediaFrame::makeHello(dev2, t2).encode(), QHostAddress::LocalHost, ctx->udpPort);
    QVERIFY(waitForDatagram(udp2));
    (void)udp2.receiveDatagram();
    udpReads2.clear();

    // 1. dev1 sends Media while unbound -> must be dropped
    udp1.writeDatagram(UdpMediaFrame::makeMedia(dev1, dev2, "audio").encode(),
                       QHostAddress::LocalHost, ctx->udpPort);
    QTest::qWait(50);
    QCOMPARE(udpReads2.count(), 0);

    // Now dev1 binds
    const auto t1 = requestTokenOverWs(ws1, replies1);
    udp1.writeDatagram(UdpMediaFrame::makeHello(dev1, t1).encode(), QHostAddress::LocalHost, ctx->udpPort);
    QVERIFY(waitForDatagram(udp1));
    (void)udp1.receiveDatagram();

    // 2. An attacker / wrong source socket tries sending as dev1
    QUdpSocket impostor;
    QVERIFY(impostor.bind(QHostAddress::LocalHost, 0));
    udpReads2.clear();
    impostor.writeDatagram(UdpMediaFrame::makeMedia(dev1, dev2, "spoofed").encode(),
                           QHostAddress::LocalHost, ctx->udpPort);
    QTest::qWait(50);
    QCOMPARE(udpReads2.count(), 0);
}

void UdpMediaServiceTest::expiryAndSilenceTimeout()
{
    auto ctx = TestContext::create();
    qint64 fakeTime = 1'000'000;
    ctx->server->udpMediaService()->setClock([&fakeTime] { return fakeTime; });

    const auto dev1 = DeviceId::generate();
    const auto dev2 = DeviceId::generate();
    ctx->server->registerTestToken("tok1", AuthenticatedDevice{AccountId::generate(), dev1});
    ctx->server->registerTestToken("tok2", AuthenticatedDevice{AccountId::generate(), dev2});

    QWebSocket ws1, ws2;
    QSignalSpy replies1(&ws1, &QWebSocket::binaryMessageReceived);
    QSignalSpy replies2(&ws2, &QWebSocket::binaryMessageReceived);
    ws1.open(ctx->makeWsRequest("tok1"));
    ws2.open(ctx->makeWsRequest("tok2"));
    QTRY_COMPARE(ws1.state(), QAbstractSocket::ConnectedState);
    QTRY_COMPARE(ws2.state(), QAbstractSocket::ConnectedState);

    QUdpSocket udp1, udp2;
    QVERIFY(udp1.bind(QHostAddress::LocalHost, 0));
    QVERIFY(udp2.bind(QHostAddress::LocalHost, 0));
    QSignalSpy udpReads2(&udp2, &QUdpSocket::readyRead);

    // Bind both devices
    const auto t1 = requestTokenOverWs(ws1, replies1);
    const auto t2 = requestTokenOverWs(ws2, replies2);
    udp1.writeDatagram(UdpMediaFrame::makeHello(dev1, t1).encode(), QHostAddress::LocalHost, ctx->udpPort);
    udp2.writeDatagram(UdpMediaFrame::makeHello(dev2, t2).encode(), QHostAddress::LocalHost, ctx->udpPort);
    QVERIFY(waitForDatagram(udp1));
    (void)udp1.receiveDatagram();
    QVERIFY(waitForDatagram(udp2));
    (void)udp2.receiveDatagram();
    QVERIFY(ctx->server->udpMediaService()->isBound(dev1));

    // Fast forward 31 seconds (> 30s silence timeout)
    fakeTime += 31'000;
    ctx->server->udpMediaService()->prune();
    QVERIFY(!ctx->server->udpMediaService()->isBound(dev1));
    QVERIFY(!ctx->server->udpMediaService()->isBound(dev2));

    // Media now dropped
    udpReads2.clear();
    udp1.writeDatagram(UdpMediaFrame::makeMedia(dev1, dev2, "audio").encode(),
                       QHostAddress::LocalHost, ctx->udpPort);
    QTest::qWait(50);
    QCOMPARE(udpReads2.count(), 0);
}

void UdpMediaServiceTest::rateLimit()
{
    auto ctx = TestContext::create();
    const auto dev1 = DeviceId::generate();
    const auto dev2 = DeviceId::generate();
    ctx->server->registerTestToken("tok1", AuthenticatedDevice{AccountId::generate(), dev1});
    ctx->server->registerTestToken("tok2", AuthenticatedDevice{AccountId::generate(), dev2});

    QWebSocket ws1, ws2;
    QSignalSpy replies1(&ws1, &QWebSocket::binaryMessageReceived);
    QSignalSpy replies2(&ws2, &QWebSocket::binaryMessageReceived);
    ws1.open(ctx->makeWsRequest("tok1"));
    ws2.open(ctx->makeWsRequest("tok2"));
    QTRY_COMPARE(ws1.state(), QAbstractSocket::ConnectedState);
    QTRY_COMPARE(ws2.state(), QAbstractSocket::ConnectedState);

    QUdpSocket udp1, udp2;
    QVERIFY(udp1.bind(QHostAddress::LocalHost, 0));
    QVERIFY(udp2.bind(QHostAddress::LocalHost, 0));
    QSignalSpy udpReads2(&udp2, &QUdpSocket::readyRead);

    const auto t1 = requestTokenOverWs(ws1, replies1);
    const auto t2 = requestTokenOverWs(ws2, replies2);
    udp1.writeDatagram(UdpMediaFrame::makeHello(dev1, t1).encode(), QHostAddress::LocalHost, ctx->udpPort);
    udp2.writeDatagram(UdpMediaFrame::makeHello(dev2, t2).encode(), QHostAddress::LocalHost, ctx->udpPort);
    QVERIFY(waitForDatagram(udp1));
    (void)udp1.receiveDatagram();
    QVERIFY(waitForDatagram(udp2));
    (void)udp2.receiveDatagram();
    udpReads2.clear();

    // Blast 250 packets in immediate succession (capacity is 150)
    const auto frame = UdpMediaFrame::makeMedia(dev1, dev2, "burst").encode();
    for (int i = 0; i < 250; ++i) {
        udp1.writeDatagram(frame, QHostAddress::LocalHost, ctx->udpPort);
    }

    int receivedCount = 0;
    while (udpReads2.wait(100) || udp2.hasPendingDatagrams()) {
        while (udp2.hasPendingDatagrams()) {
            (void)udp2.receiveDatagram();
            ++receivedCount;
        }
    }

    // Capacity is 150 tokens, so received must be <= 160 (allowing small timing refill) and > 0, but < 250
    QVERIFY(receivedCount > 0);
    QVERIFY(receivedCount <= 160);
    QVERIFY(receivedCount < 250);
}

void UdpMediaServiceTest::pongRoutingAndBye()
{
    auto ctx = TestContext::create();
    const auto dev1 = DeviceId::generate();
    const auto dev2 = DeviceId::generate();
    ctx->server->registerTestToken("tok1", AuthenticatedDevice{AccountId::generate(), dev1});
    ctx->server->registerTestToken("tok2", AuthenticatedDevice{AccountId::generate(), dev2});

    QWebSocket ws1, ws2;
    QSignalSpy replies1(&ws1, &QWebSocket::binaryMessageReceived);
    QSignalSpy replies2(&ws2, &QWebSocket::binaryMessageReceived);
    ws1.open(ctx->makeWsRequest("tok1"));
    ws2.open(ctx->makeWsRequest("tok2"));
    QTRY_COMPARE(ws1.state(), QAbstractSocket::ConnectedState);
    QTRY_COMPARE(ws2.state(), QAbstractSocket::ConnectedState);

    QUdpSocket udp1, udp2;
    QVERIFY(udp1.bind(QHostAddress::LocalHost, 0));
    QVERIFY(udp2.bind(QHostAddress::LocalHost, 0));
    QSignalSpy udpReads1(&udp1, &QUdpSocket::readyRead);
    QSignalSpy udpReads2(&udp2, &QUdpSocket::readyRead);

    const auto t1 = requestTokenOverWs(ws1, replies1);
    const auto t2 = requestTokenOverWs(ws2, replies2);
    udp1.writeDatagram(UdpMediaFrame::makeHello(dev1, t1).encode(), QHostAddress::LocalHost, ctx->udpPort);
    udp2.writeDatagram(UdpMediaFrame::makeHello(dev2, t2).encode(), QHostAddress::LocalHost, ctx->udpPort);
    QVERIFY(waitForDatagram(udp1));
    (void)udp1.receiveDatagram();
    QVERIFY(waitForDatagram(udp2));
    (void)udp2.receiveDatagram();

    // Ping / Pong
    udpReads1.clear();
    udpReads2.clear();
    const QByteArray pingPayload = "ping_timestamp_12345";
    udp1.writeDatagram(UdpMediaFrame::makePing(dev1, dev2, pingPayload).encode(),
                       QHostAddress::LocalHost, ctx->udpPort);
    QTRY_COMPARE(udpReads2.count(), 1);
    const auto ping = UdpMediaFrame::decode(udp2.receiveDatagram().data());
    QVERIFY(ping.has_value());
    QCOMPARE(ping->type, UdpMediaFrame::Type::Ping);
    QCOMPARE(ping->payload, pingPayload);

    // dev2 replies with Pong
    udp2.writeDatagram(UdpMediaFrame::makePong(dev2, dev1, pingPayload).encode(),
                       QHostAddress::LocalHost, ctx->udpPort);
    QTRY_COMPARE(udpReads1.count(), 1);
    const auto pong = UdpMediaFrame::decode(udp1.receiveDatagram().data());
    QVERIFY(pong.has_value());
    QCOMPARE(pong->type, UdpMediaFrame::Type::Pong);
    QCOMPARE(pong->payload, pingPayload);

    // dev1 sends Bye
    udpReads2.clear();
    udp1.writeDatagram(UdpMediaFrame::makeBye(dev1, dev2).encode(),
                       QHostAddress::LocalHost, ctx->udpPort);
    QTRY_COMPARE(udpReads2.count(), 1);
    const auto bye = UdpMediaFrame::decode(udp2.receiveDatagram().data());
    QVERIFY(bye.has_value());
    QCOMPARE(bye->type, UdpMediaFrame::Type::Bye);
    QVERIFY(!ctx->server->udpMediaService()->isBound(dev1));
}

void UdpMediaServiceTest::webSocketDisconnectClearsBinding()
{
    auto ctx = TestContext::create();
    const auto dev1 = DeviceId::generate();
    ctx->server->registerTestToken("tok1", AuthenticatedDevice{AccountId::generate(), dev1});

    auto *ws = new QWebSocket();
    QSignalSpy replies(ws, &QWebSocket::binaryMessageReceived);
    ws->open(ctx->makeWsRequest("tok1"));
    QTRY_COMPARE(ws->state(), QAbstractSocket::ConnectedState);

    QUdpSocket udp;
    QVERIFY(udp.bind(QHostAddress::LocalHost, 0));
    const auto token = requestTokenOverWs(*ws, replies);
    udp.writeDatagram(UdpMediaFrame::makeHello(dev1, token).encode(), QHostAddress::LocalHost, ctx->udpPort);
    QVERIFY(waitForDatagram(udp));
    (void)udp.receiveDatagram();
    QVERIFY(ctx->server->udpMediaService()->isBound(dev1));

    // Close WebSocket
    ws->close();
    QTRY_COMPARE(ws->state(), QAbstractSocket::UnconnectedState);
    ws->deleteLater();

    // Verify binding was cleared on disconnect
    QTRY_VERIFY(!ctx->server->udpMediaService()->isBound(dev1));
}

QTEST_MAIN(UdpMediaServiceTest)
#include "tst_udpmediaservice.moc"
