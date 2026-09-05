// Two-client end-to-end integration test: proves the whole OpenChat pipeline
// over a REAL TLS edge, a REAL in-process relay, and a REAL PostgreSQL store.
//
// Two freshly created ProfileSessions (SQLCipher + real MlsClient) each bootstrap
// against an in-process RelayServer through an in-process TLS-terminating reverse
// proxy, then exchange a content-blind MLS add-contact handshake and application
// messages over their live WebSocket streams. Nothing here is mocked below the
// TLS socket: the relay authenticates, stores, and routes opaque ciphertext
// through Postgres exactly as in production.
//
// The one edge behaviour the proxy adds mirrors the mandatory production reverse
// proxy (deploy/Caddyfile.dev): the relay accepts the /v1/live upgrade through
// QHttpServer, which cannot declare a WebSocket subprotocol, yet RelayClient
// requires the server to echo `openchat.ciphertext.v1`. The proxy injects that
// echo into the 101 response, just as Caddy does with a header_down directive.

#include "app/AccountBootstrap.h"
#include "app/AddContactService.h"
#include "app/ContactRequestService.h"
#include "app/DeviceLink.h"
#include "app/ProfileSession.h"
#include "AudioTestSupport.h"
#include "CallTestSupport.h"
#include "call/CallEngine.h"
#include "call/SyncCallTransport.h"
#include "controllers/ChatController.h"
#include "media/AudioConvert.h"
#include "domain/ChatTypes.h"
#include "domain/Contact.h"
#include "network/RelayClient.h"
#include "network/RelayTransport.h"
#include "network/SyncEngine.h"
#include "security/KeyVault.h"
#include "security/SafetyNumber.h"
#include "security/SecureBuffer.h"
#include "storage/SqlCipherChatRepository.h"
#include "storage/SqlCipherContactRepository.h"

#include "AuthService.h"
#include "DirectoryService.h"
#include "EnvelopeService.h"
#include "KeyPackageService.h"
#include "PostgresStore.h"
#include "RelayServer.h"

#include "relay/RelayTestSupport.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QHostAddress>
#include <QSslServer>
#include <QSslSocket>
#include <QSqlError>
#include <QSqlQuery>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest/QtTest>

#include <memory>
#include <optional>

using namespace OpenChat;
using namespace OpenChat::Relay;

namespace {

// Minimal in-memory KeyVault: this environment has no OS keychain, so the
// profile's database and wrapping keys live only for the test. Mirrors the fake
// vault used by tst_accountbootstrap / tst_addcontactservice.
class InMemoryVault final : public KeyVault
{
public:
    KeyVaultAvailability availability() const override { return KeyVaultAvailability::Available; }

    Result<SecureBuffer, KeyVaultError> readProfileKey(const ProfileId &) override
    {
        return read(m_databaseKey);
    }
    Result<SecureBuffer, KeyVaultError> createProfileKey(const ProfileId &) override
    {
        return create(m_databaseKey);
    }
    Result<void, KeyVaultError> deleteProfileKey(const ProfileId &) override
    {
        m_databaseKey.reset();
        return Result<void, KeyVaultError>::success();
    }
    Result<SecureBuffer, KeyVaultError> readDeviceWrappingKey(const ProfileId &) override
    {
        return read(m_wrappingKey);
    }
    Result<SecureBuffer, KeyVaultError> createDeviceWrappingKey(const ProfileId &) override
    {
        return create(m_wrappingKey);
    }
    Result<void, KeyVaultError> deleteDeviceWrappingKey(const ProfileId &) override
    {
        m_wrappingKey.reset();
        return Result<void, KeyVaultError>::success();
    }

private:
    static Result<SecureBuffer, KeyVaultError> read(const std::optional<SecureBuffer> &key)
    {
        if (!key)
            return Result<SecureBuffer, KeyVaultError>::failure(KeyVaultError::NotFound);
        return Result<SecureBuffer, KeyVaultError>::success(SecureBuffer::fromBytes(key->view()));
    }
    static Result<SecureBuffer, KeyVaultError> create(std::optional<SecureBuffer> &key)
    {
        if (key)
            return Result<SecureBuffer, KeyVaultError>::failure(KeyVaultError::AlreadyExists);
        key = SecureBuffer::random(32);
        return Result<SecureBuffer, KeyVaultError>::success(SecureBuffer::fromBytes(key->view()));
    }

    std::optional<SecureBuffer> m_databaseKey;
    std::optional<SecureBuffer> m_wrappingKey;
};

// One spliced connection of the in-process TLS proxy. Terminates TLS with the
// leaf presented by the QSslServer, opens a plain TCP socket to the loopback
// relay, and splices bytes both ways. For the /v1/live upgrade it injects the
// Sec-WebSocket-Protocol echo into the 101 response (when injection is enabled);
// every other byte passes through untouched.
class ProxyConnection final : public QObject
{
    Q_OBJECT

public:
    ProxyConnection(QSslSocket *client, quint16 backendPort, bool injectSubprotocol,
                    QObject *parent)
        : QObject(parent), m_client(client), m_injectSubprotocol(injectSubprotocol)
    {
        m_client->setParent(this);
        m_backend = new QTcpSocket(this);

        connect(m_client, &QIODevice::readyRead, this, &ProxyConnection::onClientReadyRead);
        connect(m_client, &QSslSocket::disconnected, this, &ProxyConnection::closeConnection);
        connect(m_client, &QAbstractSocket::errorOccurred, this,
                [this](QAbstractSocket::SocketError) { closeConnection(); });

        connect(m_backend, &QTcpSocket::connected, this, &ProxyConnection::onBackendConnected);
        connect(m_backend, &QIODevice::readyRead, this, &ProxyConnection::onBackendReadyRead);
        connect(m_backend, &QTcpSocket::disconnected, this, &ProxyConnection::closeConnection);
        connect(m_backend, &QAbstractSocket::errorOccurred, this,
                [this](QAbstractSocket::SocketError) { closeConnection(); });

        m_backend->connectToHost(QHostAddress(QHostAddress::LocalHost), backendPort);
    }

private:
    void onClientReadyRead()
    {
        m_pendingClient += m_client->readAll();
        flushClientToBackend();
    }

    void onBackendConnected()
    {
        m_backendConnected = true;
        flushClientToBackend();
    }

    void flushClientToBackend()
    {
        // Latch liveness from the request line as soon as we have enough of it. The
        // decision only gates the backend->client 101 injection, and the client
        // always sends the full request before the relay replies, so this is set
        // well before any 101 arrives.
        if (!m_liveDetermined && (m_pendingClient.contains('\n') || m_pendingClient.size() >= 16)) {
            m_isLive = m_pendingClient.startsWith("GET /v1/live");
            m_liveDetermined = true;
        }
        if (m_backendConnected && !m_pendingClient.isEmpty()) {
            m_backend->write(m_pendingClient);
            m_pendingClient.clear();
        }
    }

    void onBackendReadyRead()
    {
        const QByteArray chunk = m_backend->readAll();
        if (chunk.isEmpty())
            return;

        // Straight pass-through once the upgrade is handled, or whenever this is
        // not an injectable live upgrade.
        if (m_upgradeSpliced || !(m_injectSubprotocol && m_isLive)) {
            m_client->write(chunk);
            return;
        }

        // Buffer the 101 response header block, then inject the subprotocol echo
        // exactly once, before splicing raw thereafter.
        m_pendingBackend += chunk;
        const int headerEnd = m_pendingBackend.indexOf("\r\n\r\n");
        if (headerEnd < 0)
            return;

        QByteArray headerBlock = m_pendingBackend.left(headerEnd);
        const QByteArray rest = m_pendingBackend.mid(headerEnd + 4);
        if (!headerBlock.toLower().contains("sec-websocket-protocol:")) {
            headerBlock += "\r\nSec-WebSocket-Protocol: ";
            headerBlock += relaySubprotocol;
        }
        m_client->write(headerBlock + "\r\n\r\n" + rest);
        m_upgradeSpliced = true;
        m_pendingBackend.clear();
    }

    void closeConnection()
    {
        if (m_torn)
            return;
        m_torn = true;
        if (m_backend)
            m_backend->disconnectFromHost();
        if (m_client)
            m_client->disconnectFromHost();
        deleteLater();
    }

    QSslSocket *m_client = nullptr;
    QTcpSocket *m_backend = nullptr;
    bool m_injectSubprotocol = true;
    bool m_backendConnected = false;
    bool m_liveDetermined = false;
    bool m_isLive = false;
    bool m_upgradeSpliced = false;
    bool m_torn = false;
    QByteArray m_pendingClient;  // client bytes held until the backend connects
    QByteArray m_pendingBackend; // 101 header block held until it is complete
};

// A minimal in-process TLS-terminating reverse proxy standing in for the
// production Caddy edge. Presents the given leaf, decrypts, and splices plaintext
// to the plain HTTP/WS relay backend on loopback.
class TlsProxy final : public QObject
{
    Q_OBJECT

public:
    TlsProxy(const QSslConfiguration &serverTls, quint16 backendPort, bool injectSubprotocol,
             QObject *parent = nullptr)
        : QObject(parent), m_backendPort(backendPort), m_injectSubprotocol(injectSubprotocol)
    {
        m_server = new QSslServer(this);
        m_server->setSslConfiguration(serverTls);
        m_server->setHandshakeTimeout(10000);
        m_server->listen(QHostAddress::LocalHost, 0);

        connect(m_server, &QTcpServer::pendingConnectionAvailable, this, [this] {
            while (QTcpSocket *pending = m_server->nextPendingConnection()) {
                auto *client = qobject_cast<QSslSocket *>(pending);
                if (!client) {
                    pending->deleteLater();
                    continue;
                }
                new ProxyConnection(client, m_backendPort, m_injectSubprotocol, this);
            }
        });
    }

    [[nodiscard]] bool isListening() const { return m_server && m_server->isListening(); }
    [[nodiscard]] quint16 port() const { return m_server ? m_server->serverPort() : 0; }

private:
    QSslServer *m_server = nullptr;
    quint16 m_backendPort = 0;
    bool m_injectSubprotocol = true;
};

// Everything one client owns for its lifetime. Declared so member destruction is
// a safe backstop to lock()'s explicit teardown: the destructor stops the live
// socket and locks the session (which resets the SyncEngine that borrows the
// transport and client) before any member is destroyed, mirroring the
// disconnect()-then-lock() order of tst_accountbootstrap.
struct ClientStack final {
    QTemporaryDir dir;
    InMemoryVault vault;
    std::unique_ptr<ProfileSession> session;
    std::unique_ptr<RelayClient> client;
    std::unique_ptr<RelayTransport> transport;
    AccountId account = AccountId::generate();
    DeviceId deviceId = DeviceId::generate();
    QByteArray signingPublicKey;
    QString handle;

    ClientStack() = default;
    ClientStack(const ClientStack &) = delete;
    ClientStack &operator=(const ClientStack &) = delete;

    ~ClientStack()
    {
        if (client)
            client->disconnect();
        if (session)
            session->lock();
    }
};

} // namespace

class EndToEndTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void pipelineDeliversMessagesOverRealTls();
    void relockedProfileRelinksWithoutTokens();
    void voiceCallCarriesAudioOverRealTls();

private:
    void bootstrapClient(ClientStack &stack, const QString &handlePrefix);
    [[nodiscard]] RelayEndpoints proxyEndpoints() const;

    bool m_available = false;
    QString m_testDb = QStringLiteral("oc_relay_e2etest");
    PostgresStore::Config m_config;
    std::unique_ptr<PostgresStore> m_store;
    std::unique_ptr<AuthService> m_auth;
    std::unique_ptr<EnvelopeService> m_envelopes;
    std::unique_ptr<KeyPackageService> m_keyPackages;
    std::unique_ptr<DirectoryService> m_directory;
    std::unique_ptr<RelayServer> m_server;
    quint16 m_relayPort = 0;

    std::unique_ptr<RelayTest::CertAuthority> m_ca;
    std::unique_ptr<TlsProxy> m_proxy;
    quint16 m_proxyPort = 0;
};

void EndToEndTest::initTestCase()
{
    // Postgres connection contract mirrors tst_relayservices: env-driven host /
    // port / user, an admin database used only to (re)create a dedicated per-run
    // database so this test coexists with the service test's own database.
    m_config.host = qEnvironmentVariable("OPENCHAT_TEST_PG_HOST", QStringLiteral("127.0.0.1"));
    m_config.port = qEnvironmentVariableIntValue("OPENCHAT_TEST_PG_PORT");
    if (m_config.port == 0)
        m_config.port = 5432;
    m_config.user = qEnvironmentVariable("OPENCHAT_TEST_PG_USER", QStringLiteral("postgres"));
    const QString adminDb =
        qEnvironmentVariable("OPENCHAT_TEST_PG_ADMINDB", QStringLiteral("postgres"));

    PostgresStore::Config adminConfig = m_config;
    adminConfig.database = adminDb;
    QString error;
    auto admin = PostgresStore::open(adminConfig, QStringLiteral("e2e_admin"), &error);
    if (!admin) {
        qInfo() << "PostgreSQL unavailable:" << error;
        QSKIP("PostgreSQL not available for the E2E test");
    }
    {
        QSqlQuery drop(admin->database());
        drop.exec(QStringLiteral("DROP DATABASE IF EXISTS %1").arg(m_testDb));
        QSqlQuery create(admin->database());
        if (!create.exec(QStringLiteral("CREATE DATABASE %1").arg(m_testDb))) {
            const QString message = create.lastError().text();
            admin.reset();
            QSKIP(qPrintable(QStringLiteral("cannot create test db: %1").arg(message)));
        }
    }
    admin.reset();

    m_config.database = m_testDb;
    m_store = PostgresStore::open(m_config, QStringLiteral("e2e_main"), &error);
    QVERIFY2(m_store, qPrintable(error));
    // Deliberately NO setClock(): the relay must share the wall clock with the
    // clients so envelope validity/expiry windows agree with their timestamps.

    const QStringList migrations{QStringLiteral(":/relay/001_accounts_devices.sql"),
                                 QStringLiteral(":/relay/002_tokens_keypackages.sql"),
                                 QStringLiteral(":/relay/003_inboxes_attachments.sql"),
                                 QStringLiteral(":/relay/004_invites.sql")};
    QVERIFY2(m_store->applyMigrations(migrations, &error), qPrintable(error));

    // Service + server wiring order mirrors relay/src/main.cpp.
    m_auth = std::make_unique<AuthService>(*m_store);
    m_envelopes = std::make_unique<EnvelopeService>(*m_store);
    m_keyPackages = std::make_unique<KeyPackageService>(*m_store);
    m_directory = std::make_unique<DirectoryService>(*m_store);
    m_server = std::make_unique<RelayServer>(*m_store, *m_auth, *m_envelopes, *m_keyPackages,
                                             *m_directory);
    m_relayPort = m_server->start(QHostAddress::LocalHost, 0);
    QVERIFY(m_relayPort != 0);

    // Empirical-validation seam: the finding that the live socket cannot connect
    // without the subprotocol echo is proven by setting this env var, which
    // disables the injection and makes the isConnected() gate time out. Default
    // (unset) injects, matching the committed production Caddyfile.dev directive.
    const bool inject = !qEnvironmentVariableIsSet("OPENCHAT_E2E_NO_SUBPROTOCOL_INJECT");

    m_ca = std::make_unique<RelayTest::CertAuthority>();
    m_proxy = std::make_unique<TlsProxy>(RelayTest::serverConfig(m_ca->localhostLeaf()), m_relayPort,
                                         inject);
    QVERIFY(m_proxy->isListening());
    m_proxyPort = m_proxy->port();
    QVERIFY(m_proxyPort != 0);

    m_available = true;
}

void EndToEndTest::cleanupTestCase()
{
    // Strict teardown: the relay borrows the services and the store, and the
    // services borrow the store, so the relay is torn down first, then the
    // services, then the store; the TLS proxy (independent of all of them) is
    // released last. The per-run database is left in place, isolated by name.
    m_server.reset();
    m_directory.reset();
    m_keyPackages.reset();
    m_envelopes.reset();
    m_auth.reset();
    m_store.reset();
    m_proxy.reset();
    m_ca.reset();
}

RelayEndpoints EndToEndTest::proxyEndpoints() const
{
    const QString base = QStringLiteral("https://localhost:%1/v1").arg(m_proxyPort);
    RelayEndpoints endpoints;
    endpoints.accounts = QUrl(base + QStringLiteral("/accounts"));
    endpoints.authChallenge = QUrl(base + QStringLiteral("/auth/challenge"));
    endpoints.authComplete = QUrl(base + QStringLiteral("/auth/complete"));
    endpoints.authRefresh = QUrl(base + QStringLiteral("/auth/refresh"));
    endpoints.sync = QUrl(base + QStringLiteral("/sync"));
    endpoints.keyPackages = QUrl(base + QStringLiteral("/key-packages"));
    endpoints.keyPackagesClaim = QUrl(base + QStringLiteral("/key-packages/claim"));
    endpoints.directory = QUrl(base + QStringLiteral("/directory"));
    endpoints.directoryAccount = QUrl(base + QStringLiteral("/directory/account"));
    endpoints.invites = QUrl(base + QStringLiteral("/invites"));
    endpoints.invitesRedeem = QUrl(base + QStringLiteral("/invites/redeem"));
    endpoints.live = QUrl(QStringLiteral("wss://localhost:%1/v1/live").arg(m_proxyPort));
    return endpoints;
}

void EndToEndTest::bootstrapClient(ClientStack &stack, const QString &handlePrefix)
{
    QVERIFY(stack.dir.isValid());

    const auto profileId = ProfileId::generate();
    const auto paths = ProfilePaths::forProfile(stack.dir.path(), profileId);
    auto created = ProfileSession::create(profileId, stack.vault, paths);
    QVERIFY(created.hasValue());
    stack.session = std::move(created).value();

    const auto account = stack.session->accountId();
    QVERIFY(account.hasValue());
    const auto credential = stack.session->publicCredential();
    QVERIFY(credential.hasValue());
    stack.account = account.value();
    stack.deviceId = credential.value().deviceId;
    stack.signingPublicKey = credential.value().signingPublicKey;

    stack.client = std::make_unique<RelayClient>(stack.deviceId, stack.account, proxyEndpoints(),
                                                 RelayCredentials{});
    stack.client->setTlsConfiguration(RelayTest::clientConfigTrusting(m_ca->caCertPem()));
    stack.transport = std::make_unique<RelayTransport>(*stack.client);

    stack.handle =
        handlePrefix + QStringLiteral("-") + QUuid::createUuid().toString(QUuid::Id128).toLower();

    AccountBootstrap bootstrap(*stack.session, *stack.client, *stack.transport);
    bool succeeded = false;
    std::optional<AccountBootstrap::Error> failure;
    connect(&bootstrap, &AccountBootstrap::succeeded, this, [&] { succeeded = true; });
    connect(&bootstrap, &AccountBootstrap::failed, this,
            [&](AccountBootstrap::Error error) { failure = error; });

    bootstrap.start(stack.handle, 4);
    QTRY_VERIFY_WITH_TIMEOUT(succeeded || failure.has_value(), 30000);
    if (failure.has_value())
        QFAIL(qPrintable(
            QStringLiteral("bootstrap failed with error %1").arg(static_cast<int>(*failure))));
    QVERIFY(succeeded);
}

void EndToEndTest::pipelineDeliversMessagesOverRealTls()
{
    if (!m_available)
        QSKIP("PostgreSQL not available for the E2E test");

    ClientStack alice;
    ClientStack bob;

    // --- Bootstrap both accounts over the TLS proxy: register, authenticate,
    //     publish 4 KeyPackages each, then open the live stream. ---
    bootstrapClient(alice, QStringLiteral("alice"));
    if (QTest::currentTestFailed())
        return;
    bootstrapClient(bob, QStringLiteral("bob"));
    if (QTest::currentTestFailed())
        return;

    // --- Gate: both live sockets negotiated the subprotocol and connected. This
    //     is exactly where the Sec-WebSocket-Protocol edge injection is proven;
    //     without it isConnected() never becomes true and this times out. ---
    QTRY_VERIFY_WITH_TIMEOUT(alice.client->isConnected() && bob.client->isConnected(), 30000);

    // --- Wire the receive-side and send-side contact services. ---
    ContactRequestService bobRequests(*bob.session, *bob.session->syncEngine());
    std::optional<AccountId> bobIncomingSender;
    std::optional<ConversationId> bobIncomingConversation;
    connect(&bobRequests, &ContactRequestService::incomingRequest, this,
            [&](const AccountId &sender, const ConversationId &conversation) {
                bobIncomingSender = sender;
                bobIncomingConversation = conversation;
            });
    std::optional<AccountId> bobAccepted;
    bool bobSecurityNotice = false;
    connect(&bobRequests, &ContactRequestService::contactAccepted, this,
            [&](const AccountId &sender) { bobAccepted = sender; });
    connect(&bobRequests, &ContactRequestService::securityNotice, this,
            [&](const ConversationId &, const AccountId &) { bobSecurityNotice = true; });

    // Alice's own request service closes the loop when Bob accepts.
    ContactRequestService aliceRequests(*alice.session, *alice.session->syncEngine());
    std::optional<AccountId> aliceAccepted;
    connect(&aliceRequests, &ContactRequestService::contactAccepted, this,
            [&](const AccountId &peer) { aliceAccepted = peer; });

    AddContactService aliceAdd(*alice.session, *alice.client, *alice.session->syncEngine());
    std::optional<ConversationId> aliceConversation;
    std::optional<AccountId> alicePeer;
    std::optional<AddContactService::Error> aliceAddFailure;
    connect(&aliceAdd, &AddContactService::succeeded, this,
            [&](const ConversationId &conversation, const AccountId &peer) {
                aliceConversation = conversation;
                alicePeer = peer;
            });
    connect(&aliceAdd, &AddContactService::failed, this,
            [&](AddContactService::Error error) { aliceAddFailure = error; });

    // --- Alice adds Bob by handle: real directory resolution + KeyPackage claim
    //     over TLS, then an MLS Welcome shipped over the live stream to Bob. ---
    aliceAdd.startByHandle(bob.handle);
    QTRY_VERIFY_WITH_TIMEOUT(aliceConversation.has_value() || aliceAddFailure.has_value(), 30000);
    QVERIFY(!aliceAddFailure.has_value());
    QVERIFY(aliceConversation.has_value());
    QVERIFY(alicePeer.has_value());
    QCOMPARE(alicePeer->bytes(), bob.account.bytes());
    const ConversationId conversation = *aliceConversation;

    // Bob's live client received the Welcome, stashed it, and surfaced a request
    // naming Alice and the sender-chosen conversation.
    QTRY_VERIFY_WITH_TIMEOUT(bobIncomingSender.has_value(), 30000);
    QCOMPARE(bobIncomingSender->bytes(), alice.account.bytes());
    QVERIFY(bobIncomingConversation.has_value());
    QCOMPARE(bobIncomingConversation->bytes(), conversation.bytes());

    // Alice's roster: a PendingOutgoing row bound to the conversation, carrying
    // Bob's MLS-authenticated signing key from the claimed KeyPackage credential.
    {
        auto found = alice.session->contacts()->find(bob.account);
        QVERIFY(found.hasValue());
        QVERIFY(found.value().has_value());
        QCOMPARE(found.value()->state, ContactState::PendingOutgoing);
        QVERIFY(found.value()->conversationId.has_value());
        QCOMPARE(found.value()->conversationId->bytes(), conversation.bytes());
        QVERIFY(found.value()->peerSigningKey.has_value());
        QCOMPARE(*found.value()->peerSigningKey, bob.signingPublicKey);
    }

    // --- Bob accepts: authenticate the Welcome, join, and commit atomically. ---
    bobRequests.acceptContact(conversation);
    QTRY_VERIFY_WITH_TIMEOUT(bobAccepted.has_value(), 30000);
    QVERIFY(!bobSecurityNotice);
    QCOMPARE(bobAccepted->bytes(), alice.account.bytes());
    {
        auto found = bob.session->contacts()->find(alice.account);
        QVERIFY(found.hasValue());
        QVERIFY(found.value().has_value());
        QCOMPARE(found.value()->state, ContactState::Accepted);
        QVERIFY(found.value()->peerSigningKey.has_value());
        QCOMPARE(*found.value()->peerSigningKey, alice.signingPublicKey);
    }

    // --- Bob's acceptance travels back to Alice as a ContactAccept control
    //     message through the relay: her PendingOutgoing row becomes Accepted,
    //     and both sides now hold the durable conversation row (created by the
    //     services, not by this test). ---
    QTRY_VERIFY_WITH_TIMEOUT(aliceAccepted.has_value(), 30000);
    QCOMPARE(aliceAccepted->bytes(), bob.account.bytes());
    {
        auto found = alice.session->contacts()->find(bob.account);
        QVERIFY(found.hasValue());
        QVERIFY(found.value().has_value());
        QCOMPARE(found.value()->state, ContactState::Accepted);
        QVERIFY(found.value()->peerDeviceId.has_value());
        QCOMPARE(found.value()->peerDeviceId->bytes(), bob.deviceId.bytes());
    }
    for (ClientStack *stack : {&alice, &bob}) {
        auto rows = stack->session->chats()->conversations();
        QVERIFY(rows.hasValue());
        bool present = false;
        for (const ConversationRecord &record : rows.value())
            present = present || record.id == conversation;
        QVERIFY(present);
    }

    // --- The chat surface on both sides, exactly as the app wires it: Bob's
    //     accepted contact is his chat; Alice's accepted request is hers. ---
    ChatController aliceChat;
    aliceChat.setLiveServices(alice.session.get(), alice.session->syncEngine(), &aliceRequests);
    ChatController bobChat;
    bobChat.setLiveServices(bob.session.get(), bob.session->syncEngine(), &bobRequests);
    QCOMPARE(aliceChat.contacts()->rowCount(), 1);
    QCOMPARE(bobChat.contacts()->rowCount(), 1);
    QCOMPARE(aliceChat.currentContactName(), bob.handle); // typed at add time
    QVERIFY(aliceChat.hasCurrentContact());
    QVERIFY(bobChat.hasCurrentContact());

    // --- Alice -> Bob application message over the live stream, sent from the
    //     composer and received into Bob's open conversation. ---
    std::optional<MessageRecord> bobReceived;
    connect(bob.session->syncEngine(), &SyncEngine::messageReceived, this,
            [&](const MessageRecord &message) { bobReceived = message; });
    aliceChat.setComposerText(QStringLiteral("hello from Alice"));
    QVERIFY(aliceChat.canSend());
    QVERIFY(aliceChat.sendMessage());
    QCOMPARE(aliceChat.messages()->rowCount(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(bobReceived.has_value(), 30000);
    QCOMPARE(bobReceived->body, QStringLiteral("hello from Alice"));
    QCOMPARE(bobReceived->flow, MessageFlow::Incoming);
    QCOMPARE(bobReceived->conversationId.bytes(), conversation.bytes());
    QCOMPARE(bobChat.messages()->rowCount(), 1);
    QCOMPARE(bobChat.messages()->data(bobChat.messages()->index(0), MessageListModel::BodyRole)
                 .toString(),
             QStringLiteral("hello from Alice"));
    // Relay acceptance advanced Alice's visible row past Queued.
    QTRY_VERIFY_WITH_TIMEOUT(
        aliceChat.messages()->data(aliceChat.messages()->index(0),
                                   MessageListModel::DeliveryStateRole).toInt()
            == static_cast<int>(MessageDeliveryState::Sent),
        30000);

    // --- Bob -> Alice application message, symmetric. ---
    std::optional<MessageRecord> aliceReceived;
    connect(alice.session->syncEngine(), &SyncEngine::messageReceived, this,
            [&](const MessageRecord &message) { aliceReceived = message; });
    bobChat.setComposerText(QStringLiteral("hi back from Bob"));
    QVERIFY(bobChat.sendMessage());
    QTRY_VERIFY_WITH_TIMEOUT(aliceReceived.has_value(), 30000);
    QCOMPARE(aliceReceived->body, QStringLiteral("hi back from Bob"));
    QCOMPARE(aliceReceived->flow, MessageFlow::Incoming);
    QCOMPARE(aliceReceived->conversationId.bytes(), conversation.bytes());
    QCOMPARE(aliceChat.messages()->rowCount(), 2);

    // --- Reverse directory lookup: Bob learns Alice's handle from her id. ---
    std::optional<QString> resolvedHandle;
    connect(bob.client.get(), &RelayClient::accountResolved, this,
            [&](const AccountId &account, const QString &handle) {
                if (account == alice.account)
                    resolvedHandle = handle;
            });
    bob.client->resolveAccount(alice.account);
    QTRY_VERIFY_WITH_TIMEOUT(resolvedHandle.has_value(), 30000);
    QCOMPARE(*resolvedHandle, alice.handle);

    // Neither engine tripped its fail-closed guard along the way.
    QVERIFY(!alice.session->syncEngine()->isFailedClosed());
    QVERIFY(!bob.session->syncEngine()->isFailedClosed());

    // --- Safety numbers: both sides derive the same 60-digit code from the two
    //     authenticated signing keys and account ids, and each side's bound peer
    //     key is exactly the other's real signing key. ---
    auto aliceFound = alice.session->contacts()->find(bob.account);
    QVERIFY(aliceFound.hasValue());
    QVERIFY(aliceFound.value().has_value());
    const ContactRecord aliceRecord = *aliceFound.value();
    auto bobFound = bob.session->contacts()->find(alice.account);
    QVERIFY(bobFound.hasValue());
    QVERIFY(bobFound.value().has_value());
    const ContactRecord bobRecord = *bobFound.value();

    QVERIFY(aliceRecord.peerSigningKey.has_value());
    QVERIFY(bobRecord.peerSigningKey.has_value());
    QCOMPARE(*aliceRecord.peerSigningKey, bob.signingPublicKey);
    QCOMPARE(*bobRecord.peerSigningKey, alice.signingPublicKey);

    const auto aliceSafety = computeSafetyNumber(alice.signingPublicKey, alice.account.bytes(),
                                                 *aliceRecord.peerSigningKey, bob.account.bytes());
    const auto bobSafety = computeSafetyNumber(bob.signingPublicKey, bob.account.bytes(),
                                               *bobRecord.peerSigningKey, alice.account.bytes());
    QVERIFY(aliceSafety.hasValue());
    QVERIFY(bobSafety.hasValue());
    QCOMPARE(aliceSafety.value().size(), qsizetype(60));
    QCOMPARE(bobSafety.value().size(), qsizetype(60));
    QCOMPARE(aliceSafety.value(), bobSafety.value());

    // --- The relay really persisted both accounts and 4 KeyPackages each. ---
    {
        QSqlQuery accounts(m_store->database());
        accounts.prepare(QStringLiteral("SELECT count(*) FROM accounts WHERE handle = :handle"));
        accounts.bindValue(QStringLiteral(":handle"), alice.handle);
        QVERIFY(accounts.exec());
        QVERIFY(accounts.next());
        QCOMPARE(accounts.value(0).toInt(), 1);
        accounts.bindValue(QStringLiteral(":handle"), bob.handle);
        QVERIFY(accounts.exec());
        QVERIFY(accounts.next());
        QCOMPARE(accounts.value(0).toInt(), 1);
    }
    {
        QSqlQuery packages(m_store->database());
        packages.prepare(
            QStringLiteral("SELECT count(*) FROM key_packages WHERE device_id = :device"));
        packages.bindValue(QStringLiteral(":device"), alice.deviceId.bytes());
        QVERIFY(packages.exec());
        QVERIFY(packages.next());
        QCOMPARE(packages.value(0).toInt(), 4);
        packages.bindValue(QStringLiteral(":device"), bob.deviceId.bytes());
        QVERIFY(packages.exec());
        QVERIFY(packages.next());
        QCOMPARE(packages.value(0).toInt(), 4);
    }

    // Explicit networking teardown before the stacks unwind (the ClientStack
    // destructor is the backstop): stop the live sockets, then lock the sessions.
    alice.client->disconnect();
    bob.client->disconnect();
    alice.session->lock();
    bob.session->lock();
}

void EndToEndTest::relockedProfileRelinksWithoutTokens()
{
    if (!m_available)
        QSKIP("PostgreSQL not available for the E2E test");

    // A registered account, then the exact app-restart sequence: the live
    // socket closes, the session locks, and the profile is unlocked again with a
    // brand-new RelayClient holding no tokens at all.
    ClientStack carol;
    bootstrapClient(carol, QStringLiteral("carol"));
    if (QTest::currentTestFailed())
        return;
    QTRY_VERIFY_WITH_TIMEOUT(carol.client->isConnected(), 30000);
    const auto profileId = ProfileId::fromBytes(
        QByteArray::fromHex(QDir(carol.dir.path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot)
                                .constFirst()
                                .toLatin1()));
    QVERIFY(profileId.has_value());

    carol.client->disconnect();
    carol.session->lock();
    carol.transport.reset();
    carol.client.reset();

    auto unlocked = ProfileSession::unlock(*profileId, carol.vault,
                                           ProfilePaths::forProfile(carol.dir.path(), *profileId));
    QVERIFY(unlocked.hasValue());
    carol.session = std::move(unlocked).value();
    carol.client = std::make_unique<RelayClient>(carol.deviceId, carol.account, proxyEndpoints(),
                                                 RelayCredentials{});
    carol.client->setTlsConfiguration(RelayTest::clientConfigTrusting(m_ca->caCertPem()));
    carol.transport = std::make_unique<RelayTransport>(*carol.client);
    QVERIFY(carol.session->startNetworking(*carol.transport).hasValue());
    QVERIFY(!carol.client->isConnected());

    // The device link runs the challenge/response with the stored identity,
    // installs the fresh tokens and reopens the live stream.
    DeviceLink link(*carol.session, *carol.client);
    int linked = 0;
    int failed = 0;
    connect(&link, &DeviceLink::linked, this, [&] { ++linked; });
    connect(&link, &DeviceLink::authenticationFailed, this, [&] { ++failed; });
    link.start(DeviceLink::Start::NeedsAuthentication);
    QTRY_VERIFY_WITH_TIMEOUT(linked == 1, 30000);
    QCOMPARE(failed, 0);
    QVERIFY(link.isAuthenticated());
    QTRY_VERIFY_WITH_TIMEOUT(carol.client->isConnected(), 30000);

    // The new tokens authorize real calls: an exact-handle lookup resolves.
    std::optional<AccountId> resolved;
    connect(carol.client.get(), &RelayClient::handleResolved, this,
            [&](const RelayDirectoryEntry &entry) { resolved = entry.accountId; });
    carol.client->resolveHandle(carol.handle);
    QTRY_VERIFY_WITH_TIMEOUT(resolved.has_value(), 30000);
    QCOMPARE(resolved->bytes(), carol.account.bytes());

    carol.client->disconnect();
    carol.session->lock();
}

void EndToEndTest::voiceCallCarriesAudioOverRealTls()
{
    if (!m_available)
        QSKIP("PostgreSQL not available for the E2E test");

    // A whole voice call over the real stack: two bootstrapped profiles, real
    // TLS, a real relay with real Postgres behind it, MLS-encrypted call
    // signalling on the durable path and sealed audio frames on the unreliable
    // datagram path the relay forwards without storing.
    //
    // The audio pushed in is a file from this machine, and what comes out of the
    // far end's speaker is compared against it. Nothing here is simulated except
    // the microphone and the speaker themselves.
    ClientStack alice;
    ClientStack bob;
    bootstrapClient(alice, QStringLiteral("callera"));
    if (QTest::currentTestFailed())
        return;
    bootstrapClient(bob, QStringLiteral("callerb"));
    if (QTest::currentTestFailed())
        return;
    QTRY_VERIFY_WITH_TIMEOUT(alice.client->isConnected() && bob.client->isConnected(), 30000);

    // --- Become contacts, which is what gives the call an MLS group to signal
    //     through and a peer device to address. ---
    ContactRequestService aliceRequests(*alice.session, *alice.session->syncEngine());
    ContactRequestService bobRequests(*bob.session, *bob.session->syncEngine());
    std::optional<ConversationId> bobIncoming;
    connect(&bobRequests, &ContactRequestService::incomingRequest, this,
            [&](const AccountId &, const ConversationId &conversation) {
                bobIncoming = conversation;
            });
    bool aliceAccepted = false;
    connect(&aliceRequests, &ContactRequestService::contactAccepted, this,
            [&](const AccountId &) { aliceAccepted = true; });

    AddContactService aliceAdd(*alice.session, *alice.client, *alice.session->syncEngine());
    std::optional<ConversationId> aliceConversation;
    connect(&aliceAdd, &AddContactService::succeeded, this,
            [&](const ConversationId &conversation, const AccountId &) {
                aliceConversation = conversation;
            });
    aliceAdd.startByHandle(bob.handle);
    QTRY_VERIFY_WITH_TIMEOUT(aliceConversation.has_value(), 30000);
    QTRY_VERIFY_WITH_TIMEOUT(bobIncoming.has_value(), 30000);
    bobRequests.acceptContact(*bobIncoming);
    QTRY_VERIFY_WITH_TIMEOUT(aliceAccepted, 30000);

    // --- Bring up the call stack on both sides exactly as the app does. ---
    SyncCallTransport aliceCallTransport(*alice.session->syncEngine());
    SyncCallTransport bobCallTransport(*bob.session->syncEngine());
    OpenChat::CallTest::ScriptedAudioDevices aliceDevices;
    OpenChat::CallTest::ScriptedAudioDevices bobDevices;
    // The lossless codec, so "the same sound came out the other end" can be
    // checked as an exact comparison rather than as a similarity score.
    CallEngine::Config callConfig;
    callConfig.preferredCodec = AudioCodecKind::Pcm;
    CallEngine aliceCall(callConfig, aliceCallTransport, aliceDevices.factory());
    CallEngine bobCall(callConfig, bobCallTransport, bobDevices.factory());
    // The call's own ring and pick-up tones mix into the same playback stream by
    // design, which would make "what came out the far end" un-comparable. This
    // test is about the transport carrying audio; the sounds have their own.
    aliceCall.setSoundsEnabled(false);
    bobCall.setSoundsEnabled(false);

    bool bobRinging = false;
    connect(&bobCall, &CallEngine::incomingCall, this, [&] { bobRinging = true; });

    CallEngine::CallPeer peer;
    peer.conversation = *aliceConversation;
    peer.device = bob.deviceId;
    peer.displayName = bob.handle;
    QVERIFY(aliceCall.placeCall(peer));

    // The offer crossed the relay as an MLS-encrypted control message and rang
    // Bob's device.
    QTRY_VERIFY_WITH_TIMEOUT(bobRinging, 30000);
    QCOMPARE(bobCall.direction(), CallDirection::Incoming);
    QCOMPARE(bobCall.peer().device.bytes(), alice.deviceId.bytes());
    QTRY_COMPARE_WITH_TIMEOUT(aliceCall.state(), CallState::Ringing, 30000);

    bobCall.acceptCall();
    QTRY_COMPARE_WITH_TIMEOUT(aliceCall.state(), CallState::Connecting, 30000);
    QVERIFY(aliceDevices.capture->started);
    QVERIFY(bobDevices.playback->started);

    // --- Push real audio through: a file from this machine if there is one,
    //     otherwise a synthesised reference. ---
    const std::optional<OpenChat::AudioTest::LoadedWav> file =
        OpenChat::AudioTest::loadFirstSystemWav();
    const QVector<qint16> source = file ? AudioConvert::toCallFormat(file->audio)
                                        : OpenChat::AudioTest::syntheticSpeech(600);
    QVERIFY(!source.isEmpty());
    const QList<AudioFrame> spoken = AudioConvert::toFrames(source);

    QList<AudioFrame> heard;
    bool bobHeardAliceTalking = false;
    for (const AudioFrame &frame : spoken) {
        aliceDevices.speak(frame);
        // Bob's microphone runs too, so Alice's end also sees media arrive and
        // both ends reach Active, exactly as in a real call.
        bobDevices.speak(silentAudioFrame());
        // Let the relay actually carry the frames: this is a real socket.
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        heard.append(bobDevices.listen());
        (void)aliceDevices.listen();
        // Sampled while the audio is playing: the indicator is supposed to fall
        // away once the talking stops, so checking it afterwards would prove the
        // opposite of what it is for.
        bobHeardAliceTalking = bobHeardAliceTalking || bobCall.isRemoteSpeaking();
    }
    // Drain whatever is still in flight or still buffered.
    for (int i = 0; i < 40; ++i) {
        QTest::qWait(2);
        heard.append(bobDevices.listen());
    }

    QTRY_COMPARE_WITH_TIMEOUT(bobCall.state(), CallState::Active, 30000);
    QCOMPARE(aliceCall.state(), CallState::Active);

    // What Bob's speaker rendered contains Alice's audio, in order and byte for
    // byte. A frame is only compared once, so a duplicate or a reordering would
    // break the walk rather than pass it.
    int matched = 0;
    for (const AudioFrame &frame : heard) {
        QVERIFY(isFullAudioFrame(frame));
        if (matched < spoken.size() && frame == spoken.at(matched))
            ++matched;
    }
    // The datagram path is best-effort by design, so a frame may legitimately be
    // dropped; requiring the overwhelming majority proves the path works while
    // leaving room for the one the network is entitled to lose.
    QVERIFY2(matched >= spoken.size() * 9 / 10,
             qPrintable(QStringLiteral("only %1 of %2 frames arrived intact")
                            .arg(matched)
                            .arg(spoken.size())));
    // Bob's end could tell Alice was the one talking, and Bob's own silent
    // microphone never claimed he was.
    QVERIFY2(bobHeardAliceTalking, "Bob's end never registered Alice as speaking");
    QVERIFY(!bobCall.isLocalSpeaking());
    // And the indicator falls away again now that the audio has stopped, rather
    // than latching on for the rest of the call.
    QVERIFY(!bobCall.isRemoteSpeaking());

    // Media rode the unreliable path, so none of it was stored in anyone's inbox
    // and none of it became a message row.
    {
        QSqlQuery inbox(m_store->database());
        inbox.prepare(QStringLiteral(
            "SELECT count(*) FROM inbox_messages WHERE recipient_device_id = :device"));
        inbox.bindValue(QStringLiteral(":device"), bob.deviceId.bytes());
        QVERIFY(inbox.exec());
        QVERIFY(inbox.next());
        // Whatever is left is signalling and handshake traffic awaiting an ack;
        // a stored media frame per 20 ms would put this in the hundreds.
        QVERIFY2(inbox.value(0).toInt() < 20,
                 qPrintable(QStringLiteral("the relay stored %1 envelopes for Bob; media must "
                                           "not be persisted")
                                .arg(inbox.value(0).toInt())));
    }
    auto bobMessages = bob.session->chats()->messages(*bobIncoming, 100, std::nullopt);
    QVERIFY(bobMessages.hasValue());
    QCOMPARE(bobMessages.value().size(), 0);

    // --- Hanging up tears the call down on both sides through the relay. ---
    aliceCall.hangUp();
    QCOMPARE(aliceCall.state(), CallState::Ended);
    QTRY_COMPARE_WITH_TIMEOUT(bobCall.state(), CallState::Ended, 30000);
    QCOMPARE(bobCall.endReason(), CallEndReason::RemoteHangup);
    QVERIFY(!aliceDevices.capture->started);
    QVERIFY(!bobDevices.capture->started);

    alice.client->disconnect();
    bob.client->disconnect();
    alice.session->lock();
    bob.session->lock();
}

QTEST_GUILESS_MAIN(EndToEndTest)
#include "tst_e2e.moc"
