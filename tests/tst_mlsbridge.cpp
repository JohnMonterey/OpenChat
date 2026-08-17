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
    void restoresOpaqueStateThroughCallbacks();
    void storageFailureRollsBackTheRatchet();
    void storedStateIsBoundToTheDeviceIdentity();
    void rejectsInvalidInputsWithoutCrossingTheAbi();
};

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
