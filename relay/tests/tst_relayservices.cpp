#include "RelayServer.h"
#include <QWebSocket>
#include <QCborArray>
#include <QNetworkRequest>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QCborMap>
#include <QSignalSpy>
#include "AuthService.h"
#include "CosmeticsService.h"
#include "DirectoryService.h"
#include "EnvelopeService.h"
#include "KeyPackageService.h"
#include "PostgresStore.h"
#include "RelayCrypto.h"

#include "domain/CosmeticRules.h"
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
    void cosmeticsStartWithOneCaseAndDropFromConnectedTime();
    void claimingIsIdempotentAndGrantsTheReward();
    void onlyOwnedItemsCanBeWornInTheirSlot();
    void loadoutsArePublicAndBounded();
    void aDeviceCollectionIsImportedOnce();
    void operatorGrantsReachEveryAccount();
    void connectedTimeDropsCasesOverTheWire();
    void challengeAuthenticationRoundTrip();
    void challengeReplayRejected();
    void challengeExpiryRejected();
    void refreshRotationAndReuseRevokesFamily();
    void livePresenceOfflineStorageAndReconnectReplay();
    void duplicateSendIsIdempotent();
    void offlineRecipientIsStoredAndResendIsIdempotent();
    void retentionSweepDropsExpiredAndUnreachableInboxes();
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
    void envelopeToARetiredDeviceIsRefusedAtOnce();
    void longDeliveriesTravelInSmallFrames();
    void directoryResolvesHandleToActiveDevices();
    void directoryExcludesRevokedDevices();
    void directoryResolvesAccountToHandle();
    void inviteRedeemsOnceAndReturnsInviter();
    void inviteExpiryRejectsRedemption();
    void registrationRequiresPasswordKey();
    void handlesAreCanonicalAndUnique();
    void passwordIsStoredOnlyAsSaltedHash();
    void passwordLoginEnrollsDeviceAndRetiresOthers();
    void passwordLoginFailuresAreIndistinguishable();
    void passwordLoginIsThrottledPerHandle();
    void passwordLoginDeviceIdRules();
    void passwordHashCostIsUpgradedOnLogin();

private:
    struct Registered {
        AccountId account = AccountId::generate();
        DeviceId device = DeviceId::generate();
        DeviceKey key;
        AuthTokens tokens;
    };

    Registered registerDevice(const QString &handle);

    // Stands in for the client's locally stretched password key: any 32 bytes,
    // deterministic per handle so a test can present the right or a wrong one.
    static QByteArray passwordKeyFor(const QString &handle)
    {
        return QCryptographicHash::hash("test password key:" + handle.toUtf8(),
                                        QCryptographicHash::Sha256);
    }

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
                                 QStringLiteral(":/relay/005_envelope_acceptances.sql"),
                                 QStringLiteral(":/relay/006_account_passwords.sql"),
                                 QStringLiteral(":/relay/007_cosmetics.sql")};
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
        "rate_limits, invites, envelope_acceptances, cosmetic_claims, cosmetic_cases, "
        "cosmetic_items, cosmetic_loadouts RESTART IDENTITY CASCADE")));
}

RelayServicesTest::Registered RelayServicesTest::registerDevice(const QString &handle)
{
    Registered reg;
    reg.key = generateDeviceKey();
    AuthService auth(*m_store);
    const auto registered = auth.registerAccount(reg.account, handle, reg.device, reg.key.publicKey,
                                                 QByteArray("mls-credential-blob"),
                                                 passwordKeyFor(handle));
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
                                 QByteArray("cred"), passwordKeyFor(QStringLiteral("bob")))
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
                                 reg.key.publicKey, QByteArray("cred"),
                                 passwordKeyFor(QStringLiteral("carol")))
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

void RelayServicesTest::livePresenceOfflineStorageAndReconnectReplay()
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
    // The recipient is offline, yet the envelope is accepted and stored for it.
    const auto offline = signedEnvelope(sender.key.pkey, sender.account, sender.device,
                                        recipient.device, "offline", m_now);
    local.sendBinaryMessage(encodeCanonical(offline));
    QTRY_COMPARE(replies.size(), 1);
    response = QCborValue::fromCbor(replies.takeFirst().first().toByteArray()).toArray();
    QCOMPARE(response.at(0).toInteger(), 1); // RelayAccepted
    QCOMPARE(response.at(1).toByteArray(), offline.envelopeId.bytes());
    // The sender goes away too; the relay holds the envelope on its own.
    local.close();
    QTRY_COMPARE(local.state(), QAbstractSocket::UnconnectedState);
    received.clear();
    peer.open(request(recipient.tokens.accessToken));
    QTRY_COMPARE(received.size(), 3); // the backlog, then what was sent while offline
    for (const auto &frame : received) {
        const auto delivery = QCborValue::fromCbor(frame.first().toByteArray()).toArray();
        QCOMPARE(delivery.at(0).toInteger(), 4);
    }
    const auto last = QCborValue::fromCbor(received.last().first().toByteArray()).toArray();
    QCOMPARE(last.at(2).toByteArray(), encodeCanonical(offline));
    peer.close();
    QTRY_COMPARE(peer.state(), QAbstractSocket::UnconnectedState);
}

void RelayServicesTest::offlineRecipientIsStoredAndResendIsIdempotent()
{
    const auto sender = registerDevice(QStringLiteral("retry_sender"));
    const auto recipient = registerDevice(QStringLiteral("retry_recipient"));
    EnvelopeService envelopes(*m_store);
    const auto envelope = signedEnvelope(sender.key.pkey, sender.account, sender.device,
                                         recipient.device, "hello", m_now);
    const auto bytes = encodeCanonical(envelope);
    const AuthenticatedDevice identity{sender.account, sender.device};
    // Nobody is connected: the envelope waits in the recipient's inbox.
    const auto accepted = envelopes.submit(identity, bytes);
    QVERIFY(accepted.hasValue());
    QVERIFY(!accepted.value().duplicate);
    const auto waiting = envelopes.fetchSince(recipient.device, 0, 100);
    QVERIFY(waiting.hasValue());
    QCOMPARE(waiting.value().items.size(), 1);
    QCOMPARE(waiting.value().items.first().envelope, bytes);
    // Once received, a resend whose acceptance the sender never saw is a
    // duplicate: it reports the original sequence and is not delivered again.
    QVERIFY(envelopes.acknowledge(recipient.device, accepted.value().serverSequence).hasValue());
    const auto retry = envelopes.submit(identity, bytes);
    QVERIFY(retry.hasValue());
    QVERIFY(retry.value().duplicate);
    QCOMPARE(retry.value().serverSequence, accepted.value().serverSequence);
    QVERIFY(envelopes.fetchSince(recipient.device, 0, 100).value().items.isEmpty());
}

void RelayServicesTest::retentionSweepDropsExpiredAndUnreachableInboxes()
{
    const auto sender = registerDevice(QStringLiteral("sweep_sender"));
    const auto waiting = registerDevice(QStringLiteral("sweep_waiting"));
    const auto retired = registerDevice(QStringLiteral("sweep_retired"));
    AuthService auth(*m_store);
    EnvelopeService envelopes(*m_store);
    const AuthenticatedDevice identity{sender.account, sender.device};
    const auto submit = [&](const DeviceId &recipient, const QByteArray &ciphertext) {
        const auto envelope = signedEnvelope(sender.key.pkey, sender.account, sender.device,
                                             recipient, ciphertext, m_now);
        const QByteArray bytes = encodeCanonical(envelope);
        const auto stored = envelopes.submit(identity, bytes);
        return stored.hasValue() ? bytes : QByteArray();
    };

    // signedEnvelope expires 60 s after creation.
    QVERIFY(!submit(waiting.device, "expires first").isEmpty());
    m_now += 30'000;
    const QByteArray stillValid = submit(waiting.device, "still valid");
    QVERIFY(!stillValid.isEmpty());
    QVERIFY(!submit(retired.device, "never fetched").isEmpty());
    QVERIFY(auth.revokeDevice(retired.device).hasValue());

    // Neither recipient ever connects or acknowledges. Past the first expiry the
    // sweep drops that envelope and the retired device's inbox, and leaves the
    // unexpired one waiting for its recipient.
    m_now += 45'000;
    const auto pruned = envelopes.pruneExpired();
    QVERIFY(pruned.hasValue());
    QCOMPARE(pruned.value(), 2);
    const auto remaining = envelopes.fetchSince(waiting.device, 0, 100);
    QVERIFY(remaining.hasValue());
    QCOMPARE(remaining.value().items.size(), 1);
    QCOMPARE(remaining.value().items.first().envelope, stillValid);
    QCOMPARE(envelopes.pruneExpired().value(), 0);
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

void RelayServicesTest::envelopeToARetiredDeviceIsRefusedAtOnce()
{
    const auto sender = registerDevice(QStringLiteral("nack_sender"));
    const auto retired = registerDevice(QStringLiteral("nack_retired"));
    AuthService auth(*m_store);
    EnvelopeService envelopes(*m_store);
    KeyPackageService packages(*m_store);
    DirectoryService directory(*m_store);
    QVERIFY(auth.revokeDevice(retired.device).hasValue());
    RelayServer server(*m_store, auth, envelopes, packages, directory, RelayServer::Limits{}, nullptr);
    const auto port = server.start(QHostAddress::LocalHost, 0);
    QVERIFY(port);
    QNetworkRequest request(QUrl(QStringLiteral("ws://127.0.0.1:%1/v1/live?since=0").arg(port)));
    request.setRawHeader("Authorization", "Bearer " + sender.tokens.accessToken);
    QWebSocket socket;
    QSignalSpy replies(&socket, &QWebSocket::binaryMessageReceived);
    socket.open(request);
    QTRY_COMPARE(socket.state(), QAbstractSocket::ConnectedState);

    // A retired device and one that never existed will never take it: the
    // sender hears so at once instead of retrying for minutes.
    for (const DeviceId &recipient : {retired.device, DeviceId::generate()}) {
        replies.clear();
        const auto envelope = signedEnvelope(sender.key.pkey, sender.account, sender.device, recipient,
                                             "to nobody", m_now);
        socket.sendBinaryMessage(encodeCanonical(envelope));
        QTRY_COMPARE(replies.size(), 1);
        const auto reply = QCborValue::fromCbor(replies.first().first().toByteArray()).toArray();
        QCOMPARE(reply.at(0).toInteger(), 9); // RecipientUnavailable
        QCOMPARE(reply.at(1).toByteArray(), envelope.envelopeId.bytes());
    }
    socket.close();
    QTRY_COMPARE(socket.state(), QAbstractSocket::UnconnectedState);
}

void RelayServicesTest::longDeliveriesTravelInSmallFrames()
{
    // The client counts every frame as a sign of life, so a long catch-up on
    // a slow downlink must not arrive as one frame per envelope.
    const auto sender = registerDevice(QStringLiteral("frames_sender"));
    const auto recipient = registerDevice(QStringLiteral("frames_recipient"));
    AuthService auth(*m_store);
    EnvelopeService envelopes(*m_store);
    KeyPackageService packages(*m_store);
    DirectoryService directory(*m_store);
    const auto envelope = signedEnvelope(sender.key.pkey, sender.account, sender.device, recipient.device,
                                         QByteArray(200 * 1024, 'p'), m_now);
    QVERIFY(envelopes.submit({sender.account, sender.device}, encodeCanonical(envelope)).hasValue());
    RelayServer server(*m_store, auth, envelopes, packages, directory, RelayServer::Limits{}, nullptr);
    const auto port = server.start(QHostAddress::LocalHost, 0);
    QVERIFY(port);
    QNetworkRequest request(QUrl(QStringLiteral("ws://127.0.0.1:%1/v1/live?since=0").arg(port)));
    request.setRawHeader("Authorization", "Bearer " + recipient.tokens.accessToken);
    QWebSocket socket;
    int frames = 0;
    connect(&socket, &QWebSocket::binaryFrameReceived, &socket, [&frames](const QByteArray &, bool) { ++frames; });
    QSignalSpy messages(&socket, &QWebSocket::binaryMessageReceived);
    socket.open(request);
    QTRY_COMPARE(messages.size(), 1);
    QVERIFY2(frames >= 12, qPrintable(QString::number(frames)));
    socket.close();
    QTRY_COMPARE(socket.state(), QAbstractSocket::UnconnectedState);
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

void RelayServicesTest::registrationRequiresPasswordKey()
{
    AuthService auth(*m_store);
    const DeviceKey key = generateDeviceKey();
    const QByteArray good = passwordKeyFor(QStringLiteral("paula"));

    // A pre-password client sends no key at all; a truncated or oversized key and
    // an unknown client-side stretch version are refused the same way.
    const QList<QByteArray> badKeys{QByteArray(), good.left(31), good + QByteArray(1, 'x')};
    for (const QByteArray &bad : badKeys) {
        const auto result = auth.registerAccount(AccountId::generate(), QStringLiteral("paula"),
                                                 DeviceId::generate(), key.publicKey,
                                                 QByteArray("cred"), bad);
        QVERIFY(!result.hasValue());
        QCOMPARE(result.error(), RelayError::InvalidRequest);
    }
    const auto wrongVersion =
        auth.registerAccount(AccountId::generate(), QStringLiteral("paula"), DeviceId::generate(),
                             key.publicKey, QByteArray("cred"), good,
                             AuthService::supportedPasswordKdf + 1);
    QVERIFY(!wrongVersion.hasValue());
    QCOMPARE(wrongVersion.error(), RelayError::InvalidRequest);

    // None of the refusals reserved the handle.
    QVERIFY(auth.registerAccount(AccountId::generate(), QStringLiteral("paula"),
                                 DeviceId::generate(), key.publicKey, QByteArray("cred"), good)
                .hasValue());
}

void RelayServicesTest::handlesAreCanonicalAndUnique()
{
    AuthService auth(*m_store);
    DirectoryService directory(*m_store);
    const DeviceKey key = generateDeviceKey();
    const auto attempt = [&](const QString &handle) {
        return auth.registerAccount(AccountId::generate(), handle, DeviceId::generate(),
                                    key.publicKey, QByteArray("cred"), passwordKeyFor(handle));
    };

    // Input is canonicalized before it is stored...
    QVERIFY(attempt(QStringLiteral("  @Victor.V-1 ")).hasValue());
    QSqlQuery stored(m_store->database());
    QVERIFY(stored.exec(QStringLiteral("SELECT handle FROM accounts")));
    QVERIFY(stored.next());
    QCOMPARE(stored.value(0).toString(), QStringLiteral("victor.v-1"));

    // ...so every spelling of a taken handle is a conflict.
    for (const QString &again : {QStringLiteral("victor.v-1"), QStringLiteral("VICTOR.V-1"),
                                 QStringLiteral("@Victor.v-1")}) {
        const auto clash = attempt(again);
        QVERIFY(!clash.hasValue());
        QCOMPARE(clash.error(), RelayError::Conflict);
    }

    // Anything outside the ASCII handle alphabet is refused outright, which is
    // what makes two distinct handles impossible to confuse visually.
    for (const QString &invalid :
         {QStringLiteral("ab"), QStringLiteral("has space"), QStringLiteral("-leading"),
          QStringLiteral(".leading"), QString::fromUtf8("j\xC3\xB6hn"),
          QString::fromUtf8("v\xD1\x96" "ctor"), // Cyrillic look-alike of "victor"
          QString(33, QLatin1Char('a')), QStringLiteral("semi;colon")}) {
        const auto refused = attempt(invalid);
        QVERIFY2(!refused.hasValue(), qPrintable(invalid));
        QCOMPARE(refused.error(), RelayError::InvalidRequest);
    }

    // Lookups canonicalize the same way.
    QVERIFY(directory.resolveHandle(QStringLiteral("@VICTOR.V-1")).hasValue());

    // A mixed-case handle from before handles were canonical still reserves its
    // lowercase form, and can still be resolved.
    QSqlQuery legacy(m_store->database());
    legacy.prepare(QStringLiteral(
        "INSERT INTO accounts (account_id, handle, created_at_ms) VALUES (?, 'OldZed', 1)"));
    const AccountId legacyAccount = AccountId::generate();
    legacy.addBindValue(legacyAccount.bytes());
    QVERIFY(legacy.exec());
    const auto shadowed = attempt(QStringLiteral("oldzed"));
    QVERIFY(!shadowed.hasValue());
    QCOMPARE(shadowed.error(), RelayError::Conflict);
}

void RelayServicesTest::passwordIsStoredOnlyAsSaltedHash()
{
    const QByteArray key = passwordKeyFor(QStringLiteral("hashed1"));
    const auto first = registerDevice(QStringLiteral("hashed1"));
    Q_UNUSED(first);

    // A second account that happens to use the very same password key.
    AuthService auth(*m_store);
    const DeviceKey otherKey = generateDeviceKey();
    QVERIFY(auth.registerAccount(AccountId::generate(), QStringLiteral("hashed2"),
                                 DeviceId::generate(), otherKey.publicKey, QByteArray("cred"), key)
                .hasValue());

    QSqlQuery rows(m_store->database());
    QVERIFY(rows.exec(QStringLiteral(
        "SELECT password_hash, password_salt, password_params, password_kdf FROM accounts "
        "ORDER BY handle")));
    QList<QByteArray> hashes;
    QList<QByteArray> salts;
    while (rows.next()) {
        hashes.append(rows.value(0).toByteArray());
        salts.append(rows.value(1).toByteArray());
        QCOMPARE(rows.value(2).toString(),
                 QString::fromLatin1(PasswordHashParams{}.serialize()));
        QCOMPARE(rows.value(3).toInt(), AuthService::supportedPasswordKdf);
    }
    QCOMPARE(hashes.size(), 2);
    // The key itself is never at rest, and equal keys do not produce equal rows.
    QVERIFY(!hashes.contains(key));
    QVERIFY(hashes.at(0) != hashes.at(1));
    QVERIFY(salts.at(0) != salts.at(1));
    QCOMPARE(hashes.at(0).size(), 32);
    QCOMPARE(salts.at(0).size(), 16);
}

void RelayServicesTest::passwordLoginEnrollsDeviceAndRetiresOthers()
{
    const QString handle = QStringLiteral("wendy");
    const auto original = registerDevice(handle);
    AuthService auth(*m_store);
    DirectoryService directory(*m_store);
    QVERIFY(auth.authenticate(original.tokens.accessToken).has_value());

    // A fresh installation: new device key, no tokens, only handle + password.
    const DeviceKey newKey = generateDeviceKey();
    const DeviceId newDevice = DeviceId::generate();
    const auto login = auth.loginWithPassword(QStringLiteral("@Wendy"), passwordKeyFor(handle),
                                              AuthService::supportedPasswordKdf, newDevice,
                                              newKey.publicKey, QByteArray("new-credential"));
    QVERIFY(login.hasValue());
    QCOMPARE(login.value().accountId, original.account);
    QCOMPARE(login.value().handle, handle);
    QCOMPARE(login.value().retiredDevices.size(), 1);
    QCOMPARE(login.value().retiredDevices.first(), original.device);

    // The login alone grants nothing: tokens still require the signed challenge,
    // which the new device can pass and the retired one no longer can.
    const auto challenge = auth.issueChallenge(original.account, newDevice, 1);
    QVERIFY(challenge.hasValue());
    const QByteArray context("account-device-bind");
    const auto tokens = auth.completeChallenge(
        original.account, newDevice, challenge.value(),
        signWith(newKey.pkey, challengeSigningMessage(challenge.value(), context)), context);
    QVERIFY(tokens.hasValue());
    QVERIFY(auth.authenticate(tokens.value().accessToken).has_value());

    QVERIFY(!auth.authenticate(original.tokens.accessToken).has_value());
    const auto retiredRefresh = auth.refresh(original.tokens.refreshToken);
    QVERIFY(!retiredRefresh.hasValue());
    const auto retiredChallenge = auth.issueChallenge(original.account, original.device, 1);
    QVERIFY(!retiredChallenge.hasValue());
    QCOMPARE(retiredChallenge.error(), RelayError::Revoked);

    // Contacts resolving the handle now see exactly the new device.
    const auto resolved = directory.resolveHandle(handle);
    QVERIFY(resolved.hasValue());
    QCOMPARE(resolved.value().devices.size(), 1);
    QCOMPARE(resolved.value().devices.first().deviceId, newDevice);
}

void RelayServicesTest::passwordLoginFailuresAreIndistinguishable()
{
    const auto real = registerDevice(QStringLiteral("xavier"));
    AuthService auth(*m_store);
    const DeviceKey key = generateDeviceKey();

    // An account from before passwords existed: it has a handle but no hash.
    QSqlQuery legacy(m_store->database());
    legacy.prepare(QStringLiteral(
        "INSERT INTO accounts (account_id, handle, created_at_ms) VALUES (?, 'legacyuser', 1)"));
    legacy.addBindValue(AccountId::generate().bytes());
    QVERIFY(legacy.exec());

    struct Case {
        QString handle;
        QByteArray key;
        int kdf;
    };
    const QList<Case> cases{
        {QStringLiteral("xavier"), passwordKeyFor(QStringLiteral("not-xavier")), 1}, // wrong key
        {QStringLiteral("nobody-here"), passwordKeyFor(QStringLiteral("xavier")), 1}, // unknown
        {QStringLiteral("legacyuser"), passwordKeyFor(QStringLiteral("legacyuser")), 1},
        {QStringLiteral("xavier"), passwordKeyFor(QStringLiteral("xavier")), 2}, // other stretch
        {QString::fromUtf8("x\xC3\xA4vier"), passwordKeyFor(QStringLiteral("xavier")), 1},
    };
    for (const Case &c : cases) {
        const auto result = auth.loginWithPassword(c.handle, c.key, c.kdf, DeviceId::generate(),
                                                   key.publicKey, QByteArray("cred"));
        QVERIFY2(!result.hasValue(), qPrintable(c.handle));
        QCOMPARE(result.error(), RelayError::Unauthorized);
    }

    // No failed attempt enrolled a device or disturbed the real one.
    QSqlQuery count(m_store->database());
    QVERIFY(count.exec(QStringLiteral("SELECT count(*) FROM devices")));
    QVERIFY(count.next());
    QCOMPARE(count.value(0).toInt(), 1);
    QVERIFY(auth.authenticate(real.tokens.accessToken).has_value());

    // Malformed material is a caller bug, not a credential failure.
    const auto malformed = auth.loginWithPassword(
        QStringLiteral("xavier"), QByteArray(31, 'k'), 1, DeviceId::generate(), key.publicKey,
        QByteArray("cred"));
    QVERIFY(!malformed.hasValue());
    QCOMPARE(malformed.error(), RelayError::InvalidRequest);
}

void RelayServicesTest::passwordLoginIsThrottledPerHandle()
{
    const QString handle = QStringLiteral("yolanda");
    const auto reg = registerDevice(handle);
    Q_UNUSED(reg);
    const auto bystander = registerDevice(QStringLiteral("zachary"));
    Q_UNUSED(bystander);

    AuthService::Policy policy;
    policy.maxLoginFailures = 3;
    AuthService auth(*m_store, policy);
    const DeviceKey key = generateDeviceKey();
    const auto tryLogin = [&](const QString &who, const QByteArray &passwordKey) {
        return auth.loginWithPassword(who, passwordKey, 1, DeviceId::generate(), key.publicKey,
                                      QByteArray("cred"));
    };

    for (int i = 0; i < policy.maxLoginFailures; ++i) {
        const auto wrong = tryLogin(handle, passwordKeyFor(QStringLiteral("guess")));
        QCOMPARE(wrong.error(), RelayError::Unauthorized);
    }
    // The budget is spent: even the correct password is not evaluated any more.
    const auto locked = tryLogin(handle, passwordKeyFor(handle));
    QVERIFY(!locked.hasValue());
    QCOMPARE(locked.error(), RelayError::RateLimited);

    // The throttle is per handle; another account is unaffected.
    QVERIFY(tryLogin(QStringLiteral("zachary"), passwordKeyFor(QStringLiteral("zachary")))
                .hasValue());

    // Guessing at an unknown handle is budgeted identically, so the throttle is
    // not itself an oracle for which handles exist.
    for (int i = 0; i < policy.maxLoginFailures; ++i)
        QCOMPARE(tryLogin(QStringLiteral("ghost-user"), passwordKeyFor(handle)).error(),
                 RelayError::Unauthorized);
    QCOMPARE(tryLogin(QStringLiteral("ghost-user"), passwordKeyFor(handle)).error(),
             RelayError::RateLimited);

    // The table never names the handles that were tried.
    QSqlQuery subjects(m_store->database());
    QVERIFY(subjects.exec(QStringLiteral("SELECT subject FROM rate_limits")));
    while (subjects.next()) {
        QCOMPARE(subjects.value(0).toByteArray().size(), 32);
        QVERIFY(!subjects.value(0).toByteArray().contains("yolanda"));
    }

    // A new window restores the budget, and a success clears the count.
    m_now += policy.loginWindowMs;
    QVERIFY(tryLogin(handle, passwordKeyFor(handle)).hasValue());
    QSqlQuery remaining(m_store->database());
    remaining.prepare(QStringLiteral("SELECT count(*) FROM rate_limits WHERE subject = ?"));
    remaining.addBindValue(QCryptographicHash::hash("openchat login v1:yolanda",
                                                    QCryptographicHash::Sha256));
    QVERIFY(remaining.exec());
    QVERIFY(remaining.next());
    QCOMPARE(remaining.value(0).toInt(), 0);
}

void RelayServicesTest::passwordLoginDeviceIdRules()
{
    const QString handle = QStringLiteral("ursula");
    const auto reg = registerDevice(handle);
    Q_UNUSED(reg);
    const auto other = registerDevice(QStringLiteral("umberto"));
    AuthService auth(*m_store);

    const DeviceKey key = generateDeviceKey();
    const DeviceId device = DeviceId::generate();
    const auto first = auth.loginWithPassword(handle, passwordKeyFor(handle), 1, device,
                                              key.publicKey, QByteArray("cred"));
    QVERIFY(first.hasValue());

    // A retry after a lost response presents the same device: accepted, and it
    // must not retire the device it just enrolled.
    const auto retry = auth.loginWithPassword(handle, passwordKeyFor(handle), 1, device,
                                              key.publicKey, QByteArray("cred"));
    QVERIFY(retry.hasValue());
    QVERIFY(retry.value().retiredDevices.isEmpty());
    QVERIFY(!auth.isDeviceRevoked(device));

    // The same device id under a different key, or a device id that belongs to
    // another account, is a conflict -- and must leave both accounts untouched.
    const DeviceKey impostor = generateDeviceKey();
    const auto rekeyed = auth.loginWithPassword(handle, passwordKeyFor(handle), 1, device,
                                                impostor.publicKey, QByteArray("cred"));
    QVERIFY(!rekeyed.hasValue());
    QCOMPARE(rekeyed.error(), RelayError::Conflict);
    const auto stolen = auth.loginWithPassword(handle, passwordKeyFor(handle), 1, other.device,
                                               key.publicKey, QByteArray("cred"));
    QVERIFY(!stolen.hasValue());
    QCOMPARE(stolen.error(), RelayError::Conflict);
    QVERIFY(!auth.isDeviceRevoked(device));
    QVERIFY(!auth.isDeviceRevoked(other.device));
}

void RelayServicesTest::passwordHashCostIsUpgradedOnLogin()
{
    const QString handle = QStringLiteral("tobias");
    AuthService::Policy cheap;
    cheap.passwordParams.memoryKiB = 8 * 1024;
    cheap.passwordParams.iterations = 1;
    const DeviceKey key = generateDeviceKey();
    {
        AuthService old(*m_store, cheap);
        QVERIFY(old.registerAccount(AccountId::generate(), handle, DeviceId::generate(),
                                    key.publicKey, QByteArray("cred"), passwordKeyFor(handle))
                    .hasValue());
    }
    const auto paramsNow = [&] {
        QSqlQuery row(m_store->database());
        row.exec(QStringLiteral("SELECT password_params FROM accounts WHERE handle = 'tobias'"));
        row.next();
        return row.value(0).toString();
    };
    QCOMPARE(paramsNow(), QString::fromLatin1(cheap.passwordParams.serialize()));

    // A relay running today's cost verifies against the row's own cost, then
    // re-hashes at the current one; the password keeps working afterwards.
    AuthService current(*m_store);
    const DeviceKey newKey = generateDeviceKey();
    QVERIFY(current.loginWithPassword(handle, passwordKeyFor(handle), 1, DeviceId::generate(),
                                      newKey.publicKey, QByteArray("cred"))
                .hasValue());
    QCOMPARE(paramsNow(), QString::fromLatin1(PasswordHashParams{}.serialize()));
    QVERIFY(current.loginWithPassword(handle, passwordKeyFor(handle), 1, DeviceId::generate(),
                                      newKey.publicKey, QByteArray("cred"))
                .hasValue());

    // Stored parameters are bounded, so a corrupted row cannot demand an
    // unbounded allocation from the relay.
    QVERIFY(!PasswordHashParams::parse("argon2id$m=99999999,t=2").has_value());
    QVERIFY(!PasswordHashParams::parse("argon2id$m=19456,t=0").has_value());
    QVERIFY(!PasswordHashParams::parse("scrypt$n=1").has_value());
}

void RelayServicesTest::cosmeticsStartWithOneCaseAndDropFromConnectedTime()
{
    const auto alice = registerDevice(QStringLiteral("cosmetic_alice"));
    CosmeticsService cosmetics(*m_store);
    const qint64 interval = cosmetics.dropIntervalMs();
    QCOMPARE(interval, qint64(30 * 60'000));

    // A new account has one case waiting, nothing owned or worn, and nothing
    // opened: the case on offer is the first.
    const auto fresh = cosmetics.state(alice.account);
    QVERIFY(fresh.hasValue());
    QCOMPARE(fresh.value().drops, 1);
    QCOMPARE(fresh.value().progressMs, 0);
    QCOMPARE(fresh.value().dropIntervalMs, interval);
    QVERIFY(fresh.value().owned.isEmpty());
    QVERIFY(fresh.value().loadout.isEmpty());
    QVERIFY(!fresh.value().last);
    QCOMPARE(fresh.value().nextCaseKey, QStringLiteral("first"));
    QVERIFY(!fresh.value().imported);

    // Connected time counts toward the next case; every full interval drops
    // one and the remainder carries on.
    QCOMPARE(cosmetics.accrue(alice.account, interval / 2).value().progressMs, interval / 2);
    const auto dropped = cosmetics.accrue(alice.account, interval + interval * 3 / 4);
    QCOMPARE(dropped.value().drops, 3);
    QCOMPARE(dropped.value().progressMs, interval / 4);
    QCOMPARE(cosmetics.accrue(alice.account, 0).value().drops, 3);

    // An account the relay does not know has no cases.
    QCOMPARE(cosmetics.state(AccountId::generate()).error(), RelayError::NotFound);
}

void RelayServicesTest::claimingIsIdempotentAndGrantsTheReward()
{
    const auto alice = registerDevice(QStringLiteral("cosmetic_claimer"));
    CosmeticsService cosmetics(*m_store);
    const QByteArray request = QByteArray(16, 'a');
    const auto first = cosmetics.claim(alice.account, request);
    QVERIFY(first.hasValue());
    QVERIFY(first.value().newlyClaimed);
    const CosmeticClaim claim = first.value().claim;
    QVERIFY(CosmeticRules::find(claim.rewardId));
    QCOMPARE(claim.claimId.size(), 16);
    QCOMPARE(claim.caseKey, QStringLiteral("first"));
    // The case is spent, the reward owned, and it is the last one opened.
    QCOMPARE(first.value().state.drops, 0);
    QCOMPARE(first.value().state.owned, QStringList{claim.rewardId});
    QCOMPARE(first.value().state.last->claimId, claim.claimId);
    QCOMPARE(first.value().state.nextCaseKey,
             QStringLiteral("after ") + QString::fromLatin1(claim.claimId.toHex()));

    // A retry after a lost response returns the same claim and spends nothing.
    const auto retry = cosmetics.claim(alice.account, request);
    QVERIFY(retry.hasValue());
    QVERIFY(!retry.value().newlyClaimed);
    QCOMPARE(retry.value().claim.claimId, claim.claimId);
    QCOMPARE(retry.value().claim.rewardId, claim.rewardId);
    QCOMPARE(retry.value().claim.seed, claim.seed);
    QCOMPARE(retry.value().state.drops, 0);

    // With nothing waiting, a new claim is refused; malformed ids are rejected.
    QCOMPARE(cosmetics.claim(alice.account, QByteArray(16, 'b')).error(), RelayError::Conflict);
    QCOMPARE(cosmetics.claim(alice.account, QByteArray(4, 'c')).error(), RelayError::InvalidRequest);

    // The next case is offered under the last one's key.
    QVERIFY(cosmetics.accrue(alice.account, cosmetics.dropIntervalMs()).hasValue());
    const auto second = cosmetics.claim(alice.account, QByteArray(16, 'b'));
    QVERIFY(second.value().newlyClaimed);
    QCOMPARE(second.value().claim.caseKey, first.value().state.nextCaseKey);
    QVERIFY(second.value().state.owned.contains(second.value().claim.rewardId));
}

void RelayServicesTest::onlyOwnedItemsCanBeWornInTheirSlot()
{
    const auto alice = registerDevice(QStringLiteral("cosmetic_wearer"));
    CosmeticsService cosmetics(*m_store);
    QVERIFY(cosmetics.grantDrops(1, alice.account).hasValue());
    QVERIFY(cosmetics.importCollection(alice.account, {QStringLiteral("frame.neon"),
                                                       QStringLiteral("bead.gem")}, 0).hasValue());
    // Not owned: refused.
    QCOMPARE(cosmetics.equip(alice.account, QStringLiteral("frame"), QStringLiteral("frame.inferno")).error(),
             RelayError::NotFound);
    // Owned, but not a frame; an unknown slot or item: invalid.
    QCOMPARE(cosmetics.equip(alice.account, QStringLiteral("frame"), QStringLiteral("bead.gem")).error(),
             RelayError::InvalidRequest);
    QCOMPARE(cosmetics.equip(alice.account, QStringLiteral("hat"), QStringLiteral("frame.neon")).error(),
             RelayError::InvalidRequest);
    QCOMPARE(cosmetics.equip(alice.account, QStringLiteral("frame"), QStringLiteral("frame.retired")).error(),
             RelayError::InvalidRequest);
    // Owned and in its slot: worn, then cleared.
    const auto worn = cosmetics.equip(alice.account, QStringLiteral("frame"), QStringLiteral("frame.neon"));
    QVERIFY(worn.hasValue());
    QCOMPARE(worn.value().loadout.value(QStringLiteral("frame")), QStringLiteral("frame.neon"));
    QVERIFY(cosmetics.equip(alice.account, QStringLiteral("bead"), QStringLiteral("bead.gem")).hasValue());
    const auto cleared = cosmetics.equip(alice.account, QStringLiteral("frame"), QString());
    QVERIFY(!cleared.value().loadout.contains(QStringLiteral("frame")));
    QCOMPARE(cleared.value().loadout.value(QStringLiteral("bead")), QStringLiteral("bead.gem"));
}

void RelayServicesTest::loadoutsArePublicAndBounded()
{
    const auto alice = registerDevice(QStringLiteral("cosmetic_shown"));
    const auto bob = registerDevice(QStringLiteral("cosmetic_viewer"));
    CosmeticsService cosmetics(*m_store);
    QVERIFY(cosmetics.importCollection(alice.account, {QStringLiteral("scene.aurora")}, 0).hasValue());
    QVERIFY(cosmetics.equip(alice.account, QStringLiteral("scene"), QStringLiteral("scene.aurora")).hasValue());
    // Anyone may read what others wear; those wearing nothing are absent.
    const auto read = cosmetics.loadouts({alice.account, bob.account, AccountId::generate()});
    QVERIFY(read.hasValue());
    QCOMPARE(read.value().size(), 1);
    QCOMPARE(read.value().value(alice.account).value(QStringLiteral("scene")), QStringLiteral("scene.aurora"));
    QVERIFY(cosmetics.loadouts({}).value().isEmpty());
    QList<AccountId> tooMany;
    for (int i = 0; i < 257; ++i)
        tooMany.append(AccountId::generate());
    QCOMPARE(cosmetics.loadouts(tooMany).error(), RelayError::InvalidRequest);
}

void RelayServicesTest::aDeviceCollectionIsImportedOnce()
{
    const auto alice = registerDevice(QStringLiteral("cosmetic_importer"));
    CosmeticsService cosmetics(*m_store);
    // Known items come across, unknown ones do not, and waiting cases are
    // capped (never below what the account already has).
    const auto imported = cosmetics.importCollection(
        alice.account, {QStringLiteral("bubble.magma"), QStringLiteral("frame.orbit"),
                        QStringLiteral("frame.retired"), QStringLiteral("bubble.magma")}, 50);
    QVERIFY(imported.hasValue());
    QVERIFY(imported.value().imported);
    QCOMPARE(imported.value().drops, 10);
    QCOMPARE(imported.value().owned.size(), 2);
    QVERIFY(imported.value().owned.contains(QStringLiteral("bubble.magma")));
    // A second import changes nothing.
    const auto again = cosmetics.importCollection(alice.account, {QStringLiteral("scene.sakura")}, 5);
    QCOMPARE(again.value().drops, 10);
    QCOMPARE(again.value().owned.size(), 2);
    // An import with nothing waiting keeps the welcome case.
    const auto bob = registerDevice(QStringLiteral("cosmetic_importer_two"));
    QCOMPARE(cosmetics.importCollection(bob.account, {}, 0).value().drops, 1);
}

void RelayServicesTest::operatorGrantsReachEveryAccount()
{
    const auto alice = registerDevice(QStringLiteral("cosmetic_granted_one"));
    const auto bob = registerDevice(QStringLiteral("cosmetic_granted_two"));
    CosmeticsService cosmetics(*m_store);
    // Alice has spent her welcome case; Bob has never asked.
    QVERIFY(cosmetics.claim(alice.account, QByteArray(16, 'g')).hasValue());
    const auto everyone = cosmetics.grantDrops(5);
    QVERIFY(everyone.hasValue());
    QCOMPARE(everyone.value(), 2);
    QCOMPARE(cosmetics.state(alice.account).value().drops, 5);
    QCOMPARE(cosmetics.state(bob.account).value().drops, 6);
    // One account by id; bounds are enforced.
    QCOMPARE(cosmetics.grantDrops(2, bob.account).value(), 1);
    QCOMPARE(cosmetics.state(bob.account).value().drops, 8);
    QCOMPARE(cosmetics.grantDrops(0).error(), RelayError::InvalidRequest);
    QCOMPARE(cosmetics.grantDrops(1001).error(), RelayError::InvalidRequest);
    QCOMPARE(cosmetics.grantDrops(1, AccountId::generate()).error(), RelayError::NotFound);
}

void RelayServicesTest::connectedTimeDropsCasesOverTheWire()
{
    const auto alice = registerDevice(QStringLiteral("cosmetic_online"));
    const auto bob = registerDevice(QStringLiteral("cosmetic_onlooker"));
    AuthService auth(*m_store);
    EnvelopeService envelopes(*m_store);
    KeyPackageService packages(*m_store);
    DirectoryService directory(*m_store);
    CosmeticsService::Policy policy;
    policy.dropIntervalMs = 400;
    CosmeticsService cosmetics(*m_store, policy);
    RelayServer server(*m_store, auth, envelopes, packages, directory);
    server.setCosmetics(&cosmetics);
    const auto port = server.start(QHostAddress::LocalHost, 0);
    QVERIFY(port);

    const auto http = [port](const QString &path, const QByteArray &token) {
        QNetworkRequest req(QUrl(QStringLiteral("http://127.0.0.1:%1/v1/").arg(port) + path));
        req.setRawHeader("Authorization", "Bearer " + token);
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/cbor");
        return req;
    };
    QNetworkAccessManager network;
    const auto finish = [](QNetworkReply *reply) {
        QTest::qWaitFor([reply] { return reply->isFinished(); }, 5000);
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QCborMap body = QCborValue::fromCbor(reply->readAll()).toMap();
        reply->deleteLater();
        return std::pair{status, body};
    };

    // Unauthenticated callers get nothing.
    QCOMPARE(finish(network.get(http(QStringLiteral("cosmetics"), "invalid"))).first, 401);

    // Connected, an opted-in client hears where it stands at once...
    QWebSocket socket;
    QSignalSpy frames(&socket, &QWebSocket::binaryMessageReceived);
    QNetworkRequest live(QUrl(QStringLiteral("ws://127.0.0.1:%1/v1/live?cosmetics=1").arg(port)));
    live.setRawHeader("Authorization", "Bearer " + alice.tokens.accessToken);
    socket.open(live);
    QTRY_COMPARE(frames.size(), 1);
    auto frame = QCborValue::fromCbor(frames.takeFirst().at(0).toByteArray()).toArray();
    QCOMPARE(frame.at(0).toInteger(), 14);
    QCOMPARE(frame.at(1).toMap().value(QLatin1StringView("drops")).toInteger(), 1);
    QCOMPARE(frame.at(1).toMap().value(QLatin1StringView("interval_ms")).toInteger(), 400);
    // ...and a case drops, and is pushed, for each interval it stays.
    QTRY_VERIFY_WITH_TIMEOUT(!frames.isEmpty(), 3000);
    frame = QCborValue::fromCbor(frames.takeFirst().at(0).toByteArray()).toArray();
    QCOMPARE(frame.at(0).toInteger(), 14);
    QCOMPARE(frame.at(1).toMap().value(QLatin1StringView("drops")).toInteger(), 2);
    QTRY_VERIFY_WITH_TIMEOUT(!frames.isEmpty(), 3000);
    frame = QCborValue::fromCbor(frames.takeFirst().at(0).toByteArray()).toArray();
    QCOMPARE(frame.at(1).toMap().value(QLatin1StringView("drops")).toInteger(), 3);

    // Opening one over HTTP is pushed too.
    QCborMap claim;
    claim.insert(QLatin1StringView("request_id"), QByteArray(16, 'w'));
    auto [claimStatus, claimed] = finish(network.post(http(QStringLiteral("cosmetics/claim"),
                                                           alice.tokens.accessToken),
                                                      claim.toCborValue().toCbor()));
    QCOMPARE(claimStatus, 200);
    QVERIFY(claimed.value(QLatin1StringView("newly_claimed")).toBool());
    const QString reward = claimed.value(QLatin1StringView("claim")).toMap()
                               .value(QLatin1StringView("reward_id")).toString();
    QVERIFY(CosmeticRules::find(reward));
    QTRY_VERIFY(!frames.isEmpty());
    frames.clear();

    // Wearing it, and another account seeing it.
    QCborMap equip;
    equip.insert(QLatin1StringView("slot"), QString::fromLatin1(CosmeticRules::find(reward)->slot));
    equip.insert(QLatin1StringView("item_id"), reward);
    auto [equipStatus, equipped] = finish(network.post(http(QStringLiteral("cosmetics/equip"),
                                                            alice.tokens.accessToken),
                                                       equip.toCborValue().toCbor()));
    QCOMPARE(equipStatus, 200);
    QCborMap lookup;
    lookup.insert(QLatin1StringView("accounts"), QCborArray{alice.account.bytes()});
    auto [lookupStatus, loadouts] = finish(network.post(http(QStringLiteral("cosmetics/loadouts"),
                                                             bob.tokens.accessToken),
                                                        lookup.toCborValue().toCbor()));
    QCOMPARE(lookupStatus, 200);
    const auto entry = loadouts.value(QLatin1StringView("loadouts")).toArray().first().toMap();
    QCOMPARE(entry.value(QLatin1StringView("account_id")).toByteArray(), alice.account.bytes());
    QCOMPARE(entry.value(QLatin1StringView("slots")).toMap()
                 .value(QString::fromLatin1(CosmeticRules::find(reward)->slot)).toString(), reward);

    // Closed, the account earns nothing however long it stays away.
    socket.close();
    QTRY_COMPARE(socket.state(), QAbstractSocket::UnconnectedState);
    QTest::qWait(200);
    const auto closed = cosmetics.state(alice.account).value();
    QTest::qWait(1000);
    const auto later = cosmetics.state(alice.account).value();
    QCOMPARE(later.drops, closed.drops);
    QCOMPARE(later.progressMs, closed.progressMs);
}

QTEST_GUILESS_MAIN(RelayServicesTest)
#include "tst_relayservices.moc"
