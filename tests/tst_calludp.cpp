#include <QtTest>

#include "AudioTestSupport.h"
#include "CallTestSupport.h"
#include "app/TransportSettings.h"
#include "call/CallEngine.h"
#include "call/CallSignal.h"
#include "call/SyncCallTransport.h"
#include "call/UdpCallMediaPath.h"
#include "media/AudioConvert.h"
#include "network/SyncEngine.h"
#include "protocol/UdpMediaFrame.h"
#include "AuthService.h"
#include "DirectoryService.h"
#include "EnvelopeService.h"
#include "KeyPackageService.h"
#include "PostgresStore.h"
#include "RelayServer.h"
#include "UdpMediaService.h"

#include <QCryptographicHash>
#include <QHostAddress>
#include <QSignalSpy>
#include <QUdpSocket>
#include <memory>

using namespace OpenChat;
using namespace OpenChat::Relay;

namespace {

struct RelayTestContext {
    std::unique_ptr<PostgresStore> store;
    std::unique_ptr<AuthService> auth;
    std::unique_ptr<EnvelopeService> envelopes;
    std::unique_ptr<KeyPackageService> keyPackages;
    std::unique_ptr<DirectoryService> directory;
    std::unique_ptr<RelayServer> server;
    quint16 wsPort = 0;
    quint16 udpPort = 0;

    static std::unique_ptr<RelayTestContext> create()
    {
        auto ctx = std::make_unique<RelayTestContext>();
        ctx->store = PostgresStore::createNull(QStringLiteral("tst_calludp_null_store"));
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
};

// Test transport with exact SyncCallTransport routing logic:
// Audio frames (v1, <= 1400 bytes) attempt UDP media path first.
// If UDP is not active or fails, falls back to WS datagram path.
class TestCallTransport final : public CallTransport
{
public:
    explicit TestCallTransport(const DeviceId &device) : localDevice(device) {}

    void sendSignal(const ConversationId &conversation, const DeviceId &recipientDevice,
                    const QByteArray &payload) override
    {
        sentSignals.append(payload);
        if (peer && peer->onSignal)
            peer->onSignal(conversation, localDevice, payload);
    }

    void sendMedia(const ConversationId &conversation, const DeviceId &recipientDevice,
                   const QByteArray &packet) override
    {
        if (!connected)
            return;

        // Exact SyncCallTransport routing:
        if (udpMediaPath && !packet.isEmpty() && static_cast<quint8>(packet.at(0)) == 1
            && packet.size() <= 1400) {
            if (udpMediaPath->sendMedia(recipientDevice, packet)) {
                ++udpMediaSent;
                return;
            }
            if (udpOnly)
                return;
        }

        ++wsMediaSent;
        if (peer && peer->onMedia)
            peer->onMedia(conversation, localDevice, packet);
    }

    [[nodiscard]] bool isConnected() const override { return connected; }

    TestCallTransport *peer = nullptr;
    DeviceId localDevice;
    UdpCallMediaPath *udpMediaPath = nullptr;
    bool connected = true;
    bool udpOnly = false;
    int udpMediaSent = 0;
    int wsMediaSent = 0;
    QList<QByteArray> sentSignals;
};

struct TestEndpoint final {
    DeviceId deviceId = DeviceId::generate();
    CallTest::ScriptedAudioDevices devices;
    std::unique_ptr<TestCallTransport> transport;
    std::unique_ptr<UdpCallMediaPath> udpPath;
    std::unique_ptr<CallEngine> engine;
    qint64 nowMs = 0;

    void build(quint16 udpPort, CallEngine::Config config = {})
    {
        transport = std::make_unique<TestCallTransport>(deviceId);
        udpPath = std::make_unique<UdpCallMediaPath>(deviceId);
        udpPath->setRelayEndpoint(QHostAddress::LocalHost, udpPort);
        transport->udpMediaPath = udpPath.get();

        config.mediaClock = [this] { return nowMs; };
        engine = std::make_unique<CallEngine>(config, *transport, devices.factory());
        engine->setSoundsEnabled(false);
        engine->setUdpMediaPath(udpPath.get());
    }

    void speak(const AudioFrame &frame) { devices.speak(frame); }
    [[nodiscard]] AudioFrame listen()
    {
        nowMs += CallAudioFormat::frameDurationMs;
        return devices.listen();
    }
};

CallEngine::CallPeer peerFor(const ConversationId &conversation, const DeviceId &device, const QString &name)
{
    CallEngine::CallPeer peer;
    peer.conversation = conversation;
    peer.device = device;
    peer.displayName = name;
    peer.avatarKey = QStringLiteral("userpfp_none");
    return peer;
}

// Minimal SyncEngine mocks for testing SyncCallTransport
class MinimalSyncStore final : public SyncStore
{
public:
    Result<void, RepositoryError> commitSend(const MessageRecord &, const OutboxRecord &, QByteArrayView) override
    {
        return Result<void, RepositoryError>::success();
    }
    Result<void, RepositoryError> commitControlSend(const OutboxRecord &, QByteArrayView) override
    {
        return Result<void, RepositoryError>::success();
    }
    Result<void, RepositoryError> commitGroupSend(const MessageRecord &, const QVector<OutboxRecord> &, QByteArrayView) override
    {
        return Result<void, RepositoryError>::success();
    }
    Result<void, RepositoryError> commitControlSendMany(const QVector<OutboxRecord> &, QByteArrayView) override
    {
        return Result<void, RepositoryError>::success();
    }
    Result<void, RepositoryError> failEnvelope(const EnvelopeId &) override
    {
        return Result<void, RepositoryError>::success();
    }
    Result<void, RepositoryError> commitMlsStateOnly(QByteArrayView) override
    {
        return Result<void, RepositoryError>::success();
    }
    Result<bool, RepositoryError> canJoinGroup(const AccountId &, const ConversationId &) override
    {
        return Result<bool, RepositoryError>::success(true);
    }
    Result<bool, RepositoryError> commitGroupWelcome(const EnvelopeId &, const DeviceId &, const ConversationId &, quint64, qint64, QByteArrayView, bool) override
    {
        return Result<bool, RepositoryError>::success(true);
    }
    Result<bool, RepositoryError> commitReceive(const MessageRecord &, const EnvelopeId &, quint64, QByteArrayView) override
    {
        return Result<bool, RepositoryError>::success(true);
    }
    Result<bool, RepositoryError> commitControlReceive(const EnvelopeId &, const DeviceId &, quint64, QByteArrayView) override
    {
        return Result<bool, RepositoryError>::success(true);
    }
    Result<HandshakeReceiveOutcome, RepositoryError> commitHandshakeReceive(const EnvelopeId &, const AccountId &, const DeviceId &, const ConversationId &, QByteArrayView, qint64, quint64) override
    {
        return Result<HandshakeReceiveOutcome, RepositoryError>::success(HandshakeReceiveOutcome::Stashed);
    }
    Result<void, RepositoryError> commitHandshakeAccept(const AccountId &, const ConversationId &, qint64, QByteArrayView, QByteArrayView) override
    {
        return Result<void, RepositoryError>::success();
    }
    Result<bool, RepositoryError> hasSeen(const EnvelopeId &) override
    {
        return Result<bool, RepositoryError>::success(false);
    }
    Result<QVector<OutboxRecord>, RepositoryError> claimDue(qint64, int, qint64) override
    {
        return Result<QVector<OutboxRecord>, RepositoryError>::success({});
    }
    Result<void, RepositoryError> markAccepted(const EnvelopeId &) override
    {
        return Result<void, RepositoryError>::success();
    }
    Result<void, RepositoryError> scheduleRetry(const EnvelopeId &, int, qint64) override
    {
        return Result<void, RepositoryError>::success();
    }
    Result<void, RepositoryError> advanceDeliveryState(const MessageId &, DeliveryState) override
    {
        return Result<void, RepositoryError>::success();
    }
    Result<void, RepositoryError> failSend(const EnvelopeId &, const MessageId &) override
    {
        return Result<void, RepositoryError>::success();
    }
};

class MinimalSyncMls final : public SyncMlsSession
{
public:
    Result<QByteArray, MlsError> encrypt(const ConversationId &, QByteArrayView plaintext) override
    {
        return Result<QByteArray, MlsError>::success(plaintext.toByteArray());
    }
    Result<SyncProcessOutcome, MlsError> process(const ConversationId &, QByteArrayView mlsMessage) override
    {
        SyncProcessOutcome outcome;
        outcome.kind = SyncProcessOutcome::Kind::Application;
        outcome.applicationData = mlsMessage.toByteArray();
        return Result<SyncProcessOutcome, MlsError>::success(outcome);
    }
    Result<QList<QByteArray>, MlsError> inspectWelcome(QByteArrayView) override
    {
        return Result<QList<QByteArray>, MlsError>::success({});
    }
    Result<void, MlsError> joinGroup(const ConversationId &, QByteArrayView) override
    {
        return Result<void, MlsError>::success();
    }
    QByteArray takePendingState() override { return "mls_state"; }
};

class MinimalSyncTransport final : public SyncTransport
{
public:
    bool isConnected() const override { return true; }
    void sendEnvelope(const CiphertextEnvelopeV1 &) override {}
    void sendDatagram(const CiphertextEnvelopeV1 &envelope) override
    {
        datagramsSent.append(envelope);
    }
    void acknowledge(const EnvelopeId &, quint64) override {}

    QVector<CiphertextEnvelopeV1> datagramsSent;
};

} // namespace

class CallUdpTest final : public QObject
{
    Q_OBJECT

private slots:
    void init()
    {
        QSettings().remove(QStringLiteral("Transport/mode"));
    }

    void peerActivationAndRttCalculation()
    {
        auto ctx = RelayTestContext::create();
        QVERIFY(ctx->udpPort > 0);

        const auto dev1 = DeviceId::generate();
        const auto dev2 = DeviceId::generate();

        UdpCallMediaPath path1(dev1);
        UdpCallMediaPath path2(dev2);
        path1.setRelayEndpoint(QHostAddress::LocalHost, ctx->udpPort);
        path2.setRelayEndpoint(QHostAddress::LocalHost, ctx->udpPort);

        // Mint single-use tokens from the relay
        const auto tok1 = ctx->server->udpMediaService()->mintToken(dev1);
        const auto tok2 = ctx->server->udpMediaService()->mintToken(dev2);
        QVERIFY(!tok1.isEmpty());
        QVERIFY(!tok2.isEmpty());

        path1.onTokenReceived(tok1);
        path2.onTokenReceived(tok2);

        path1.startProbing(dev2);
        path2.startProbing(dev1);

        // Both peers should become Active via Hello -> BindOk and Ping -> Pong
        QTRY_VERIFY_WITH_TIMEOUT(path1.isPeerActive(dev2), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(path2.isPeerActive(dev1), 5000);

        QCOMPARE(path1.peerState(dev2), UdpPeerState::Active);
        QCOMPARE(path2.peerState(dev1), UdpPeerState::Active);

        // RTT is measured
        QVERIFY(path1.rttMs(dev2) >= 0.0);
        QVERIFY(path2.rttMs(dev1) >= 0.0);

        // UI text format
        const QString text1 = path1.mediaPathText(dev2);
        QVERIFY2(text1.startsWith(QStringLiteral("UDP · ")), qPrintable(text1));
        QVERIFY2(text1.endsWith(QStringLiteral(" ms")), qPrintable(text1));
    }

    void fullCallAudioRoundtripOverUdp()
    {
        auto ctx = RelayTestContext::create();
        QVERIFY(ctx->udpPort > 0);

        TestEndpoint alice;
        TestEndpoint bob;
        alice.build(ctx->udpPort);
        bob.build(ctx->udpPort);

        alice.transport->peer = bob.transport.get();
        bob.transport->peer = alice.transport.get();

        const ConversationId conversation = ConversationId::generate();

        // Connect media paths with tokens
        alice.udpPath->onTokenReceived(ctx->server->udpMediaService()->mintToken(alice.deviceId));
        bob.udpPath->onTokenReceived(ctx->server->udpMediaService()->mintToken(bob.deviceId));

        // Alice places call to Bob
        const auto bobPeer = peerFor(conversation, bob.deviceId, QStringLiteral("Bob"));
        QVERIFY(alice.engine->placeCall(bobPeer));

        QTRY_COMPARE(alice.engine->state(), CallState::Ringing);
        QTRY_COMPARE(bob.engine->state(), CallState::Ringing);

        // Bob accepts call
        bob.engine->acceptCall();
        QCOMPARE(alice.engine->state(), CallState::Connecting);
        QCOMPARE(bob.engine->state(), CallState::Connecting);

        // Both paths probe and become Active
        alice.udpPath->startProbing(bob.deviceId);
        bob.udpPath->startProbing(alice.deviceId);
        QTRY_VERIFY_WITH_TIMEOUT(alice.udpPath->isPeerActive(bob.deviceId), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(bob.udpPath->isPeerActive(alice.deviceId), 5000);

        // Alice speaks an audio frame (Opus)
        const AudioFrame tone =
            AudioConvert::toFrames(AudioTest::tone(20, 440.0, 0.5)).first();

        // Speak frame from Alice: must route via UDP
        alice.speak(tone);
        QTRY_VERIFY(alice.transport->udpMediaSent > 0);
        QCOMPARE(alice.transport->wsMediaSent, 0);

        // Bob receives via UDP direct media and transitions to Active
        QTRY_COMPARE(bob.engine->state(), CallState::Active);

        // Bob speaks frame back to Alice: must route via UDP
        bob.speak(tone);
        QTRY_VERIFY(bob.transport->udpMediaSent > 0);
        QCOMPARE(bob.transport->wsMediaSent, 0);

        // Alice receives and transitions to Active
        QTRY_COMPARE(alice.engine->state(), CallState::Active);

        // Both endpoints are in CallState::Active purely driven by UDP media!
        QCOMPARE(alice.engine->state(), CallState::Active);
        QCOMPARE(bob.engine->state(), CallState::Active);
    }

    void lossTimeoutCausesTransparentFallbackToWebSocket()
    {
        auto ctx = RelayTestContext::create();
        TestEndpoint alice;
        TestEndpoint bob;
        alice.build(ctx->udpPort);
        bob.build(ctx->udpPort);
        alice.transport->peer = bob.transport.get();
        bob.transport->peer = alice.transport.get();

        const ConversationId conversation = ConversationId::generate();
        alice.udpPath->onTokenReceived(ctx->server->udpMediaService()->mintToken(alice.deviceId));
        bob.udpPath->onTokenReceived(ctx->server->udpMediaService()->mintToken(bob.deviceId));

        alice.engine->placeCall(peerFor(conversation, bob.deviceId, QStringLiteral("Bob")));
        bob.engine->acceptCall();

        alice.udpPath->startProbing(bob.deviceId);
        bob.udpPath->startProbing(alice.deviceId);
        QTRY_VERIFY_WITH_TIMEOUT(alice.udpPath->isPeerActive(bob.deviceId), 5000);

        const AudioFrame tone =
            AudioConvert::toFrames(AudioTest::tone(20, 440.0, 0.5)).first();

        alice.speak(tone);
        bob.speak(tone);
        QTRY_COMPARE(alice.engine->state(), CallState::Active);
        QTRY_COMPARE(bob.engine->state(), CallState::Active);
        QCOMPARE(alice.transport->wsMediaSent, 0);

        // Simulate network loss on Alice's UDP path: close socket and advance clock
        qint64 clockTime = alice.udpPath->nowMs();
        alice.udpPath->setClock([&clockTime] { return clockTime; });
        alice.udpPath->socket()->close();

        // Advance past loss timeout (3000 ms) and trigger state check
        clockTime += UdpCallMediaPath::lossTimeoutMs + 500;
        QMetaObject::invokeMethod(alice.udpPath.get(), "onTickerTimeout");

        // Alice's UDP path for Bob transitions to Suspended
        QCOMPARE(alice.udpPath->peerState(bob.deviceId), UdpPeerState::Suspended);
        QVERIFY(!alice.udpPath->isPeerActive(bob.deviceId));
        QCOMPARE(alice.udpPath->mediaPathText(bob.deviceId), QStringLiteral("Relay (TCP)"));

        // Alice speaks another frame: must transparently fall back to WS!
        const int prevWsSent = alice.transport->wsMediaSent;
        alice.speak(tone);

        QTRY_VERIFY(alice.transport->wsMediaSent > prevWsSent);
        // Call remains Active!
        QCOMPARE(alice.engine->state(), CallState::Active);
        QCOMPARE(bob.engine->state(), CallState::Active);
    }

    void hangupSendsByeAndStopsMediaPath()
    {
        auto ctx = RelayTestContext::create();
        TestEndpoint alice;
        TestEndpoint bob;
        alice.build(ctx->udpPort);
        bob.build(ctx->udpPort);
        alice.transport->peer = bob.transport.get();
        bob.transport->peer = alice.transport.get();

        const ConversationId conversation = ConversationId::generate();
        alice.udpPath->onTokenReceived(ctx->server->udpMediaService()->mintToken(alice.deviceId));
        bob.udpPath->onTokenReceived(ctx->server->udpMediaService()->mintToken(bob.deviceId));

        alice.engine->placeCall(peerFor(conversation, bob.deviceId, QStringLiteral("Bob")));
        bob.engine->acceptCall();
        alice.udpPath->startProbing(bob.deviceId);
        bob.udpPath->startProbing(alice.deviceId);
        QTRY_VERIFY_WITH_TIMEOUT(alice.udpPath->isPeerActive(bob.deviceId), 5000);

        // Track UDP datagrams on relay
        QUdpSocket observer;
        QVERIFY(observer.bind(QHostAddress::LocalHost, 0));

        // Alice hangs up
        alice.engine->hangUp();
        QCOMPARE(alice.engine->state(), CallState::Ended);

        // Verify alice's udpPath state is stopped for Bob
        QVERIFY(!alice.udpPath->isPeerActive(bob.deviceId));
    }

    void transportSettingsModes()
    {
        const auto dev1 = DeviceId::generate();
        const auto dev2 = DeviceId::generate();

        UdpCallMediaPath path(dev1);
        TransportSettings settings;
        path.setSettings(&settings);

        // Default mode is Auto
        QCOMPARE(settings.transportMode(), TransportMode::Auto);

        // Switch to Tcp
        settings.setTransportMode(TransportMode::Tcp);
        QCOMPARE(settings.transportMode(), TransportMode::Tcp);
        path.startProbing(dev2);
        // Under TcpOnly, peer state is immediately Suspended / not active
        QVERIFY(!path.isPeerActive(dev2));
        QCOMPARE(path.mediaPathText(dev2), QStringLiteral("Relay (TCP)"));

        // Switch to Udp
        settings.setTransportMode(TransportMode::Udp);
        QCOMPARE(settings.transportMode(), TransportMode::Udp);
    }

    void syncCallTransportRoutingRules()
    {
        SyncEngine::Config cfg{AccountId::generate(), DeviceId::generate()};

        MinimalSyncStore store;
        MinimalSyncMls mls;
        MinimalSyncTransport transport;
        SyncEngine syncEngine(
            cfg, store, mls, transport,
            [](QByteArrayView) { return QByteArray(64, 's'); },
            [] { return 1'700'000'000'000; });

        SyncCallTransport callTransport(syncEngine);

        const auto localDev = cfg.localDeviceId;
        const auto peerDev = DeviceId::generate();
        const auto conv = ConversationId::generate();

        UdpCallMediaPath udpPath(localDev);
        callTransport.setUdpMediaPath(&udpPath);

        // Voice packet (version 1, <= 1400 B)
        QByteArray voicePacket(200, '\x00');
        voicePacket[0] = static_cast<char>(1);

        // 1. While UDP is NOT active, packet goes to SyncEngine's WS path
        callTransport.sendMedia(conv, peerDev, voicePacket);
        QCOMPARE(transport.datagramsSent.size(), 1);

        // 2. Video packet (version 2) ALWAYS goes to SyncEngine, never UDP
        QByteArray videoPacket(3000, '\x00');
        videoPacket[0] = static_cast<char>(2);
        callTransport.sendMedia(conv, peerDev, videoPacket);
        QCOMPARE(transport.datagramsSent.size(), 2);

        // 3. Screen share packet (version 3) ALWAYS goes to SyncEngine
        QByteArray screenPacket(5000, '\x00');
        screenPacket[0] = static_cast<char>(3);
        callTransport.sendMedia(conv, peerDev, screenPacket);
        QCOMPARE(transport.datagramsSent.size(), 3);

        // 4. Inbound datagram on SyncEngine forwards to onMedia
        bool mediaReceived = false;
        callTransport.onMedia = [&](const ConversationId &, const DeviceId &, const QByteArray &) {
            mediaReceived = true;
        };

        const QByteArray hash = QCryptographicHash::hash(voicePacket, QCryptographicHash::Sha256);
        const CiphertextEnvelopeV1 inboundEnv{
            1,
            EnvelopeId::generate(),
            AccountId::generate(),
            peerDev,
            localDev,
            conv,
            EnvelopeMessageKind::CallMedia,
            1'700'000'000'000,
            1'700'000'060'000,
            EnvelopeId::generate(),
            voicePacket,
            hash,
            QByteArray(64, '\x01')};
        syncEngine.handleDatagram(inboundEnv);

        QVERIFY(mediaReceived);
    }
};

QTEST_MAIN(CallUdpTest)
#include "tst_calludp.moc"
