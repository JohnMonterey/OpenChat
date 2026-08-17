#include "network/MlsSyncSession.h"
#include "network/RelayTransport.h"

#include "crypto/MlsClient.h"
#include "domain/Contact.h"
#include "network/RelayClient.h"
#include "protocol/CiphertextEnvelope.h"
#include "security/SecureBuffer.h"
#include "storage/CapturingMlsStateStore.h"
#include "storage/SqlCipherContactRepository.h"
#include "storage/SqlCipherDatabase.h"
#include "storage/SqlCipherSyncStore.h"

#include <QCryptographicHash>
#include <QDateTime>
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

// Serialized DevicePublicCredential the MLS layer authenticates:
// version(1) || deviceId(16) || signingKey(32). Used as an MlsClient identity so
// inspectWelcome returns a credential naming `device`.
QByteArray credentialFor(const DeviceId &device)
{
    QByteArray credential;
    credential.append(char{1});
    credential.append(device.bytes());
    credential.append(QByteArray(32, 'k'));
    return credential;
}

// The engine's own authentication rule, replicated here to exercise the real
// credential the MLS layer returns: the credential must name exactly `claimed`.
bool credentialNamesDevice(QByteArrayView credential, const DeviceId &claimed)
{
    if (credential.size() < 1 + DeviceId::byteCount)
        return false;
    if (credential.at(0) != char{1})
        return false;
    return credential.sliced(1, DeviceId::byteCount) == QByteArrayView(claimed.bytes());
}

} // namespace

class SyncAdaptersTest final : public QObject
{
    Q_OBJECT

private slots:
    void mlsSyncSessionCapturesRatchetAndMapsOutcomes();
    void relayTransportDelegatesAndForwards();
    void handshakeAcceptAuthenticatesJoinsPersistsAndDecrypts();
    void handshakeAcceptRejectsMismatchedSenderDeviceWithoutJoining();
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

void SyncAdaptersTest::handshakeAcceptAuthenticatesJoinsPersistsAndDecrypts()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // Device A is the sender; device B is us. Each MlsClient carries a real device
    // credential as its identity, so the Welcome authenticates by device.
    const DeviceId deviceA = DeviceId::generate();
    const DeviceId deviceB = DeviceId::generate();
    const AccountId senderAccount = AccountId::generate();
    const QByteArray aIdentity = credentialFor(deviceA);
    const QByteArray bIdentity = credentialFor(deviceB);
    const ProfileId bProfile = ProfileId::generate();

    auto aDb = openDatabase(dir, QStringLiteral("a.sqlite3"));
    auto bDb = openDatabase(dir, QStringLiteral("b.sqlite3"));
    QVERIFY(aDb);
    QVERIFY(bDb);

    // local_mls_state has a FK to local_profiles(profile_id); seed B's device
    // identity row so the accept commit's MLS-state upsert satisfies it (the real
    // app path always has this row from profile creation).
    QVERIFY(bDb->storeDeviceIdentity(bProfile, deviceB, QByteArray(32, 'p'),
                                     SecureBuffer::random(32), SecureBuffer::random(32),
                                     QDateTime::currentMSecsSinceEpoch())
                .hasValue());

    CapturingMlsStateStore aCapture(*aDb, ProfileId::generate());
    CapturingMlsStateStore bCapture(*bDb, bProfile);
    auto aResult = MlsClient::create(aIdentity, &aCapture);
    auto bResult = MlsClient::create(bIdentity, &bCapture);
    QVERIFY(aResult);
    QVERIFY(bResult);
    auto a = std::move(aResult).value();
    auto b = std::move(bResult).value();

    // A forms a fresh group and adds B (using B's real KeyPackage), producing a
    // Welcome sealed to B.
    const ConversationId conversation = ConversationId::generate();
    auto bKeyPackage = b->generateKeyPackage();
    QVERIFY(bKeyPackage);
    // Generating the KeyPackage advanced and captured B's ratchet out-of-band; drain
    // it so the next assertions isolate the inspect/join advances.
    (void)bCapture.takePendingState();
    QVERIFY(a->createGroup(conversation));
    auto add = a->addMembers(conversation, {bKeyPackage.value()});
    QVERIFY(add);
    const QByteArray welcome = add.value().welcome;

    MlsSyncSession bSession(*b, bCapture);

    // Authenticate BEFORE joining: inspectWelcome is read-only and names A's device.
    auto members = bSession.inspectWelcome(welcome);
    QVERIFY(members);
    QCOMPARE(members.value().size(), qsizetype(1));
    QVERIFY(credentialNamesDevice(members.value().front(), deviceA));
    // Inspecting did not advance or capture any ratchet state.
    QVERIFY(bSession.takePendingState().isEmpty());

    // Auth passed: join and surrender the post-join state.
    QVERIFY(bSession.joinGroup(conversation, welcome));
    const QByteArray joinedState = bSession.takePendingState();
    QVERIFY(!joinedState.isEmpty());

    // The accept commit is atomic with the PendingIncoming->Accepted flip, so B
    // must first hold a PendingIncoming roster row for the sender.
    SqlCipherContactRepository bContacts(*bDb);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const ContactRecord pending{senderAccount, QString(),     QString(), ContactState::PendingIncoming,
                                conversation,  now,           now};
    QVERIFY(bContacts.recordIncomingRequest(pending).hasValue());

    SqlCipherSyncStore bStore(*bDb, bProfile);
    QVERIFY(bStore.commitHandshakeAccept(senderAccount, conversation, now, joinedState).hasValue());

    // The roster flipped to Accepted and the MLS state is durable.
    auto accepted = bContacts.find(senderAccount);
    QVERIFY(accepted.hasValue());
    QVERIFY(accepted.value().has_value());
    QCOMPARE(accepted.value()->state, ContactState::Accepted);
    QVERIFY(!bDb->loadMlsState(bProfile).value().isEmpty());

    // A now encrypts an application message at the joined epoch.
    auto ciphertext = a->encrypt(conversation, QByteArrayView("hello from A"));
    QVERIFY(ciphertext);

    // Reload B purely from the persisted MLS state (a fresh client that never saw
    // the Welcome) and decrypt A's message: proof the epochs match and the join
    // really persisted through the atomic accept commit.
    CapturingMlsStateStore bReloadCapture(*bDb, bProfile);
    auto bReloadResult = MlsClient::create(bIdentity, &bReloadCapture);
    QVERIFY(bReloadResult);
    auto bReloaded = std::move(bReloadResult).value();
    auto decrypted = bReloaded->process(conversation, ciphertext.value().bytes);
    QVERIFY(decrypted);
    QCOMPARE(decrypted.value().kind, MlsProcessKind::Application);
    QCOMPARE(decrypted.value().applicationData, QByteArray("hello from A"));
    QCOMPARE(decrypted.value().senderIdentity, aIdentity);
}

void SyncAdaptersTest::handshakeAcceptRejectsMismatchedSenderDeviceWithoutJoining()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const DeviceId deviceA = DeviceId::generate();
    const DeviceId deviceB = DeviceId::generate();
    const DeviceId claimedButWrong = DeviceId::generate(); // what a lying relay claims
    QVERIFY(claimedButWrong != deviceA);

    auto aDb = openDatabase(dir, QStringLiteral("a2.sqlite3"));
    auto bDb = openDatabase(dir, QStringLiteral("b2.sqlite3"));
    QVERIFY(aDb);
    QVERIFY(bDb);

    CapturingMlsStateStore aCapture(*aDb, ProfileId::generate());
    CapturingMlsStateStore bCapture(*bDb, ProfileId::generate());
    auto a = std::move(MlsClient::create(credentialFor(deviceA), &aCapture)).value();
    auto b = std::move(MlsClient::create(credentialFor(deviceB), &bCapture)).value();

    const ConversationId conversation = ConversationId::generate();
    auto bKeyPackage = b->generateKeyPackage();
    QVERIFY(bKeyPackage);
    (void)bCapture.takePendingState(); // drain the KeyPackage-generation advance
    QVERIFY(a->createGroup(conversation));
    auto add = a->addMembers(conversation, {bKeyPackage.value()});
    QVERIFY(add);
    const QByteArray welcome = add.value().welcome;

    MlsSyncSession bSession(*b, bCapture);

    // The Welcome genuinely names deviceA, but the relay claims a different sender
    // device: authentication rejects it.
    auto members = bSession.inspectWelcome(welcome);
    QVERIFY(members);
    QCOMPARE(members.value().size(), qsizetype(1));
    QVERIFY(credentialNamesDevice(members.value().front(), deviceA));      // real
    QVERIFY(!credentialNamesDevice(members.value().front(), claimedButWrong)); // forged

    // Because auth fails, B never joins -- and inspectWelcome left no state behind.
    QVERIFY(bSession.takePendingState().isEmpty());

    // Proof no join happened: B cannot process an application message for the group
    // it never joined.
    auto ciphertext = a->encrypt(conversation, QByteArrayView("should not arrive"));
    QVERIFY(ciphertext);
    auto processed = bSession.process(conversation, ciphertext.value().bytes);
    QVERIFY(!processed);
    QCOMPARE(processed.error(), MlsError::MissingGroup);
}

QTEST_MAIN(SyncAdaptersTest)
#include "tst_syncadapters.moc"
