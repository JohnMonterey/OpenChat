// End-to-end UDP media bring-up across the seam no other test covers: a real
// RelayClient asking for a media token over its live WebSocket, a relay that
// mints from a real UdpMediaService exactly as RelayServer::handleLiveBinary
// does, and a real UdpCallMediaPath turning that token into a Hello, a binding
// and an Active peer.
//
// tst_relayclient proves the client emits mediaTokenReceived when a server
// replies [13, token]. tst_udpmediaservice proves the relay answers [12] with
// [13, token] and forwards media between two bound endpoints. tst_calludp
// proves the path activates once a token is injected by hand. Nobody joins
// those halves, which is exactly where a client that always falls back to the
// WebSocket would hide.

#include "relay/RelayTestSupport.h"

#include "app/TransportSettings.h"
#include "call/UdpCallMediaPath.h"
#include "network/RelayClient.h"
#include "protocol/UdpMediaFrame.h"

#include "UdpMediaService.h"

#include <QCborArray>
#include <QCborValue>
#include <QSignalSpy>
#include <QSslSocket>
#include <QWebSocket>
#include <QtTest/QtTest>

#include <memory>

using namespace OpenChat;
using namespace OpenChat::Relay;

class UdpMediaE2eTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void tokenOverLiveSocketBringsPeersActive();
    void reprobeAfterBindingLossRecovers();
};

namespace {

RelayEndpoints wssOnly(const QUrl &live)
{
    RelayEndpoints endpoints;
    endpoints.live = live;
    return endpoints;
}

RelayCredentials fixedCredentials()
{
    RelayCredentials credentials;
    credentials.accessToken = [] { return QByteArray("access"); };
    credentials.refreshToken = [] { return QByteArray("refresh"); };
    return credentials;
}

// One client end: its own TLS live socket, a real RelayClient and a real UDP
// media path pointed at the shared relay media service.
struct Endpoint final {
    DeviceId device = DeviceId::generate();
    std::unique_ptr<RelayTest::FakeWssServer> server;
    std::unique_ptr<RelayClient> client;
    std::unique_ptr<UdpCallMediaPath> path;
    int tokenRequests = 0;

    // Mirrors RelayServer::handleLiveBinary's control-frame branch for [12]:
    // mint a single-use token for this device and reply [13, token].
    void serve(UdpMediaService &media, const QSslConfiguration &tls)
    {
        server = std::make_unique<RelayTest::FakeWssServer>(
            tls, QStringList{QString::fromLatin1(relaySubprotocol)});
        server->onConnected = [this, &media](QWebSocket *socket) {
            QObject::connect(socket, &QWebSocket::binaryMessageReceived, socket,
                             [this, &media, socket](const QByteArray &bytes) {
                                 const auto request = QCborValue::fromCbor(bytes).toArray();
                                 if (request.size() != 1 || request.at(0).toInteger() != 12)
                                     return;
                                 ++tokenRequests;
                                 QCborArray reply;
                                 reply.append(13);
                                 reply.append(media.mintToken(device));
                                 socket->sendBinaryMessage(reply.toCborValue().toCbor());
                             });
        };
    }

    void connectClient(const QSslConfiguration &clientTls, quint16 mediaPort,
                       TransportSettings *settings)
    {
        client = std::make_unique<RelayClient>(device, AccountId::generate(),
                                               wssOnly(server->liveUrl()), fixedCredentials());
        client->setTlsConfiguration(clientTls);
        path = std::make_unique<UdpCallMediaPath>(device);
        path->setRelayEndpoint(QHostAddress::LocalHost, mediaPort);
        path->setRelayClient(client.get());
        path->setSettings(settings);
        client->connectLive(0);
    }
};

} // namespace

void UdpMediaE2eTest::initTestCase()
{
    QVERIFY2(QSslSocket::supportsSsl(), "TLS backend must be available for these tests");
}

// The whole bring-up with nothing hand-fed: both ends ask the relay for a token
// over the live socket, Hello binds them, and each peer reaches Active purely
// from the periodic probe traffic the path generates on its own.
void UdpMediaE2eTest::tokenOverLiveSocketBringsPeersActive()
{
    RelayTest::CertAuthority ca;
    const auto serverTls = RelayTest::serverConfig(ca.localhostLeaf());
    const auto clientTls = RelayTest::clientConfigTrusting(ca.caCertPem());

    UdpMediaService media;
    const quint16 mediaPort = media.start(QHostAddress::LocalHost, 0);
    QVERIFY(mediaPort > 0);

    TransportSettings settings;
    settings.setTransportMode(TransportMode::Auto);

    Endpoint alice;
    Endpoint bob;
    alice.serve(media, serverTls);
    bob.serve(media, serverTls);
    QVERIFY(alice.server->isListening());
    QVERIFY(bob.server->isListening());

    alice.connectClient(clientTls, mediaPort, &settings);
    bob.connectClient(clientTls, mediaPort, &settings);
    QTRY_VERIFY(alice.client->isConnected());
    QTRY_VERIFY(bob.client->isConnected());

    // This is the call: CallEngine::startMedia() does exactly this and nothing
    // more for the UDP path.
    alice.path->startProbing(bob.device);
    bob.path->startProbing(alice.device);

    // The token has to come back over the live socket before any Hello exists.
    QTRY_VERIFY_WITH_TIMEOUT(alice.tokenRequests > 0, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(bob.tokenRequests > 0, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(media.isBound(alice.device), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(media.isBound(bob.device), 5000);

    // And then the peers must find each other without any injected packet. The
    // path only pings on its 5 s re-probe cadence, so allow two rounds.
    QTRY_VERIFY_WITH_TIMEOUT(alice.path->isPeerActive(bob.device), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(bob.path->isPeerActive(alice.device), 15000);

    // A voice-sized packet now rides UDP rather than reporting failure to the
    // caller, which is what makes SyncCallTransport skip the WebSocket.
    QSignalSpy received(bob.path.get(), &UdpCallMediaPath::mediaReceived);
    QByteArray voice(200, '\x07');
    voice[0] = '\x01'; // audio wire version
    QVERIFY(alice.path->sendMedia(bob.device, voice));
    QTRY_VERIFY(received.count() > 0);
    QCOMPARE(received.first().at(1).toByteArray(), voice);
}

// A binding can be lost while the call is up: the relay drops it after silence,
// on a Bye, or when the live socket reconnects. The path re-probes every 5 s,
// and that re-probe has to produce a binding again rather than replaying a
// token the relay has already consumed.
void UdpMediaE2eTest::reprobeAfterBindingLossRecovers()
{
    RelayTest::CertAuthority ca;
    const auto serverTls = RelayTest::serverConfig(ca.localhostLeaf());
    const auto clientTls = RelayTest::clientConfigTrusting(ca.caCertPem());

    UdpMediaService media;
    const quint16 mediaPort = media.start(QHostAddress::LocalHost, 0);
    QVERIFY(mediaPort > 0);

    TransportSettings settings;
    settings.setTransportMode(TransportMode::Auto);

    Endpoint alice;
    Endpoint bob;
    alice.serve(media, serverTls);
    bob.serve(media, serverTls);

    alice.connectClient(clientTls, mediaPort, &settings);
    bob.connectClient(clientTls, mediaPort, &settings);
    QTRY_VERIFY(alice.client->isConnected());
    QTRY_VERIFY(bob.client->isConnected());

    alice.path->startProbing(bob.device);
    bob.path->startProbing(alice.device);
    QTRY_VERIFY_WITH_TIMEOUT(alice.path->isPeerActive(bob.device), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(bob.path->isPeerActive(alice.device), 15000);

    // The relay forgets Alice, exactly as it does when her live socket drops.
    media.clearBinding(alice.device);
    QVERIFY(!media.isBound(alice.device));

    // Alice must get herself bound again off her own re-probe.
    QTRY_VERIFY_WITH_TIMEOUT(media.isBound(alice.device), 20000);
    QTRY_VERIFY_WITH_TIMEOUT(alice.path->isPeerActive(bob.device), 20000);
}

QTEST_MAIN(UdpMediaE2eTest)
#include "tst_udpmediae2e.moc"
