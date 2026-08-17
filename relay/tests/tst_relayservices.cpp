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
    void duplicateSendIsIdempotent();
    void keyPackageClaimIsOneTime();
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
                                 QStringLiteral(":/relay/004_invites.sql")};
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
        "rate_limits, invites RESTART IDENTITY CASCADE")));
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

QTEST_GUILESS_MAIN(RelayServicesTest)
#include "tst_relayservices.moc"
