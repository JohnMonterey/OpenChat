#include "RelayServer.h"
#include <QWebSocket>
#include <QCborArray>
#include <QNetworkRequest>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QCborMap>
#include <QSignalSpy>
#include "AuthService.h"
#include "DirectoryService.h"
#include "EnvelopeService.h"
#include "KeyPackageService.h"
#include "PostgresStore.h"
#include "RelayCrypto.h"

#include "protocol/CanonicalCborCodec.h"
#include "protocol/CiphertextEnvelope.h"

#include <QCryptographicHash>
#include <QSqlError>
#include <QSqlQuery>
#include <QtTest/QtTest>

#include <openssl/evp.h>

#include <atomic>
#include <memory>
#include <thread>

using namespace OpenChat;
using namespace OpenChat::Relay;

namespace {

struct DeviceKey final {
    EVP_PKEY *pkey = nullptr;
    QByteArray publicKey;

    ~DeviceKey()
    {
        if (pkey)
            EVP_PKEY_free(pkey);
    }
    DeviceKey() = default;
    DeviceKey(DeviceKey &&other) noexcept : pkey(other.pkey), publicKey(other.publicKey)
    {
        other.pkey = nullptr;
    }
    DeviceKey &operator=(DeviceKey &&other) noexcept
    {
        if (this != &other) {
            if (pkey)
                EVP_PKEY_free(pkey);
            pkey = other.pkey;
            publicKey = other.publicKey;
            other.pkey = nullptr;
        }
        return *this;
    }
    DeviceKey(const DeviceKey &) = delete;
    DeviceKey &operator=(const DeviceKey &) = delete;
};

DeviceKey generateDeviceKey()
{
    DeviceKey key;
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
    if (ctx && EVP_PKEY_keygen_init(ctx) == 1)
        EVP_PKEY_keygen(ctx, &key.pkey);
    if (ctx)
        EVP_PKEY_CTX_free(ctx);
    key.publicKey = QByteArray(32, Qt::Uninitialized);
    std::size_t len = 32;
    EVP_PKEY_get_raw_public_key(key.pkey, reinterpret_cast<unsigned char *>(key.publicKey.data()),
                                &len);
    return key;
}

QByteArray signWith(EVP_PKEY *pkey, QByteArrayView message)
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    QByteArray signature(64, Qt::Uninitialized);
    std::size_t len = 64;
    bool ok = ctx && EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, pkey) == 1
        && EVP_DigestSign(ctx, reinterpret_cast<unsigned char *>(signature.data()), &len,
                          reinterpret_cast<const unsigned char *>(message.data()),
                          static_cast<std::size_t>(message.size()))
            == 1;
    if (ctx)
        EVP_MD_CTX_free(ctx);
    return ok ? signature : QByteArray{};
}

CiphertextEnvelopeV1 signedEnvelope(EVP_PKEY *senderKey, const AccountId &senderAccount,
                                    const DeviceId &senderDevice, const DeviceId &recipientDevice,
                                    const QByteArray &ciphertext, qint64 now)
{
    CiphertextEnvelopeV1 envelope{
        1,
        EnvelopeId::generate(),
        senderAccount,
        senderDevice,
        recipientDevice,
        ConversationId::generate(),
        EnvelopeMessageKind::MlsPrivateMessage,
        now,
        now + 60'000,
        EnvelopeId::generate(),
        ciphertext,
        QCryptographicHash::hash(ciphertext, QCryptographicHash::Sha256),
        QByteArray(64, '\0')};
    // envelopeSigningInput clears the signature field before encoding, so the
    // placeholder above does not affect the signed bytes.
    envelope.senderSignature = signWith(senderKey, envelopeSigningInput(envelope));
    return envelope;
}

} // namespace

class RelayServicesTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init(); // reset DB before each test

    void schemaIsCiphertextOnly();
    void challengeAuthenticationRoundTrip();
    void challengeReplayRejected();
    void challengeExpiryRejected();
    void refreshRotationAndReuseRevokesFamily();
    void livePresenceRejectionAndReconnectReplay();
    void duplicateSendIsIdempotent();
    void offlineRejectionAndLostAcceptanceAreUnambiguous();
    void datagramValidationIsFullButStoresNothing();
    void keyPackageClaimIsOneTime();
    void eightPackagePoolExhaustion();
    void keyPackageSupplyReportsCountsAndExpiry();
    void concurrentKeyPackageClaimHasOneWinner();
    void concurrentClaimWithTwoPackagesGivesDistinct();
    void sameIdempotencyKeyDifferentSendersBothDelivered();
    void watermarkAdvancesAndBoundsCatchUp();
    void catchUpIsBoundedByResponseBytes();
    void expiredEnvelopeRejectedAtSubmit();
    void acknowledgePrunesDeliveredInbox();
    void revokedDeviceIsRejectedEverywhere();
    void directoryResolvesHandleToActiveDevices();
    void directoryExcludesRevokedDevices();
    void directoryResolvesAccountToHandle();
    void inviteRedeemsOnceAndReturnsInviter();
    void inviteExpiryRejectsRedemption();

private:
    struct Registered {
        AccountId account = AccountId::generate();
        DeviceId device = DeviceId::generate();
        DeviceKey key;
        AuthTokens tokens;
    };

    Registered registerDevice(const QString &handle);

    bool m_available = false;
    QString m_testDb = QStringLiteral("oc_relay_svctest");
    PostgresStore::Config m_config;
    std::unique_ptr<PostgresStore> m_store;
    qint64 m_now = 1'700'000'000'000;
};

void RelayServicesTest::initTestCase()
{
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
    auto admin = PostgresStore::open(adminConfig, QStringLiteral("svc_admin"), &error);
    if (!admin) {
        qInfo() << "PostgreSQL unavailable:" << error;
        QSKIP("PostgreSQL not available for relay service tests");
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
    m_store = PostgresStore::open(m_config, QStringLiteral("svc_main"), &error);
    QVERIFY2(m_store, qPrintable(error));
    m_store->setClock([this] { return m_now; });

    const QStringList migrations{QStringLiteral(":/relay/001_accounts_devices.sql"),
                                 QStringLiteral(":/relay/002_tokens_keypackages.sql"),
                                 QStringLiteral(":/relay/003_inboxes_attachments.sql"),
                                 QStringLiteral(":/relay/004_invites.sql"),
                                 QStringLiteral(":/relay/005_envelope_acceptances.sql")};
    QVERIFY2(m_store->applyMigrations(migrations, &error), qPrintable(error));
    m_available = true;
}

void RelayServicesTest::cleanupTestCase()
{
    m_store.reset();
}

void RelayServicesTest::init()
{
    if (!m_available)
        QSKIP("PostgreSQL not available");
    m_now = 1'700'000'000'000;
    QSqlQuery truncate(m_store->database());
    QVERIFY(truncate.exec(QStringLiteral(
        "TRUNCATE accounts, devices, auth_challenges, token_families, refresh_tokens, "
        "access_tokens, key_packages, inbox_messages, device_watermarks, attachments, "
        "rate_limits, invites, envelope_acceptances RESTART IDENTITY CASCADE")));
}

RelayServicesTest::Registered RelayServicesTest::registerDevice(const QString &handle)
{
    Registered reg;
    reg.key = generateDeviceKey();
    AuthService auth(*m_store);
    const auto registered = auth.registerAccount(reg.account, handle, reg.device, reg.key.publicKey,
                                                 QByteArray("mls-credential-blob"));
    Q_ASSERT(registered.hasValue());

    const auto challenge = auth.issueChallenge(reg.account, reg.device, 1);
    Q_ASSERT(challenge.hasValue());
    const QByteArray context("account-device-bind");
    const QByteArray signature =
        signWith(reg.key.pkey, challengeSigningMessage(challenge.value(), context));
    auto tokens = auth.completeChallenge(reg.account, reg.device, challenge.value(), signature,
                                         context);
    Q_ASSERT(tokens.hasValue());
    reg.tokens = tokens.value();
    return reg;
}

void RelayServicesTest::schemaIsCiphertextOnly()
{
    QSqlQuery forbidden(m_store->database());
    QVERIFY(forbidden.exec(QStringLiteral(
        "SELECT count(*) FROM information_schema.columns WHERE table_schema = 'public' "
        "AND lower(column_name) ~ "
        "'(^|_)(body|plaintext|cleartext|message_text|decrypted|secret|private_key)(_|$)'")));
    QVERIFY(forbidden.next());
    QCOMPARE(forbidden.value(0).toInt(), 0);
}

void RelayServicesTest::challengeAuthenticationRoundTrip()
{
    const auto reg = registerDevice(QStringLiteral("alice"));
    QVERIFY(!reg.tokens.accessToken.isEmpty());
    QVERIFY(!reg.tokens.refreshToken.isEmpty());

    AuthService auth(*m_store);
    const auto identity = auth.authenticate(reg.tokens.accessToken);
    QVERIFY(identity.has_value());
    QCOMPARE(identity->deviceId, reg.device);
    QCOMPARE(identity->accountId, reg.account);

    // A garbage token authenticates no one.
    QVERIFY(!auth.authenticate(QByteArray("not-a-real-token")).has_value());
}

void RelayServicesTest::challengeReplayRejected()
{
    Registered reg;
    reg.key = generateDeviceKey();
    AuthService auth(*m_store);
    QVERIFY(auth.registerAccount(reg.account, QStringLiteral("bob"), reg.device, reg.key.publicKey,
                                 QByteArray("cred"))
                .hasValue());
    const auto challenge = auth.issueChallenge(reg.account, reg.device, 1);
    QVERIFY(challenge.hasValue());
    const QByteArray context("ctx");
    const QByteArray signature =
        signWith(reg.key.pkey, challengeSigningMessage(challenge.value(), context));

    QVERIFY(auth.completeChallenge(reg.account, reg.device, challenge.value(), signature, context)
                .hasValue());
    // Replaying the same challenge fails.
    const auto replay =
        auth.completeChallenge(reg.account, reg.device, challenge.value(), signature, context);
    QVERIFY(!replay.hasValue());
    QCOMPARE(replay.error(), RelayError::Unauthorized);
}

void RelayServicesTest::challengeExpiryRejected()
{
    Registered reg;
    reg.key = generateDeviceKey();
    AuthService auth(*m_store);
    QVERIFY(auth.registerAccount(reg.account, QStringLiteral("carol"), reg.device,
                                 reg.key.publicKey, QByteArray("cred"))
                .hasValue());
    const auto challenge = auth.issueChallenge(reg.account, reg.device, 1);
    QVERIFY(challenge.hasValue());
    const QByteArray context("ctx");
    const QByteArray signature =
        signWith(reg.key.pkey, challengeSigningMessage(challenge.value(), context));

    m_now += 121'000; // past the 120s challenge TTL
    const auto expired =
        auth.completeChallenge(reg.account, reg.device, challenge.value(), signature, context);
    QVERIFY(!expired.hasValue());
    QCOMPARE(expired.error(), RelayError::Expired);
}

void RelayServicesTest::refreshRotationAndReuseRevokesFamily()
{
    const auto reg = registerDevice(QStringLiteral("dave"));
    AuthService auth(*m_store);

    const auto rotated = auth.refresh(reg.tokens.refreshToken);
    QVERIFY(rotated.hasValue());
    QVERIFY(rotated.value().refreshToken != reg.tokens.refreshToken);

    // The new access token works.
    QVERIFY(auth.authenticate(rotated.value().accessToken).has_value());

    // Reusing the original (now-rotated) refresh token is treated as theft.
    const auto reuse = auth.refresh(reg.tokens.refreshToken);
    QVERIFY(!reuse.hasValue());
    QCOMPARE(reuse.error(), RelayError::TokenReuse);

    // The whole family is now revoked: the rotated refresh token no longer works
    // and its access token is rejected.
    const auto afterReuse = auth.refresh(rotated.value().refreshToken);
    QVERIFY(!afterReuse.hasValue());
    QVERIFY(!auth.authenticate(rotated.value().accessToken).has_value());
    QVERIFY(!auth.authenticate(reg.tokens.accessToken).has_value());
}

void RelayServicesTest::duplicateSendIsIdempotent()
{
    const auto sender = registerDevice(QStringLiteral("erin"));
    const auto recipient = registerDevice(QStringLiteral("frank"));

    EnvelopeService envelopes(*m_store);
    const CiphertextEnvelopeV1 envelope = signedEnvelope(
        sender.key.pkey, sender.account, sender.device, recipient.device, "ciphertext", m_now);
    const QByteArray bytes = encodeCanonical(envelope);
    const AuthenticatedDevice authSender{sender.account, sender.device};

    const auto first = envelopes.submit(authSender, bytes);
    QVERIFY2(first.hasValue(), "first submit should succeed");
    QVERIFY(!first.value().duplicate);

    const auto second = envelopes.submit(authSender, bytes);
    QVERIFY(second.hasValue());
    QVERIFY(second.value().duplicate);
    QCOMPARE(second.value().serverSequence, first.value().serverSequence);

    const auto fetched = envelopes.fetchSince(recipient.device, 0, 100);
    QVERIFY(fetched.hasValue());
    QCOMPARE(fetched.value().items.size(), 1);
    QCOMPARE(fetched.value().items.first().envelope, bytes);

    // An envelope whose sender does not match the authenticated device is refused.
    const auto forged = envelopes.submit(AuthenticatedDevice{recipient.account, recipient.device},
                                         bytes);
    QVERIFY(!forged.hasValue());
    QCOMPARE(forged.error(), RelayError::Unauthorized);
}

void RelayServicesTest::livePresenceRejectionAndReconnectReplay()
{
    const auto sender = registerDevice(QStringLiteral("live_sender"));
    const auto recipient = registerDevice(QStringLiteral("live_recipient"));
    AuthService auth(*m_store);
    EnvelopeService envelopes(*m_store);
    KeyPackageService packages(*m_store);
    DirectoryService directory(*m_store);
    RelayServer::Limits limits;
    limits.syncLimit = 1; // exercise replay across multiple pages
    RelayServer server(*m_store, auth, envelopes, packages, directory, limits, nullptr);
    const auto port = server.start(QHostAddress::LocalHost, 0);
    QVERIFY(port);
    const AuthenticatedDevice identity{sender.account, sender.device};
    for (int i = 0; i < 2; ++i) {
        const auto envelope = signedEnvelope(sender.key.pkey, sender.account, sender.device,
                                             recipient.device, "backlog", m_now);
        QVERIFY(envelopes.submit(identity, encodeCanonical(envelope)).hasValue());
    }
    const auto request = [port](const QByteArray &token) {
        QNetworkRequest req(QUrl(QStringLiteral("ws://127.0.0.1:%1/v1/live?since=0").arg(port)));
        req.setRawHeader("Authorization", "Bearer " + token);
        return req;
    };
    QWebSocket peer;
    QSignalSpy received(&peer, &QWebSocket::binaryMessageReceived);
    peer.open(request(recipient.tokens.accessToken));
    QTRY_COMPARE(received.size(), 2);
    QWebSocket local;
    QSignalSpy replies(&local, &QWebSocket::binaryMessageReceived);
    local.open(request(sender.tokens.accessToken));
    QTRY_COMPARE(local.state(), QAbstractSocket::ConnectedState);
    QCborArray ids; ids.append(recipient.device.bytes());
    QCborArray query; query.append(7); query.append(ids);
    local.sendBinaryMessage(query.toCborValue().toCbor());
    QTRY_COMPARE(replies.size(), 1);
    auto response = QCborValue::fromCbor(replies.takeFirst().first().toByteArray()).toArray();
    QCOMPARE(response.at(0).toInteger(), 8);
    QVERIFY(response.at(1).toArray().first().toArray().at(1).toBool());
    peer.close();
    QTRY_COMPARE(peer.state(), QAbstractSocket::UnconnectedState);
    local.sendBinaryMessage(query.toCborValue().toCbor());
    QTRY_COMPARE(replies.size(), 1);
    response = QCborValue::fromCbor(replies.takeFirst().first().toByteArray()).toArray();
    QVERIFY(!response.at(1).toArray().first().toArray().at(1).toBool());
    const auto rejected = signedEnvelope(sender.key.pkey, sender.account, sender.device,
                                         recipient.device, "offline", m_now);
    local.sendBinaryMessage(encodeCanonical(rejected));
    QTRY_COMPARE(replies.size(), 1);
    response = QCborValue::fromCbor(replies.takeFirst().first().toByteArray()).toArray();
    QCOMPARE(response.at(0).toInteger(), 9);
    QCOMPARE(response.at(1).toByteArray(), rejected.envelopeId.bytes());
    received.clear();
    peer.open(request(recipient.tokens.accessToken));
    QTRY_COMPARE(received.size(), 2); // only the pre-existing backlog
    for (const auto &frame : received) {
        const auto delivery = QCborValue::fromCbor(frame.first().toByteArray()).toArray();
        QCOMPARE(delivery.at(0).toInteger(), 4);
    }
    peer.close();
    local.close();
    QTRY_COMPARE(peer.state(), QAbstractSocket::UnconnectedState);
    QTRY_COMPARE(local.state(), QAbstractSocket::UnconnectedState);
}

void RelayServicesTest::offlineRejectionAndLostAcceptanceAreUnambiguous()
{
    const auto sender = registerDevice(QStringLiteral("retry_sender"));
    const auto recipient = registerDevice(QStringLiteral("retry_recipient"));
    EnvelopeService envelopes(*m_store);
    const auto envelope = signedEnvelope(sender.key.pkey, sender.account, sender.device,
                                         recipient.device, "hello", m_now);
    const auto bytes = encodeCanonical(envelope);
    const AuthenticatedDevice identity{sender.account, sender.device};
    const auto rejected = envelopes.submit(identity, bytes, false);
    QVERIFY(!rejected.hasValue());
    QCOMPARE(rejected.error(), RelayError::RecipientUnavailable);
    QVERIFY(envelopes.fetchSince(recipient.device, 0, 100).value().items.isEmpty());
    const auto accepted = envelopes.submit(identity, bytes, true);
    QVERIFY(accepted.hasValue());
    QVERIFY(envelopes.acknowledge(recipient.device, accepted.value().serverSequence).hasValue());
    const auto retry = envelopes.submit(identity, bytes, false);
    QVERIFY(retry.hasValue());
    QVERIFY(retry.value().duplicate);
    QCOMPARE(retry.value().serverSequence, accepted.value().serverSequence);
    QVERIFY(envelopes.fetchSince(recipient.device, 0, 100).value().items.isEmpty());
}

void RelayServicesTest::datagramValidationIsFullButStoresNothing()
{
    // The unreliable media path forwards without storing, so validate() is the
    // ONLY thing standing between an unauthenticated frame and a third party's
    // socket. It must apply every check submit() does — and write nothing.
    const auto sender = registerDevice(QStringLiteral("nina"));
    const auto recipient = registerDevice(QStringLiteral("oscar"));

    EnvelopeService envelopes(*m_store);
    const CiphertextEnvelopeV1 envelope =
        signedEnvelope(sender.key.pkey, sender.account, sender.device, recipient.device,
                       "sealed-audio", m_now);
    const QByteArray bytes = encodeCanonical(envelope);
    const AuthenticatedDevice authSender{sender.account, sender.device};

    const auto validated = envelopes.validate(authSender, bytes);
    QVERIFY2(validated.hasValue(), "a well-formed signed envelope should validate");
    QCOMPARE(validated.value().recipientDeviceId.bytes(), recipient.device.bytes());
    QCOMPARE(validated.value().messageKind, envelope.messageKind);

    // Nothing was written: the recipient's inbox is untouched, so a call does not
    // leave thousands of rows behind.
    const auto fetched = envelopes.fetchSince(recipient.device, 0, 100);
    QVERIFY(fetched.hasValue());
    QCOMPARE(fetched.value().items.size(), 0);

    // Claiming to be somebody else is refused.
    const auto impersonated =
        envelopes.validate(AuthenticatedDevice{recipient.account, recipient.device}, bytes);
    QVERIFY(!impersonated.hasValue());
    QCOMPARE(impersonated.error(), RelayError::Unauthorized);

    // A tampered signature is refused.
    CiphertextEnvelopeV1 forged = envelope;
    forged.senderSignature[0] = char(forged.senderSignature.at(0) ^ 0x01);
    const auto badSignature = envelopes.validate(authSender, encodeCanonical(forged));
    QVERIFY(!badSignature.hasValue());
    QCOMPARE(badSignature.error(), RelayError::Unauthorized);

    // A recipient the relay does not know is refused rather than forwarded blind.
    const CiphertextEnvelopeV1 toNobody =
        signedEnvelope(sender.key.pkey, sender.account, sender.device, DeviceId::generate(),
                       "sealed-audio", m_now);
    const auto unknownRecipient = envelopes.validate(authSender, encodeCanonical(toNobody));
    QVERIFY(!unknownRecipient.hasValue());
    QCOMPARE(unknownRecipient.error(), RelayError::NotFound);

    // An expired envelope is refused, so a captured media frame cannot be
    // replayed into a later call.
    const CiphertextEnvelopeV1 stale =
        signedEnvelope(sender.key.pkey, sender.account, sender.device, recipient.device,
                       "sealed-audio", m_now - 120'000);
    const auto expired = envelopes.validate(authSender, encodeCanonical(stale));
    QVERIFY(!expired.hasValue());
    QCOMPARE(expired.error(), RelayError::InvalidRequest);

    // Garbage is refused before anything is allocated for it.
    const auto garbage = envelopes.validate(authSender, QByteArray("not an envelope"));
    QVERIFY(!garbage.hasValue());
    QCOMPARE(garbage.error(), RelayError::InvalidRequest);
}

void RelayServicesTest::keyPackageClaimIsOneTime()
{
    const auto owner = registerDevice(QStringLiteral("grace"));
    const auto claimer = registerDevice(QStringLiteral("heidi"));

    KeyPackageService packages(*m_store);
    QVERIFY(packages.publish(owner.account, owner.device, QByteArray("key-package-1")).hasValue());
    QCOMPARE(packages.availableCount(owner.device), 1);

    // Duplicate upload is rejected.
    const auto dup = packages.publish(owner.account, owner.device, QByteArray("key-package-1"));
    QVERIFY(!dup.hasValue());
    QCOMPARE(dup.error(), RelayError::Conflict);

    const auto claimed = packages.claim(owner.device, claimer.device);
    QVERIFY(claimed.hasValue());
    QCOMPARE(claimed.value(), QByteArray("key-package-1"));
    QCOMPARE(packages.availableCount(owner.device), 0);

    const auto again = packages.claim(owner.device, claimer.device);
    QVERIFY(!again.hasValue());
    QCOMPARE(again.error(), RelayError::NotFound);
}

void RelayServicesTest::eightPackagePoolExhaustion()
{
    const auto owner = registerDevice(QStringLiteral("pool-owner"));
    const auto claimer = registerDevice(QStringLiteral("pool-claimer"));
    KeyPackageService packages(*m_store);
    for (int i = 0; i < 8; ++i)
        QVERIFY(packages.publish(owner.account, owner.device,
                                QByteArray("package-") + QByteArray::number(i)).hasValue());
    for (int i = 0; i < 8; ++i) {
        QVERIFY(packages.claim(owner.device, claimer.device).hasValue());
        QCOMPARE(packages.availableCount(owner.device), 7 - i);
    }
    const auto ninth = packages.claim(owner.device, claimer.device);
    QVERIFY(!ninth.hasValue());
    QCOMPARE(ninth.error(), RelayError::NotFound);
    QCOMPARE(packages.availableCount(owner.device), 0);
}

void RelayServicesTest::keyPackageSupplyReportsCountsAndExpiry()
{
    const auto owner = registerDevice(QStringLiteral("supply-owner"));
    const auto claimer = registerDevice(QStringLiteral("supply-claimer"));
    AuthService auth(*m_store);
    EnvelopeService envelopes(*m_store);
    KeyPackageService::Policy policy;
    policy.ttlMs = 10;
    KeyPackageService packages(*m_store, policy);
    DirectoryService directory(*m_store);
    RelayServer server(*m_store, auth, envelopes, packages, directory);
    const auto port = server.start(QHostAddress::LocalHost, 0);
    QVERIFY(port);
    const auto request = [port](const QString &path, const QByteArray &token) {
        QNetworkRequest req(QUrl(QStringLiteral("http://127.0.0.1:%1/v1/").arg(port) + path));
        req.setRawHeader("Authorization", "Bearer " + token);
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/cbor");
        return req;
    };
    QNetworkAccessManager network;
    auto *empty = network.get(request(QStringLiteral("key-packages"), owner.tokens.accessToken));
    QTRY_VERIFY(empty->isFinished());
    QCOMPARE(empty->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
    QCOMPARE(QCborValue::fromCbor(empty->readAll()).toMap()
        .value(QLatin1StringView("availableKeyPackages")).toInteger(-1), 0);
    empty->deleteLater();
    auto *unauth = network.get(request(QStringLiteral("key-packages"), "invalid"));
    QTRY_VERIFY(unauth->isFinished());
    QCOMPARE(unauth->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 401);
    unauth->deleteLater();

    QWebSocket socket;
    QSignalSpy frames(&socket, &QWebSocket::binaryMessageReceived);
    QNetworkRequest live(QUrl(QStringLiteral("ws://127.0.0.1:%1/v1/live?keyPackageSupply=1").arg(port)));
    live.setRawHeader("Authorization", "Bearer " + owner.tokens.accessToken);
    socket.open(live);
    QTRY_COMPARE(frames.size(), 1);
    QCOMPARE(QCborValue::fromCbor(frames.takeFirst().at(0).toByteArray()).toArray(), QCborArray({10, 0}));
    QCborMap upload;
    upload.insert(QLatin1StringView("key_package"), QByteArray("fresh-package"));
    auto *published = network.post(request(QStringLiteral("key-packages"), owner.tokens.accessToken),
                                    upload.toCborValue().toCbor());
    QTRY_VERIFY(published->isFinished());
    QCOMPARE(QCborValue::fromCbor(published->readAll()).toMap()
        .value(QLatin1StringView("availableKeyPackages")).toInteger(-1), 1);
    published->deleteLater();
    QCborMap claim;
    claim.insert(QLatin1StringView("target_device_id"), owner.device.bytes());
    auto *claimed = network.post(request(QStringLiteral("key-packages/claim"), claimer.tokens.accessToken),
                                 claim.toCborValue().toCbor());
    QTRY_VERIFY(claimed->isFinished());
    QCOMPARE(claimed->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
    claimed->deleteLater();
    QTRY_COMPARE(frames.size(), 1);
    QCOMPARE(QCborValue::fromCbor(frames.takeFirst().at(0).toByteArray()).toArray(), QCborArray({10, 0}));
    QVERIFY(packages.publish(owner.account, owner.device, QByteArray("expires-soon")).hasValue());
    QCOMPARE(packages.availableCount(owner.device), 1);
    m_now += 10;
    auto *expired = network.get(request(QStringLiteral("key-packages"), owner.tokens.accessToken));
    QTRY_VERIFY(expired->isFinished());
    QCOMPARE(QCborValue::fromCbor(expired->readAll()).toMap()
        .value(QLatin1StringView("availableKeyPackages")).toInteger(-1), 0);
    expired->deleteLater();
    socket.close();
}

void RelayServicesTest::concurrentKeyPackageClaimHasOneWinner()
{
    const auto owner = registerDevice(QStringLiteral("ivan"));
    const auto claimer = registerDevice(QStringLiteral("judy"));
    {
        KeyPackageService packages(*m_store);
        QVERIFY(packages.publish(owner.account, owner.device, QByteArray("solo-package"))
                    .hasValue());
    }

    std::atomic<int> successes{0};
    QMutex registryGuard; // guard QSqlDatabase::addDatabase/removeDatabase registry
    auto worker = [&](const char *name) {
        registryGuard.lock();
        QString error;
        auto store = PostgresStore::open(m_config, QString::fromLatin1(name), &error);
        registryGuard.unlock();
        if (!store)
            return;
        store->setClock([] { return qint64(1'700'000'000'000); });
        KeyPackageService packages(*store);
        const auto result = packages.claim(owner.device, claimer.device);
        if (result.hasValue() && !result.value().isEmpty())
            successes.fetch_add(1);
        registryGuard.lock();
        store.reset();
        registryGuard.unlock();
    };

    std::thread t1(worker, "svc_claim_a");
    std::thread t2(worker, "svc_claim_b");
    t1.join();
    t2.join();

    QCOMPARE(successes.load(), 1); // exactly one winner, never two
    KeyPackageService packages(*m_store);
    QCOMPARE(packages.availableCount(owner.device), 0);
}

void RelayServicesTest::concurrentClaimWithTwoPackagesGivesDistinct()
{
    const auto owner = registerDevice(QStringLiteral("nora"));
    const auto claimer = registerDevice(QStringLiteral("olga"));
    {
        KeyPackageService packages(*m_store);
        QVERIFY(packages.publish(owner.account, owner.device, QByteArray("pkg-a")).hasValue());
        QVERIFY(packages.publish(owner.account, owner.device, QByteArray("pkg-b")).hasValue());
    }

    QMutex registryGuard;
    std::atomic<int> successes{0};
    QByteArray resultA;
    QByteArray resultB;
    auto worker = [&](const char *name, QByteArray &out) {
        registryGuard.lock();
        QString error;
        auto store = PostgresStore::open(m_config, QString::fromLatin1(name), &error);
        registryGuard.unlock();
        if (!store)
            return;
        store->setClock([] { return qint64(1'700'000'000'000); });
        KeyPackageService packages(*store);
        const auto claimed = packages.claim(owner.device, claimer.device);
        if (claimed.hasValue()) {
            out = claimed.value();
            successes.fetch_add(1);
        }
        registryGuard.lock();
        store.reset();
        registryGuard.unlock();
    };

    std::thread t1(worker, "svc_two_a", std::ref(resultA));
    std::thread t2(worker, "svc_two_b", std::ref(resultB));
    t1.join();
    t2.join();

    // Both claims succeed and receive DISTINCT packages (SKIP LOCKED), rather
    // than one spuriously failing.
    QCOMPARE(successes.load(), 2);
    QVERIFY(!resultA.isEmpty());
    QVERIFY(!resultB.isEmpty());
    QVERIFY(resultA != resultB);
    KeyPackageService packages(*m_store);
    QCOMPARE(packages.availableCount(owner.device), 0);
}

void RelayServicesTest::sameIdempotencyKeyDifferentSendersBothDelivered()
{
    const auto senderOne = registerDevice(QStringLiteral("peggy"));
    const auto senderTwo = registerDevice(QStringLiteral("quinn"));
    const auto recipient = registerDevice(QStringLiteral("rick"));
    EnvelopeService envelopes(*m_store);

    auto envelopeOne = signedEnvelope(senderOne.key.pkey, senderOne.account, senderOne.device,
                                      recipient.device, "from-one", m_now);
    auto envelopeTwo = signedEnvelope(senderTwo.key.pkey, senderTwo.account, senderTwo.device,
                                      recipient.device, "from-two", m_now);
    // Force a shared idempotency key across the two distinct senders.
    envelopeTwo.idempotencyKey = envelopeOne.idempotencyKey;
    envelopeTwo.senderSignature.clear();
    envelopeTwo.senderSignature =
        signWith(senderTwo.key.pkey, envelopeSigningInput(envelopeTwo));

    const auto first = envelopes.submit(AuthenticatedDevice{senderOne.account, senderOne.device},
                                        encodeCanonical(envelopeOne));
    const auto second = envelopes.submit(AuthenticatedDevice{senderTwo.account, senderTwo.device},
                                         encodeCanonical(envelopeTwo));
    QVERIFY(first.hasValue());
    QVERIFY(second.hasValue());
    QVERIFY(!first.value().duplicate);
    QVERIFY(!second.value().duplicate); // not collapsed with sender one's message

    const auto fetched = envelopes.fetchSince(recipient.device, 0, 100);
    QVERIFY(fetched.hasValue());
    QCOMPARE(fetched.value().items.size(), 2);
}

void RelayServicesTest::watermarkAdvancesAndBoundsCatchUp()
{
    const auto sender = registerDevice(QStringLiteral("kate"));
    const auto recipient = registerDevice(QStringLiteral("leo"));
    EnvelopeService envelopes(*m_store);

    QList<quint64> sequences;
    for (int i = 0; i < 3; ++i) {
        const auto envelope = signedEnvelope(sender.key.pkey, sender.account, sender.device,
                                             recipient.device, QByteArray("m").repeated(i + 1), m_now);
        const auto submitted = envelopes.submit(AuthenticatedDevice{sender.account, sender.device},
                                                encodeCanonical(envelope));
        QVERIFY(submitted.hasValue());
        sequences.append(submitted.value().serverSequence);
    }

    const auto first = envelopes.fetchSince(recipient.device, 0, 2);
    QVERIFY(first.hasValue());
    QCOMPARE(first.value().items.size(), 2); // bounded by limit

    const auto all = envelopes.fetchSince(recipient.device, 0, 100);
    QVERIFY(all.hasValue());
    QCOMPARE(all.value().items.size(), 3);
    QCOMPARE(all.value().newWatermark, sequences.last());

    QVERIFY(envelopes.acknowledge(recipient.device, all.value().newWatermark).hasValue());
    QCOMPARE(envelopes.watermarkFor(recipient.device), all.value().newWatermark);

    const auto afterAck = envelopes.fetchSince(recipient.device, all.value().newWatermark, 100);
    QVERIFY(afterAck.hasValue());
    QCOMPARE(afterAck.value().items.size(), 0);

    // Watermarks never regress.
    QVERIFY(envelopes.acknowledge(recipient.device, 1).hasValue());
    QCOMPARE(envelopes.watermarkFor(recipient.device), all.value().newWatermark);
}

void RelayServicesTest::catchUpIsBoundedByResponseBytes()
{
    const auto sender = registerDevice(QStringLiteral("nora"));
    const auto recipient = registerDevice(QStringLiteral("otis"));
    // A page cap that only admits about one ~1 KiB envelope at a time.
    EnvelopeService envelopes(*m_store, EnvelopeService::Policy{512, 1500});

    constexpr int total = 4;
    for (int i = 0; i < total; ++i) {
        const auto envelope = signedEnvelope(sender.key.pkey, sender.account, sender.device,
                                             recipient.device, QByteArray(1024, 'x'), m_now);
        QVERIFY(envelopes.submit(AuthenticatedDevice{sender.account, sender.device},
                                 encodeCanonical(envelope))
                    .hasValue());
    }

    // Each page is byte-bounded (fewer than all four, at least one), and paging
    // from the returned watermark eventually drains every message exactly once.
    int drained = 0;
    int pages = 0;
    quint64 cursor = 0;
    while (true) {
        const auto page = envelopes.fetchSince(recipient.device, cursor, 100);
        QVERIFY(page.hasValue());
        if (page.value().items.isEmpty())
            break;
        QVERIFY(page.value().items.size() < total); // response byte cap took effect
        drained += page.value().items.size();
        cursor = page.value().newWatermark;
        if (++pages > total + 2)
            QFAIL("catch-up did not terminate");
    }
    QCOMPARE(drained, total);
    QVERIFY(pages >= 2); // more than one page was required
}

void RelayServicesTest::expiredEnvelopeRejectedAtSubmit()
{
    const auto sender = registerDevice(QStringLiteral("pia"));
    const auto recipient = registerDevice(QStringLiteral("quill"));
    EnvelopeService envelopes(*m_store);

    // A validly-formed envelope whose expiry is already in the past.
    CiphertextEnvelopeV1 envelope{
        1,
        EnvelopeId::generate(),
        sender.account,
        sender.device,
        recipient.device,
        ConversationId::generate(),
        EnvelopeMessageKind::MlsPrivateMessage,
        m_now - 120'000,
        m_now - 60'000,
        EnvelopeId::generate(),
        QByteArray("stale"),
        QCryptographicHash::hash(QByteArray("stale"), QCryptographicHash::Sha256),
        QByteArray(64, '\0')};
    envelope.senderSignature = signWith(sender.key.pkey, envelopeSigningInput(envelope));

    const auto submitted = envelopes.submit(AuthenticatedDevice{sender.account, sender.device},
                                            encodeCanonical(envelope));
    QVERIFY(!submitted.hasValue());
    QCOMPARE(submitted.error(), RelayError::InvalidRequest);
    QCOMPARE(envelopes.fetchSince(recipient.device, 0, 100).value().items.size(), 0);
}

void RelayServicesTest::acknowledgePrunesDeliveredInbox()
{
    const auto sender = registerDevice(QStringLiteral("ruth"));
    const auto recipient = registerDevice(QStringLiteral("saul"));
    EnvelopeService envelopes(*m_store);

    for (int i = 0; i < 2; ++i) {
        const auto envelope = signedEnvelope(sender.key.pkey, sender.account, sender.device,
                                             recipient.device, QByteArray("m").repeated(i + 1), m_now);
        QVERIFY(envelopes.submit(AuthenticatedDevice{sender.account, sender.device},
                                 encodeCanonical(envelope))
                    .hasValue());
    }

    const auto before = envelopes.fetchSince(recipient.device, 0, 100);
    QVERIFY(before.hasValue());
    QCOMPARE(before.value().items.size(), 2);

    QVERIFY(envelopes.acknowledge(recipient.device, before.value().newWatermark).hasValue());

    // Delivered-and-acked rows are physically removed, not merely filtered by the
    // cursor: a fetch from zero now finds nothing.
    const auto after = envelopes.fetchSince(recipient.device, 0, 100);
    QVERIFY(after.hasValue());
    QCOMPARE(after.value().items.size(), 0);
}

void RelayServicesTest::revokedDeviceIsRejectedEverywhere()
{
    const auto reg = registerDevice(QStringLiteral("mallory"));
    AuthService auth(*m_store);
    QVERIFY(auth.revokeDevice(reg.device).hasValue());
    QVERIFY(auth.isDeviceRevoked(reg.device));

    // Its access token no longer authenticates.
    QVERIFY(!auth.authenticate(reg.tokens.accessToken).has_value());
    // It cannot obtain new challenges or refresh.
    QCOMPARE(auth.issueChallenge(reg.account, reg.device, 1).error(), RelayError::Revoked);
    QCOMPARE(auth.refresh(reg.tokens.refreshToken).error(), RelayError::Revoked);

    EnvelopeService envelopes(*m_store);
    QCOMPARE(envelopes.fetchSince(reg.device, 0, 10).error(), RelayError::Revoked);

    KeyPackageService packages(*m_store);
    QCOMPARE(packages.publish(reg.account, reg.device, QByteArray("kp")).error(),
             RelayError::Revoked);
    QCOMPARE(packages.claim(reg.device, reg.device).error(), RelayError::Revoked);
}

void RelayServicesTest::directoryResolvesHandleToActiveDevices()
{
    const auto dana = registerDevice(QStringLiteral("dana"));

    DirectoryService directory(*m_store);
    const auto resolved = directory.resolveHandle(QStringLiteral("dana"));
    QVERIFY(resolved.hasValue());
    QCOMPARE(resolved.value().accountId, dana.account);
    QCOMPARE(resolved.value().devices.size(), 1);
    QCOMPARE(resolved.value().devices.first().deviceId, dana.device);
    // The published signing key is exactly the device's Ed25519 public key.
    QCOMPARE(resolved.value().devices.first().signingKey, dana.key.publicKey);

    // Exact match only: an unknown handle discloses nothing.
    const auto missing = directory.resolveHandle(QStringLiteral("nobody"));
    QVERIFY(!missing.hasValue());
    QCOMPARE(missing.error(), RelayError::NotFound);

    // No prefix/substring matching is offered either.
    const auto prefix = directory.resolveHandle(QStringLiteral("dan"));
    QVERIFY(!prefix.hasValue());
    QCOMPARE(prefix.error(), RelayError::NotFound);
}

void RelayServicesTest::directoryExcludesRevokedDevices()
{
    const auto reg = registerDevice(QStringLiteral("edith"));
    DirectoryService directory(*m_store);

    // Present before revocation.
    QVERIFY(directory.resolveHandle(QStringLiteral("edith")).hasValue());

    AuthService auth(*m_store);
    QVERIFY(auth.revokeDevice(reg.device).hasValue());

    // Its only device is revoked, so the handle now resolves to nothing.
    const auto after = directory.resolveHandle(QStringLiteral("edith"));
    QVERIFY(!after.hasValue());
    QCOMPARE(after.error(), RelayError::NotFound);
}

void RelayServicesTest::inviteRedeemsOnceAndReturnsInviter()
{
    const auto inviter = registerDevice(QStringLiteral("fiona"));
    DirectoryService directory(*m_store);

    const auto created = directory.createInvite(inviter.account);
    QVERIFY(created.hasValue());
    QCOMPARE(created.value().size(), 32); // 32-byte plaintext token

    const QByteArray token = created.value();
    const auto redeemed = directory.redeemInvite(token);
    QVERIFY(redeemed.hasValue());
    QCOMPARE(redeemed.value().accountId, inviter.account);
    QCOMPARE(redeemed.value().devices.size(), 1);
    QCOMPARE(redeemed.value().devices.first().deviceId, inviter.device);
    QCOMPARE(redeemed.value().devices.first().signingKey, inviter.key.publicKey);

    // Single-use: a second redemption of the same token is indistinguishable
    // from an unknown token.
    const auto again = directory.redeemInvite(token);
    QVERIFY(!again.hasValue());
    QCOMPARE(again.error(), RelayError::NotFound);

    // A bogus/unknown token is likewise NotFound (no oracle).
    const auto bogus = directory.redeemInvite(randomBytes(32));
    QVERIFY(!bogus.hasValue());
    QCOMPARE(bogus.error(), RelayError::NotFound);
}

void RelayServicesTest::inviteExpiryRejectsRedemption()
{
    const auto inviter = registerDevice(QStringLiteral("gwen"));
    DirectoryService directory(*m_store);

    // A short-lived invite that lapses before it is redeemed.
    const auto created = directory.createInvite(inviter.account, 1'000);
    QVERIFY(created.hasValue());

    m_now += 2'000; // advance past the 1s TTL

    const auto expired = directory.redeemInvite(created.value());
    QVERIFY(!expired.hasValue());
    QCOMPARE(expired.error(), RelayError::NotFound);
}

void RelayServicesTest::directoryResolvesAccountToHandle()
{
    const auto frank = registerDevice(QStringLiteral("frank"));
    DirectoryService directory(*m_store);

    // The reverse lookup returns exactly the registered handle.
    const auto resolved = directory.resolveAccount(frank.account);
    QVERIFY(resolved.hasValue());
    QCOMPARE(resolved.value(), QStringLiteral("frank"));

    // An unknown account discloses nothing.
    const auto missing = directory.resolveAccount(AccountId::generate());
    QVERIFY(!missing.hasValue());
    QCOMPARE(missing.error(), RelayError::NotFound);

    // Once every device is revoked the account is undiscoverable in reverse too,
    // matching the forward lookup.
    AuthService auth(*m_store);
    QVERIFY(auth.revokeDevice(frank.device).hasValue());
    const auto after = directory.resolveAccount(frank.account);
    QVERIFY(!after.hasValue());
    QCOMPARE(after.error(), RelayError::NotFound);
}

QTEST_GUILESS_MAIN(RelayServicesTest)
#include "tst_relayservices.moc"
