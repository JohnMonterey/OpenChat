#include "network/MlsSyncSession.h"
#include "network/RelayTransport.h"

#include "crypto/MlsClient.h"
#include "network/RelayClient.h"
#include "protocol/CiphertextEnvelope.h"
#include "security/SecureBuffer.h"
#include "storage/CapturingMlsStateStore.h"
#include "storage/SqlCipherDatabase.h"

#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <memory>
#include <utility>

using namespace OpenChat;

namespace {

ConversationId conversationId()
{
    return *ConversationId::fromBytes("conversation-one");
}

// Opens a fresh keyed SQLCipher database in `dir` (migrations applied by open())
// so a CapturingMlsStateStore can load()/capture against it.
std::unique_ptr<SqlCipherDatabase> openDatabase(const QTemporaryDir &dir, const QString &name)
{
    auto opened = SqlCipherDatabase::open(dir.filePath(name), SecureBuffer::random(32));
    if (!opened.hasValue())
        return nullptr;
    return std::make_unique<SqlCipherDatabase>(std::move(opened).value());
}

CiphertextEnvelopeV1 makeEnvelope(const QByteArray &ciphertext)
{
    const QByteArray hash = QCryptographicHash::hash(ciphertext, QCryptographicHash::Sha256);
    return CiphertextEnvelopeV1{
        1,
        EnvelopeId::generate(),
        AccountId::generate(),
        DeviceId::generate(),
        DeviceId::generate(),
        ConversationId::generate(),
        EnvelopeMessageKind::MlsPrivateMessage,
        1'700'000'000'000,
        1'700'000'060'000,
        EnvelopeId::generate(),
        ciphertext,
        hash,
        QByteArray(64, '\x01')};
}

RelayCredentials fixedCredentials()
{
    RelayCredentials credentials;
    credentials.accessToken = [] { return QByteArray("access"); };
    credentials.refreshToken = [] { return QByteArray("refresh"); };
    return credentials;
}

} // namespace

class SyncAdaptersTest final : public QObject
{
    Q_OBJECT

private slots:
    void mlsSyncSessionCapturesRatchetAndMapsOutcomes();
    void relayTransportDelegatesAndForwards();
};

void SyncAdaptersTest::mlsSyncSessionCapturesRatchetAndMapsOutcomes()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    auto aliceDb = openDatabase(dir, QStringLiteral("alice.sqlite3"));
    auto bobDb = openDatabase(dir, QStringLiteral("bob.sqlite3"));
    QVERIFY(aliceDb);
    QVERIFY(bobDb);

    CapturingMlsStateStore aliceCapture(*aliceDb, ProfileId::generate());
    CapturingMlsStateStore bobCapture(*bobDb, ProfileId::generate());

    const QByteArray aliceIdentity("alice-device");
    const QByteArray bobIdentity("bob-device");
    auto aliceResult = MlsClient::create(aliceIdentity, &aliceCapture);
    auto bobResult = MlsClient::create(bobIdentity, &bobCapture);
    QVERIFY(aliceResult);
    QVERIFY(bobResult);
    auto alice = std::move(aliceResult).value();
    auto bob = std::move(bobResult).value();

    // Form a two-member group (Alice adds Bob).
    auto bobKeyPackage = bob->generateKeyPackage();
    QVERIFY(bobKeyPackage);
    QVERIFY(alice->createGroup(conversationId()));
    auto add = alice->addMembers(conversationId(), {bobKeyPackage.value()});
    QVERIFY(add);
    QVERIFY(bob->joinGroup(conversationId(), add.value().welcome));

    MlsSyncSession aliceSession(*alice, aliceCapture);
    MlsSyncSession bobSession(*bob, bobCapture);

    // Group setup already advanced and captured both ratchets; drain that so the
    // next assertions isolate the encrypt/process ratchet advances.
    QVERIFY(!aliceCapture.takePendingState().isEmpty());
    QVERIFY(!bobCapture.takePendingState().isEmpty());
    QVERIFY(!aliceCapture.hasPendingState());
    QVERIFY(!bobCapture.hasPendingState());

    // encrypt() returns the ciphertext bytes and the ratchet advance is captured
    // (not written to disk), then surrendered exactly once.
    auto ciphertext = aliceSession.encrypt(conversationId(), QByteArrayView("hello bob"));
    QVERIFY(ciphertext);
    QVERIFY(!ciphertext.value().isEmpty());
    const QByteArray capturedOnSend = aliceSession.takePendingState();
    QVERIFY(!capturedOnSend.isEmpty());
    QVERIFY(aliceSession.takePendingState().isEmpty()); // surrendered once
    // The capture never touched the database.
    QVERIFY(aliceDb->loadMlsState(ProfileId::generate()).value().isEmpty());

    // process() maps an application message to Kind::Application, carrying the
    // plaintext and the MLS-authenticated sender identity; the advance is captured.
    auto applied = bobSession.process(conversationId(), ciphertext.value());
    QVERIFY(applied);
    QCOMPARE(applied.value().kind, SyncProcessOutcome::Kind::Application);
    QCOMPARE(applied.value().applicationData, QByteArray("hello bob"));
    QCOMPARE(applied.value().senderIdentity, aliceIdentity);
    QVERIFY(!bobSession.takePendingState().isEmpty());
    QVERIFY(bobSession.takePendingState().isEmpty());

    // A tampered message propagates the MlsError instead of a spurious outcome.
    QByteArray tampered = ciphertext.value();
    tampered[tampered.size() - 1] ^= char(0x80);
    auto rejected = bobSession.process(conversationId(), tampered);
    QVERIFY(!rejected);
    QCOMPARE(rejected.error(), MlsError::InvalidMessage);

    // A handshake commit maps to Kind::Control with no application plaintext.
    auto carolDb = openDatabase(dir, QStringLiteral("carol.sqlite3"));
    QVERIFY(carolDb);
    CapturingMlsStateStore carolCapture(*carolDb, ProfileId::generate());
    auto carolResult = MlsClient::create(QByteArray("carol-device"), &carolCapture);
    QVERIFY(carolResult);
    auto carol = std::move(carolResult).value();
    auto carolKeyPackage = carol->generateKeyPackage();
    QVERIFY(carolKeyPackage);
    auto addCarol = alice->addMembers(conversationId(), {carolKeyPackage.value()});
    QVERIFY(addCarol);
    (void)aliceCapture.takePendingState(); // drain Alice's commit-side advance

    auto control = bobSession.process(conversationId(), addCarol.value().commit);
    QVERIFY(control);
    QCOMPARE(control.value().kind, SyncProcessOutcome::Kind::Control);
    QVERIFY(control.value().applicationData.isEmpty());
    QVERIFY(!bobSession.takePendingState().isEmpty()); // commit still advanced the ratchet
}

void SyncAdaptersTest::relayTransportDelegatesAndForwards()
{
    RelayClient relay(DeviceId::generate(), AccountId::generate(), RelayEndpoints{},
                      fixedCredentials());
    RelayTransport transport(relay);

    // isConnected() reflects the underlying client (unconnected here).
    QVERIFY(!transport.isConnected());
    QCOMPARE(transport.isConnected(), relay.isConnected());

    // sendEnvelope()/acknowledge() swallow the not-connected error and never
    // throw or crash while offline.
    transport.sendEnvelope(makeEnvelope(QByteArray("ciphertext")));
    transport.acknowledge(EnvelopeId::generate(), 42);
    QVERIFY(!transport.isConnected());

    // Install the callbacks the engine would set in start().
    int connectedCalls = 0;
    transport.onConnected = [&connectedCalls] { ++connectedCalls; };

    QList<QByteArray> acceptedIds;
    QList<quint64> acceptedSequences;
    transport.onRelayAccepted = [&](const EnvelopeId &id, quint64 sequence) {
        acceptedIds.append(id.bytes());
        acceptedSequences.append(sequence);
    };

    int envelopeCalls = 0;
    QByteArray forwardedEnvelopeId;
    quint64 forwardedSequence = 0;
    transport.onEnvelope = [&](const CiphertextEnvelopeV1 &envelope, quint64 sequence) {
        ++envelopeCalls;
        forwardedEnvelopeId = envelope.envelopeId.bytes();
        forwardedSequence = sequence;
    };

    // connected() carries no payload, so the task's QMetaObject::invokeMethod
    // recipe drives it cleanly.
    QVERIFY(QMetaObject::invokeMethod(&relay, "connected", Qt::DirectConnection));
    QCOMPARE(connectedCalls, 1);

    // relayAccepted() and envelopeReceived() carry StrongId / envelope payloads
    // that are intentionally not default-constructible (a domain invariant, see
    // tst_relayclient), which makes QVariant/metatype marshalling impractical.
    // Qt Q_SIGNALS are public members, so the test emits them directly; the
    // RelayTransport connections are Qt::DirectConnection, so the forwarding runs
    // synchronously. The full live-socket delivery path is exercised end to end
    // by tst_relayclient and the later integration test.
    const EnvelopeId acceptedId = EnvelopeId::generate();
    emit relay.relayAccepted(acceptedId, 7);
    QCOMPARE(acceptedIds.size(), 1);
    QCOMPARE(acceptedIds.first(), acceptedId.bytes());
    QCOMPARE(acceptedSequences.first(), quint64(7));

    const CiphertextEnvelopeV1 envelope = makeEnvelope(QByteArray("inbound"));
    emit relay.envelopeReceived(envelope, 9);
    QCOMPARE(envelopeCalls, 1);
    QCOMPARE(forwardedEnvelopeId, envelope.envelopeId.bytes());
    QCOMPARE(forwardedSequence, quint64(9));
}

QTEST_MAIN(SyncAdaptersTest)
#include "tst_syncadapters.moc"
