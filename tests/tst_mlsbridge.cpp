#include "crypto/MlsClient.h"

#include <QtTest/QTest>

using namespace OpenChat;

namespace {

ConversationId conversationId()
{
    return *ConversationId::fromBytes("conversation-one");
}

class MemoryStateStore final : public MlsStateStore
{
public:
    Result<QByteArray, MlsError> load() override
    {
        return Result<QByteArray, MlsError>::success(state);
    }

    Result<void, MlsError> store(QByteArrayView value) override
    {
        if (failWrites)
            return Result<void, MlsError>::failure(MlsError::Storage);
        state = value.toByteArray();
        return Result<void, MlsError>::success();
    }

    QByteArray state;
    bool failWrites = false;
};

} // namespace

class MlsBridgeTest final : public QObject
{
    Q_OBJECT

private slots:
    void exchangesAndRejectsTampering();
    void inspectsWelcomeWithoutJoining();
    void inspectRejectsMalformedWelcome();
    void inspectKeyPackageReturnsCredentialReadOnly();
    void restoresOpaqueStateThroughCallbacks();
    void storageFailureRollsBackTheRatchet();
    void storedStateIsBoundToTheDeviceIdentity();
    void rejectsInvalidInputsWithoutCrossingTheAbi();
    void threePartyGroupListsMembersAndRemovesOne();
    void messagesFromARecentPastEpochStillDecrypt();
    void deletingAGroupForgetsItAndFreesItsId();
};

void MlsBridgeTest::deletingAGroupForgetsItAndFreesItsId()
{
    auto client = std::move(MlsClient::create("device")).value();
    // Nothing to delete yet: reported, not swallowed.
    auto missing = client->deleteGroup(conversationId());
    QVERIFY(!missing.hasValue());
    QCOMPARE(missing.error(), MlsError::MissingGroup);

    QVERIFY(client->createGroup(conversationId()));
    QVERIFY(client->groupMembers(conversationId()));
    // A second group under the same id is refused while the first exists...
    QVERIFY(!client->createGroup(conversationId()));

    QVERIFY(client->deleteGroup(conversationId()));
    // ...and after deletion there is no group to list, encrypt in, or delete.
    QVERIFY(!client->groupMembers(conversationId()));
    QVERIFY(!client->encrypt(conversationId(), "hello"));
    QVERIFY(!client->deleteGroup(conversationId()));
    // The id is free for a fresh group.
    QVERIFY(client->createGroup(conversationId()));
    QVERIFY(client->groupMembers(conversationId()));
}

void MlsBridgeTest::threePartyGroupListsMembersAndRemovesOne()
{
    auto alice = std::move(MlsClient::create("alice-device")).value();
    auto bob = std::move(MlsClient::create("bob-device")).value();
    auto carol = std::move(MlsClient::create("carol-device")).value();

    // Alice creates the group and adds both others in one commit; each joins
    // from the same Welcome.
    QVERIFY(alice->createGroup(conversationId()));
    auto add = alice->addMembers(conversationId(),
                                 {bob->generateKeyPackage().value(), carol->generateKeyPackage().value()});
    QVERIFY(add);
    QVERIFY(bob->joinGroup(conversationId(), add.value().welcome));
    QVERIFY(carol->joinGroup(conversationId(), add.value().welcome));

    // Every member sees the other two by their authenticated credentials.
    auto aliceView = alice->groupMembers(conversationId());
    QVERIFY(aliceView);
    QCOMPARE(aliceView.value().size(), qsizetype(2));
    QVERIFY(aliceView.value().contains(QByteArray("bob-device")));
    QVERIFY(aliceView.value().contains(QByteArray("carol-device")));
    auto bobView = bob->groupMembers(conversationId());
    QVERIFY(bobView);
    QVERIFY(bobView.value().contains(QByteArray("alice-device")));
    QVERIFY(bobView.value().contains(QByteArray("carol-device")));
    // A group of one lists nobody.
    auto solo = std::move(MlsClient::create("solo-device")).value();
    const ConversationId soloGroup = *ConversationId::fromBytes("conversation-two");
    QVERIFY(solo->createGroup(soloGroup));
    QVERIFY(solo->groupMembers(soloGroup));
    QVERIFY(solo->groupMembers(soloGroup).value().isEmpty());
    QVERIFY(!solo->groupMembers(conversationId()));

    // One ciphertext, readable by both other members.
    auto hello = bob->encrypt(conversationId(), "hi all");
    QVERIFY(hello);
    QCOMPARE(alice->process(conversationId(), hello.value().bytes).value().applicationData,
             QByteArray("hi all"));
    QCOMPARE(carol->process(conversationId(), hello.value().bytes).value().applicationData,
             QByteArray("hi all"));

    // Carol leaves: Bob commits her removal and Alice applies it. From then on
    // Carol cannot read the group, and the two remaining members still can.
    auto removal = bob->removeMembers(conversationId(), {QByteArray("carol-device")});
    QVERIFY(removal);
    auto applied = alice->process(conversationId(), removal.value());
    QVERIFY(applied);
    QCOMPARE(applied.value().kind, MlsProcessKind::Commit);
    QCOMPARE(alice->groupMembers(conversationId()).value(), QList<QByteArray>{"bob-device"});
    QCOMPARE(bob->groupMembers(conversationId()).value(), QList<QByteArray>{"alice-device"});
    auto afterwards = alice->encrypt(conversationId(), "just us");
    QVERIFY(afterwards);
    QCOMPARE(bob->process(conversationId(), afterwards.value().bytes).value().applicationData,
             QByteArray("just us"));
    QVERIFY(!carol->process(conversationId(), afterwards.value().bytes));
}

void MlsBridgeTest::messagesFromARecentPastEpochStillDecrypt()
{
    auto alice = std::move(MlsClient::create("alice-device")).value();
    auto bob = std::move(MlsClient::create("bob-device")).value();
    auto carol = std::move(MlsClient::create("carol-device")).value();

    QVERIFY(alice->createGroup(conversationId()));
    auto add = alice->addMembers(conversationId(), {bob->generateKeyPackage().value()});
    QVERIFY(add);
    QVERIFY(bob->joinGroup(conversationId(), add.value().welcome));

    // Bob's message was sealed at the current epoch. Before it reaches Alice
    // she adds Carol, moving the group on by one epoch; the message from the
    // epoch before must still open rather than be lost to the race.
    auto inFlight = bob->encrypt(conversationId(), "sent just before the add");
    QVERIFY(inFlight);
    auto addCarol = alice->addMembers(conversationId(), {carol->generateKeyPackage().value()});
    QVERIFY(addCarol);
    auto late = alice->process(conversationId(), inFlight.value().bytes);
    QVERIFY2(late, "an application message from the previous epoch was refused");
    QCOMPARE(late.value().applicationData, QByteArray("sent just before the add"));
}

void MlsBridgeTest::exchangesAndRejectsTampering()
{
    auto aliceResult = MlsClient::create("alice-device");
    auto bobResult = MlsClient::create("bob-device");
    QVERIFY(aliceResult);
    QVERIFY(bobResult);
    auto alice = std::move(aliceResult).value();
    auto bob = std::move(bobResult).value();

    auto keyPackage = bob->generateKeyPackage();
    QVERIFY(keyPackage);
    QVERIFY(alice->createGroup(conversationId()));
    auto add = alice->addMembers(conversationId(), {keyPackage.value()});
    QVERIFY(add);
    QVERIFY(bob->joinGroup(conversationId(), add.value().welcome));

    auto ciphertext = alice->encrypt(conversationId(), "hello bob");
    QVERIFY(ciphertext);
    QByteArray tampered = ciphertext.value().bytes;
    tampered[tampered.size() - 1] ^= char(0x80);
    auto rejected = bob->process(conversationId(), tampered);
    QVERIFY(!rejected);
    QCOMPARE(rejected.error(), MlsError::InvalidMessage);

    auto plaintext = bob->process(conversationId(), ciphertext.value().bytes);
    QVERIFY(plaintext);
    QCOMPARE(plaintext.value().kind, MlsProcessKind::Application);
    QCOMPARE(plaintext.value().applicationData, QByteArray("hello bob"));
    // The bridge surfaces the sender's MLS-authenticated credential.
    QCOMPARE(plaintext.value().senderIdentity, QByteArray("alice-device"));
}

void MlsBridgeTest::inspectsWelcomeWithoutJoining()
{
    MemoryStateStore bobState;
    auto aliceResult = MlsClient::create("alice-device");
    auto bobResult = MlsClient::create("bob-device", &bobState);
    QVERIFY(aliceResult);
    QVERIFY(bobResult);
    auto alice = std::move(aliceResult).value();
    auto bob = std::move(bobResult).value();

    auto keyPackage = bob->generateKeyPackage();
    QVERIFY(keyPackage);
    QVERIFY(alice->createGroup(conversationId()));
    auto add = alice->addMembers(conversationId(), {keyPackage.value()});
    QVERIFY(add);

    // Read-only inspection reveals the adder's authenticated credential and
    // leaves the persisted snapshot untouched (store() is never called).
    const QByteArray persistedBefore = bobState.state;
    QVERIFY(!persistedBefore.isEmpty());
    auto inspected = bob->inspectWelcome(add.value().welcome);
    QVERIFY(inspected);
    QCOMPARE(inspected.value().size(), qsizetype(1));
    QCOMPARE(inspected.value().at(0), QByteArray("alice-device"));
    QCOMPARE(bobState.state, persistedBefore);

    // Inspection left no residue: Bob can still join the same Welcome and
    // decrypt an application message from Alice.
    QVERIFY(bob->joinGroup(conversationId(), add.value().welcome));
    auto ciphertext = alice->encrypt(conversationId(), "hello bob");
    QVERIFY(ciphertext);
    auto plaintext = bob->process(conversationId(), ciphertext.value().bytes);
    QVERIFY(plaintext);
    QCOMPARE(plaintext.value().kind, MlsProcessKind::Application);
    QCOMPARE(plaintext.value().applicationData, QByteArray("hello bob"));
    QCOMPARE(plaintext.value().senderIdentity, QByteArray("alice-device"));
}

void MlsBridgeTest::inspectRejectsMalformedWelcome()
{
    auto bobResult = MlsClient::create("bob-device");
    QVERIFY(bobResult);
    auto bob = std::move(bobResult).value();

    auto rejected = bob->inspectWelcome(QByteArray("not a welcome"));
    QVERIFY(!rejected);
    QCOMPARE(rejected.error(), MlsError::InvalidMessage);
    // Empty input fails closed before crossing the ABI.
    QVERIFY(!bob->inspectWelcome({}));
}

void MlsBridgeTest::inspectKeyPackageReturnsCredentialReadOnly()
{
    MemoryStateStore bobState;
    auto bobResult = MlsClient::create("bob-device", &bobState);
    auto aliceResult = MlsClient::create("alice-device");
    QVERIFY(bobResult);
    QVERIFY(aliceResult);
    auto bob = std::move(bobResult).value();
    auto alice = std::move(aliceResult).value();

    auto keyPackage = bob->generateKeyPackage();
    QVERIFY(keyPackage);

    // Reading a KeyPackage surfaces its authenticated leaf credential without
    // writing to the persisted snapshot (store() is never called during it).
    const QByteArray persistedBefore = bobState.state;
    QVERIFY(!persistedBefore.isEmpty());
    auto ownView = bob->inspectKeyPackage(keyPackage.value());
    QVERIFY(ownView);
    QCOMPARE(ownView.value(), QByteArray("bob-device"));
    QCOMPARE(bobState.state, persistedBefore);

    // A different client extracts the SAME credential from the same bytes,
    // proving the identity comes from the KeyPackage, not from local state.
    auto peerView = alice->inspectKeyPackage(keyPackage.value());
    QVERIFY(peerView);
    QCOMPARE(peerView.value(), QByteArray("bob-device"));
    QCOMPARE(peerView.value(), ownView.value());

    // Malformed and empty inputs fail closed and leave the snapshot untouched.
    auto malformed = bob->inspectKeyPackage(QByteArray("not a key package"));
    QVERIFY(!malformed);
    QCOMPARE(malformed.error(), MlsError::InvalidMessage);
    QVERIFY(!bob->inspectKeyPackage({}));
    QCOMPARE(bobState.state, persistedBefore);
}

void MlsBridgeTest::restoresOpaqueStateThroughCallbacks()
{
    MemoryStateStore aliceState;
    MemoryStateStore bobState;
    auto aliceResult = MlsClient::create("alice-device", &aliceState);
    auto bobResult = MlsClient::create("bob-device", &bobState);
    QVERIFY(aliceResult);
    QVERIFY(bobResult);
    auto alice = std::move(aliceResult).value();
    auto bob = std::move(bobResult).value();

    auto keyPackage = bob->generateKeyPackage();
    QVERIFY(keyPackage);
    QVERIFY(alice->createGroup(conversationId()));
    auto add = alice->addMembers(conversationId(), {keyPackage.value()});
    QVERIFY(add);
    QVERIFY(bob->joinGroup(conversationId(), add.value().welcome));
    QVERIFY(!bobState.state.isEmpty());

    bob.reset();
    auto restoredResult = MlsClient::create("bob-device", &bobState);
    QVERIFY(restoredResult);
    auto restored = std::move(restoredResult).value();
    auto ciphertext = alice->encrypt(conversationId(), "after restart");
    QVERIFY(ciphertext);
    auto plaintext = restored->process(conversationId(), ciphertext.value().bytes);
    QVERIFY(plaintext);
    QCOMPARE(plaintext.value().applicationData, QByteArray("after restart"));
}

void MlsBridgeTest::rejectsInvalidInputsWithoutCrossingTheAbi()
{
    auto clientResult = MlsClient::create("device");
    QVERIFY(clientResult);
    auto client = std::move(clientResult).value();
    QVERIFY(!client->encrypt(conversationId(), {}));
    QVERIFY(!client->joinGroup(conversationId(), {}));
    QVERIFY(!client->addMembers(conversationId(), {}));
}

void MlsBridgeTest::storageFailureRollsBackTheRatchet()
{
    MemoryStateStore aliceState;
    MemoryStateStore bobState;
    auto aliceResult = MlsClient::create("alice-device", &aliceState);
    auto bobResult = MlsClient::create("bob-device", &bobState);
    QVERIFY(aliceResult);
    QVERIFY(bobResult);
    auto alice = std::move(aliceResult).value();
    auto bob = std::move(bobResult).value();

    auto keyPackage = bob->generateKeyPackage();
    QVERIFY(keyPackage);
    QVERIFY(alice->createGroup(conversationId()));
    auto add = alice->addMembers(conversationId(), {keyPackage.value()});
    QVERIFY(add);
    QVERIFY(bob->joinGroup(conversationId(), add.value().welcome));

    aliceState.failWrites = true;
    auto failed = alice->encrypt(conversationId(), "not committed");
    QVERIFY(!failed);
    QCOMPARE(failed.error(), MlsError::Storage);

    aliceState.failWrites = false;
    auto retried = alice->encrypt(conversationId(), "committed");
    QVERIFY(retried);
    auto plaintext = bob->process(conversationId(), retried.value().bytes);
    QVERIFY(plaintext);
    QCOMPARE(plaintext.value().applicationData, QByteArray("committed"));
}

void MlsBridgeTest::storedStateIsBoundToTheDeviceIdentity()
{
    MemoryStateStore state;
    auto original = MlsClient::create("alice-device", &state);
    QVERIFY(original);
    original.value().reset();

    auto wrongIdentity = MlsClient::create("mallory-device", &state);
    QVERIFY(!wrongIdentity);
    QCOMPARE(wrongIdentity.error(), MlsError::InvalidInput);
}

QTEST_GUILESS_MAIN(MlsBridgeTest)
#include "tst_mlsbridge.moc"
