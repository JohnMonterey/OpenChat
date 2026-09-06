#include "app/AddContactService.h"
#include "app/KeyPackageSupply.h"
#include "relay/RelayTestSupport.h"
#include <QCborMap>
#include <QSignalSpy>

#include "app/ProfileSession.h"
#include "crypto/MlsClient.h"
#include "domain/Contact.h"
#include "network/RelayClient.h"
#include "network/SyncEngine.h"
#include "protocol/CanonicalCborCodec.h"
#include "protocol/CiphertextEnvelope.h"
#include "security/DeviceIdentity.h"
#include "security/KeyVault.h"
#include "security/SecureBuffer.h"
#include "storage/CapturingMlsStateStore.h"
#include "storage/SqlCipherContactRepository.h"
#include "storage/SqlCipherDatabase.h"

#include "RelayCrypto.h"

#include <QDateTime>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest/QTest>

#include <memory>
#include <optional>

using namespace OpenChat;

namespace {

// Minimal in-memory KeyVault: this environment has no OS keychain, so the
// profile's database and wrapping keys live only for the test. Mirrors the fake
// vault used by tst_profilesession / tst_accountbootstrap.
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

// A connected ciphertext transport that captures whatever the engine sends. The
// handshake send drains inline while connected, so the Welcome envelope is
// available immediately after the service completes.
class CapturingTransport final : public SyncTransport
{
public:
    bool isConnected() const override { return true; }
    void sendEnvelope(const CiphertextEnvelopeV1 &envelope) override { sent.append(envelope); }
    void sendDatagram(const CiphertextEnvelopeV1 &envelope) override { datagrams.append(envelope); }
    void acknowledge(const EnvelopeId &, quint64) override { }

    QVector<CiphertextEnvelopeV1> sent;
    QVector<CiphertextEnvelopeV1> datagrams;
};

RelayDirectoryDevice device(const DeviceId &deviceId)
{
    return RelayDirectoryDevice{deviceId, QByteArray(32, 'k')};
}

} // namespace

class AddContactServiceTest final : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void addByHandleSendsWelcomeAndRecordsPendingOutgoing();
    void addByInviteSucceeds();
    void selfContactFailsWithoutSending();
    void emptyDeviceListFailsNoDevice();
    void keyPackageUnavailableFallsThroughToNextDevice();
    void keyPackageExhaustedFailsNoKeyPackage();
    void eightPackagePoolExhaustion();
    void emptyPoolRecoversAndPrivateKeysSurviveRestart();
    void supplyStopsAtThresholdAndRecoversAfterFailure();
    void blockedPeerFailsBlockedAndSendsNothing();
    void handleNotFoundFails();

private:
    // Fresh fixture per test slot (QtTest init()/cleanup()).
    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<InMemoryVault> m_vault;
    std::unique_ptr<ProfileSession> m_session;
    std::unique_ptr<CapturingTransport> m_transport;
    std::unique_ptr<RelayClient> m_relay;
    std::unique_ptr<SqlCipherDatabase> m_peerDb;
    std::unique_ptr<CapturingMlsStateStore> m_peerCapture;
    std::unique_ptr<MlsClient> m_peer;
    QByteArray m_keyPackage;
    ProfileId m_profileId = ProfileId::generate();
};

void AddContactServiceTest::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_vault = std::make_unique<InMemoryVault>();

    const auto profileId = m_profileId;
    const auto paths = ProfilePaths::forProfile(m_dir->path(), profileId);
    auto created = ProfileSession::create(profileId, *m_vault, paths);
    QVERIFY(created.hasValue());
    m_session = std::move(created).value();

    m_transport = std::make_unique<CapturingTransport>();
    QVERIFY(m_session->startNetworking(*m_transport).hasValue());
    QVERIFY(m_session->syncEngine() != nullptr);

    // Secure but unreachable endpoints: resolveHandle/redeemInvite/claimKeyPackage
    // fire real async HTTPS requests that never complete during the test (it never
    // spins the loop). The state machine is driven deterministically by emitting
    // the relay's signals directly, the established pattern in tst_syncadapters.
    RelayEndpoints endpoints;
    endpoints.directory = QUrl(QStringLiteral("https://127.0.0.1:1/directory"));
    endpoints.invitesRedeem = QUrl(QStringLiteral("https://127.0.0.1:1/invites/redeem"));
    endpoints.keyPackagesClaim = QUrl(QStringLiteral("https://127.0.0.1:1/key-packages/claim"));
    RelayCredentials credentials;
    credentials.accessToken = [] { return QByteArray("access"); };
    credentials.refreshToken = [] { return QByteArray("refresh"); };
    m_relay = std::make_unique<RelayClient>(DeviceId::generate(), AccountId::generate(), endpoints,
                                            credentials);

    // A second MLS client stands in for the peer; its real KeyPackage is what the
    // service adds to the group, and it can join the produced Welcome.
    auto peerOpened = SqlCipherDatabase::open(m_dir->filePath(QStringLiteral("peer.sqlite3")),
                                              SecureBuffer::random(32));
    QVERIFY(peerOpened.hasValue());
    m_peerDb = std::make_unique<SqlCipherDatabase>(std::move(peerOpened).value());
    m_peerCapture = std::make_unique<CapturingMlsStateStore>(*m_peerDb, ProfileId::generate());
    auto peerResult = MlsClient::create(QByteArray("peer-device"), m_peerCapture.get());
    QVERIFY(peerResult.hasValue());
    m_peer = std::move(peerResult).value();
    auto keyPackage = m_peer->generateKeyPackage();
    QVERIFY(keyPackage.hasValue());
    m_keyPackage = keyPackage.value();
}

void AddContactServiceTest::cleanup()
{
    // Order: the session's engine borrows the transport, so tear the session down
    // (which stops the engine) before the transport, and keep the vault last since
    // the session borrows it.
    if (m_session)
        m_session->lock();
    m_session.reset();
    m_transport.reset();
    m_relay.reset();
    m_peer.reset();
    m_peerCapture.reset();
    m_peerDb.reset();
    m_vault.reset();
    m_dir.reset();
    m_keyPackage.clear();
}

void AddContactServiceTest::addByHandleSendsWelcomeAndRecordsPendingOutgoing()
{
    const AccountId peerAccount = AccountId::generate();
    const DeviceId peerDevice = DeviceId::generate();
    const RelayDirectoryEntry entry{peerAccount, {device(peerDevice)}};

    AddContactService service(*m_session, *m_relay, *m_session->syncEngine());
    std::optional<ConversationId> conversation;
    std::optional<AccountId> peer;
    std::optional<AddContactService::Error> failure;
    connect(&service, &AddContactService::succeeded, this,
            [&](const ConversationId &c, const AccountId &p) {
                conversation = c;
                peer = p;
            });
    connect(&service, &AddContactService::failed, this,
            [&](AddContactService::Error e) { failure = e; });

    service.startByHandle(QStringLiteral("alice"));
    emit m_relay->handleResolved(entry);
    emit m_relay->keyPackageClaimed(m_keyPackage);

    QVERIFY(!failure.has_value());
    QVERIFY(conversation.has_value());
    QVERIFY(peer.has_value());
    QCOMPARE(peer->bytes(), peerAccount.bytes());

    // The roster carries a PendingOutgoing row bound to the new conversation.
    auto found = m_session->contacts()->find(peerAccount);
    QVERIFY(found.hasValue());
    QVERIFY(found.value().has_value());
    QCOMPARE(found.value()->state, ContactState::PendingOutgoing);
    QCOMPARE(found.value()->handle, QStringLiteral("alice"));
    QVERIFY(found.value()->conversationId.has_value());
    QCOMPARE(found.value()->conversationId->bytes(), conversation->bytes());

    // Exactly one MlsHandshake envelope left the device, addressed to the peer's
    // device, carrying the Welcome verbatim.
    QCOMPARE(m_transport->sent.size(), qsizetype(1));
    const CiphertextEnvelopeV1 sent = m_transport->sent.first();
    QCOMPARE(sent.messageKind, EnvelopeMessageKind::MlsHandshake);
    QCOMPARE(sent.recipientDeviceId.bytes(), peerDevice.bytes());
    QCOMPARE(sent.conversationId.bytes(), conversation->bytes());
    QVERIFY(!sent.ciphertext.isEmpty());

    // The ciphertext is a real MLS Welcome sealed to the peer's KeyPackage: the
    // peer joins the group with it.
    QVERIFY(m_peer->joinGroup(*conversation, sent.ciphertext).hasValue());

    // The envelope is Ed25519-signed by our device over the canonical signing
    // input, and verifies with the relay's real verifier.
    const auto credential = m_session->publicCredential();
    QVERIFY(credential.hasValue());
    const QByteArray signingInput = encodeForSignature(sent);
    QVERIFY(Relay::verifyEd25519(credential.value().signingPublicKey, signingInput,
                                 sent.senderSignature));
}

void AddContactServiceTest::addByInviteSucceeds()
{
    const AccountId peerAccount = AccountId::generate();
    const DeviceId peerDevice = DeviceId::generate();
    const RelayDirectoryEntry entry{peerAccount, {device(peerDevice)}};

    AddContactService service(*m_session, *m_relay, *m_session->syncEngine());
    std::optional<ConversationId> conversation;
    std::optional<AddContactService::Error> failure;
    connect(&service, &AddContactService::succeeded, this,
            [&](const ConversationId &c, const AccountId &) { conversation = c; });
    connect(&service, &AddContactService::failed, this,
            [&](AddContactService::Error e) { failure = e; });

    service.startByInvite(QByteArray("one-time-invite"));
    emit m_relay->inviteRedeemed(entry);
    emit m_relay->keyPackageClaimed(m_keyPackage);

    QVERIFY(!failure.has_value());
    QVERIFY(conversation.has_value());

    // The invite path stamps an empty handle but still records the peer.
    auto found = m_session->contacts()->find(peerAccount);
    QVERIFY(found.hasValue());
    QVERIFY(found.value().has_value());
    QCOMPARE(found.value()->state, ContactState::PendingOutgoing);
    QVERIFY(found.value()->handle.isEmpty());

    QCOMPARE(m_transport->sent.size(), qsizetype(1));
    QCOMPARE(m_transport->sent.first().messageKind, EnvelopeMessageKind::MlsHandshake);
    QVERIFY(m_peer->joinGroup(*conversation, m_transport->sent.first().ciphertext).hasValue());
}

void AddContactServiceTest::selfContactFailsWithoutSending()
{
    const auto ourAccount = m_session->accountId();
    QVERIFY(ourAccount.hasValue());
    const RelayDirectoryEntry entry{ourAccount.value(), {device(DeviceId::generate())}};

    AddContactService service(*m_session, *m_relay, *m_session->syncEngine());
    std::optional<AddContactService::Error> failure;
    bool succeeded = false;
    connect(&service, &AddContactService::succeeded, this,
            [&](const ConversationId &, const AccountId &) { succeeded = true; });
    connect(&service, &AddContactService::failed, this,
            [&](AddContactService::Error e) { failure = e; });

    service.startByHandle(QStringLiteral("myself"));
    emit m_relay->handleResolved(entry);

    QVERIFY(!succeeded);
    QVERIFY(failure.has_value());
    QCOMPARE(failure.value(), AddContactService::Error::SelfContact);
    QCOMPARE(m_transport->sent.size(), qsizetype(0));
}

void AddContactServiceTest::emptyDeviceListFailsNoDevice()
{
    const RelayDirectoryEntry entry{AccountId::generate(), {}};

    AddContactService service(*m_session, *m_relay, *m_session->syncEngine());
    std::optional<AddContactService::Error> failure;
    connect(&service, &AddContactService::failed, this,
            [&](AddContactService::Error e) { failure = e; });

    service.startByHandle(QStringLiteral("deviceless"));
    emit m_relay->handleResolved(entry);

    QVERIFY(failure.has_value());
    QCOMPARE(failure.value(), AddContactService::Error::NoDevice);
    QCOMPARE(m_transport->sent.size(), qsizetype(0));
}

void AddContactServiceTest::keyPackageUnavailableFallsThroughToNextDevice()
{
    const AccountId peerAccount = AccountId::generate();
    const DeviceId firstDevice = DeviceId::generate();
    const DeviceId secondDevice = DeviceId::generate();
    const RelayDirectoryEntry entry{peerAccount, {device(firstDevice), device(secondDevice)}};

    AddContactService service(*m_session, *m_relay, *m_session->syncEngine());
    std::optional<ConversationId> conversation;
    std::optional<AddContactService::Error> failure;
    connect(&service, &AddContactService::succeeded, this,
            [&](const ConversationId &c, const AccountId &) { conversation = c; });
    connect(&service, &AddContactService::failed, this,
            [&](AddContactService::Error e) { failure = e; });

    service.startByHandle(QStringLiteral("multi"));
    emit m_relay->handleResolved(entry);
    // The first device has no KeyPackage left; the service falls through to the
    // second device, which succeeds.
    emit m_relay->keyPackageClaimFailed(RelayClaimError::Unavailable);
    emit m_relay->keyPackageClaimed(m_keyPackage);

    QVERIFY(!failure.has_value());
    QVERIFY(conversation.has_value());
    QCOMPARE(m_transport->sent.size(), qsizetype(1));
    // The Welcome is addressed to the SECOND device, proving the fall-through.
    QCOMPARE(m_transport->sent.first().recipientDeviceId.bytes(), secondDevice.bytes());
}

void AddContactServiceTest::keyPackageExhaustedFailsNoKeyPackage()
{
    const RelayDirectoryEntry entry{AccountId::generate(), {device(DeviceId::generate())}};

    AddContactService service(*m_session, *m_relay, *m_session->syncEngine());
    std::optional<AddContactService::Error> failure;
    connect(&service, &AddContactService::failed, this,
            [&](AddContactService::Error e) { failure = e; });

    service.startByHandle(QStringLiteral("empty-pool"));
    emit m_relay->handleResolved(entry);
    // Only one device and it has no KeyPackage: terminal.
    emit m_relay->keyPackageClaimFailed(RelayClaimError::Unavailable);

    QVERIFY(failure.has_value());
    QCOMPARE(failure.value(), AddContactService::Error::NoKeyPackage);
    QCOMPARE(m_transport->sent.size(), qsizetype(0));
}

void AddContactServiceTest::eightPackagePoolExhaustion()
{
    const RelayDirectoryEntry entry{AccountId::generate(), {device(DeviceId::generate())}};
    QList<QByteArray> pool;
    for (int i = 0; i < 8; ++i) {
        auto package = m_peer->generateKeyPackage();
        QVERIFY(package.hasValue());
        pool.append(package.value());
    }
    for (int i = 0; i < 9; ++i) {
        AddContactService service(*m_session, *m_relay, *m_session->syncEngine());
        bool succeeded = false;
        std::optional<AddContactService::Error> failure;
        connect(&service, &AddContactService::succeeded, &service,
                [&](const ConversationId &, const AccountId &) { succeeded = true; });
        connect(&service, &AddContactService::failed, &service,
                [&](AddContactService::Error error) { failure = error; });
        service.startByHandle(QStringLiteral("finite-pool"));
        emit m_relay->handleResolved(entry);
        if (!pool.isEmpty())
            emit m_relay->keyPackageClaimed(pool.takeFirst());
        else
            emit m_relay->keyPackageClaimFailed(RelayClaimError::Unavailable);
        if (i < 8) {
            QVERIFY(succeeded);
            QVERIFY(!failure);
        } else {
            QVERIFY(failure);
            QCOMPARE(*failure, AddContactService::Error::NoKeyPackage);
            QVERIFY(!succeeded);
        }
    }
}

void AddContactServiceTest::emptyPoolRecoversAndPrivateKeysSurviveRestart()
{
    RelayTest::CertAuthority ca;
    RelayTest::FakeHttpsServer server(RelayTest::serverConfig(ca.localhostLeaf()));
    QVERIFY(server.isListening());
    const QString path = QStringLiteral("/v1/key-packages");
    auto countBody = [](int count) {
        QCborMap map;
        map.insert(QLatin1StringView("availableKeyPackages"), count);
        return map.toCborValue().toCbor();
    };
    server.enqueue(path, {200, countBody(0)});
    for (int count = 1; count <= KeyPackageSupply::targetAvailable; ++count)
        server.enqueue(path, {200, countBody(count)});
    RelayEndpoints endpoints;
    endpoints.keyPackages = server.url(path);
    RelayClient relay(DeviceId::generate(), AccountId::generate(), endpoints, {});
    relay.setTlsConfiguration(RelayTest::clientConfigTrusting(ca.caCertPem()));
    relay.setTokens("access", "refresh");
    QSignalSpy published(&relay, &RelayClient::keyPackagePublished);
    {
        KeyPackageSupply supply(*m_session, relay);
        supply.start();
        QTRY_COMPARE(published.count(), KeyPackageSupply::targetAvailable);
        QCOMPARE(server.requestCount(path), 1 + KeyPackageSupply::targetAvailable);
        // Claim notification: refill from seven, one package at a time.
        for (int count = 8; count <= KeyPackageSupply::targetAvailable; ++count)
            server.enqueue(path, {200, countBody(count)});
        emit relay.keyPackageCountReceived(7);
        QTRY_COMPARE(published.count(), KeyPackageSupply::targetAvailable + 9);
    }
    const auto package = QCborValue::fromCbor(server.lastBody(path)).toMap()
        .value(QLatin1StringView("key_package")).toByteArray();
    QVERIFY(!package.isEmpty());
    m_session->lock();
    m_session.reset();
    auto reopened = ProfileSession::unlock(m_profileId, *m_vault,
        ProfilePaths::forProfile(m_dir->path(), m_profileId));
    QVERIFY(reopened.hasValue());
    m_session = std::move(reopened).value();
    // This Welcome uses the final published package. Joining after restart proves
    // that generation committed the corresponding private material to SQLCipher.
    const auto conversation = ConversationId::generate();
    QVERIFY(m_peer->createGroup(conversation).hasValue());
    const auto added = m_peer->addMembers(conversation, {package});
    QVERIFY(added.hasValue());
    QVERIFY(m_session->mls()->joinGroup(conversation, added.value().welcome).hasValue());
}

void AddContactServiceTest::supplyStopsAtThresholdAndRecoversAfterFailure()
{
    RelayTest::CertAuthority ca;
    RelayTest::FakeHttpsServer server(RelayTest::serverConfig(ca.localhostLeaf()));
    QVERIFY(server.isListening());
    const QString path = QStringLiteral("/v1/key-packages");
    const auto body = [](int count) {
        QCborMap map;
        map.insert(QLatin1StringView("availableKeyPackages"), count);
        return map.toCborValue().toCbor();
    };
    server.enqueue(path, {200, body(8)});
    RelayEndpoints endpoints;
    endpoints.keyPackages = server.url(path);
    RelayClient relay(DeviceId::generate(), AccountId::generate(), endpoints, {});
    relay.setTlsConfiguration(RelayTest::clientConfigTrusting(ca.caCertPem()));
    relay.setTokens("access", "refresh");
    QSignalSpy counts(&relay, &RelayClient::keyPackageCountReceived);
    QSignalSpy published(&relay, &RelayClient::keyPackagePublished);
    QSignalSpy failed(&relay, &RelayClient::keyPackagePublishFailed);
    KeyPackageSupply supply(*m_session, relay);
    supply.start();
    QTRY_COMPARE(counts.count(), 1);
    QCOMPARE(published.count(), 0);
    QCOMPARE(server.requestCount(path), 1);
    server.enqueue(path, {500, {}});
    emit relay.keyPackageCountReceived(0);
    QTRY_COMPARE(failed.count(), 1);
    QCOMPARE(published.count(), 0);
    // Reconnect rechecks uncertain outcomes before generating more packages.
    server.enqueue(path, {200, body(0)});
    for (int count = 1; count <= KeyPackageSupply::targetAvailable; ++count)
        server.enqueue(path, {200, body(count)});
    emit relay.connected();
    QTRY_COMPARE(published.count(), KeyPackageSupply::targetAvailable);
    supply.pause();
    emit relay.keyPackageCountReceived(0);
    QCoreApplication::processEvents();
    QCOMPARE(published.count(), KeyPackageSupply::targetAvailable);
}

void AddContactServiceTest::blockedPeerFailsBlockedAndSendsNothing()
{
    const AccountId peerAccount = AccountId::generate();
    QVERIFY(m_session->contacts()
                ->block(peerAccount, QDateTime::currentMSecsSinceEpoch())
                .hasValue());
    const RelayDirectoryEntry entry{peerAccount, {device(DeviceId::generate())}};

    AddContactService service(*m_session, *m_relay, *m_session->syncEngine());
    std::optional<AddContactService::Error> failure;
    bool succeeded = false;
    connect(&service, &AddContactService::succeeded, this,
            [&](const ConversationId &, const AccountId &) { succeeded = true; });
    connect(&service, &AddContactService::failed, this,
            [&](AddContactService::Error e) { failure = e; });

    service.startByHandle(QStringLiteral("blocked"));
    emit m_relay->handleResolved(entry);
    // Must fail BEFORE any package is returned; blocking cannot consume one.
    QVERIFY(failure.has_value());
    emit m_relay->keyPackageClaimed(m_keyPackage);

    QVERIFY(!succeeded);
    QVERIFY(failure.has_value());
    QCOMPARE(failure.value(), AddContactService::Error::Blocked);
    // The authoritative Blocked gate fires before the send: nothing left the device.
    QCOMPARE(m_transport->sent.size(), qsizetype(0));

    // The roster row is left Blocked, never overwritten.
    auto found = m_session->contacts()->find(peerAccount);
    QVERIFY(found.hasValue());
    QVERIFY(found.value().has_value());
    QCOMPARE(found.value()->state, ContactState::Blocked);
}

void AddContactServiceTest::handleNotFoundFails()
{
    AddContactService service(*m_session, *m_relay, *m_session->syncEngine());
    std::optional<AddContactService::Error> failure;
    connect(&service, &AddContactService::failed, this,
            [&](AddContactService::Error e) { failure = e; });

    service.startByHandle(QStringLiteral("ghost"));
    emit m_relay->handleResolutionFailed(RelayDirectoryError::NotFound);

    QVERIFY(failure.has_value());
    QCOMPARE(failure.value(), AddContactService::Error::NotFound);
    QCOMPARE(m_transport->sent.size(), qsizetype(0));
}

QTEST_GUILESS_MAIN(AddContactServiceTest)
#include "tst_addcontactservice.moc"
