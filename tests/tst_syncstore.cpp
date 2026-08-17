#include "storage/CapturingMlsStateStore.h"
#include "storage/SqlCipherChatRepository.h"
#include "storage/SqlCipherDatabase.h"
#include "storage/SqlCipherSyncRepository.h"
#include "storage/SqlCipherSyncStore.h"

#include <QTemporaryDir>
#include <QtTest/QTest>

using namespace OpenChat;

namespace {

// Creates the local_profiles row the local_mls_state foreign key depends on, so
// the atomic MLS UPSERT inside every commit* has a parent to reference.
[[nodiscard]] bool seedProfile(SqlCipherDatabase &database, const ProfileId &profileId)
{
    auto privateKey = SecureBuffer::fromBytes(QByteArray(32, 's'));
    auto wrappingKey = SecureBuffer::fromBytes(QByteArray(32, 'w'));
    return database
        .storeDeviceIdentity(profileId, DeviceId::generate(), QByteArray(32, 'p'), privateKey,
                             wrappingKey, 1'000)
        .hasValue();
}

ConversationRecord conversation(const ConversationId &id)
{
    return ConversationRecord{id, QByteArray("mls-group"), QStringLiteral("Peer"),
                              ConversationKind::Direct, 1'000};
}

MessageRecord outgoingMessage(const MessageId &id, const ConversationId &conversationId,
                              const DeviceId &senderId, qint64 sentAtMs = 2'000)
{
    return MessageRecord{id,
                         conversationId,
                         senderId,
                         MessageFlow::Outgoing,
                         ContentKind::Text,
                         QStringLiteral("encrypted later"),
                         sentAtMs,
                         DeliveryState::Queued,
                         std::nullopt,
                         std::nullopt};
}

MessageRecord incomingMessage(const MessageId &id, const ConversationId &conversationId,
                              const DeviceId &senderId, quint64 sequence)
{
    return MessageRecord{id,
                         conversationId,
                         senderId,
                         MessageFlow::Incoming,
                         ContentKind::Text,
                         QStringLiteral("hello"),
                         2'000,
                         DeliveryState::Delivered,
                         sequence,
                         std::nullopt};
}

OutboxRecord outbox(const EnvelopeId &envelopeId, const MessageId &messageId,
                    const ConversationId &conversationId, qint64 dueAtMs = 3'000)
{
    return OutboxRecord{envelopeId, messageId, conversationId, QByteArray("ciphertext"), 0,
                        dueAtMs,     0,         OutboxState::Pending};
}

} // namespace

class SyncStoreTest final : public QObject
{
    Q_OBJECT

private slots:
    void atomicSendPersistsMessageOutboxAndMlsState();
    void rollbackLeavesMlsStateUnchangedWhenMessageWriteFails();
    void rollbackDropsMessageAndOutboxWhenMlsWriteFails();
    void receiveIsIdempotentAndDoesNotAdvanceStateTwice();
    void controlSendQueuesEnvelopeWithoutAMessageRow();
    void controlReceiveIsIdempotent();
    void claimDueLeasesAndReclaimsAfterExpiry();
    void acceptedEnvelopeIsNeverClaimedAgain();
    void scheduleRetryReArmsAndIsMonotonic();
    void deliveryStateIsMonotonic();
    void capturingStoreCapturesWithoutWritingAndSurrendersOnce();
    void commitMlsStatePersistsAloneAndUpdatesFromCapture();
};

void SyncStoreTest::atomicSendPersistsMessageOutboxAndMlsState()
{
    QTemporaryDir directory;
    auto key = SecureBuffer::random(32);
    auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), key);
    QVERIFY(opened.hasValue());
    auto database = std::move(opened).value();
    const auto profileId = ProfileId::generate();
    QVERIFY(seedProfile(database, profileId));

    SqlCipherChatRepository chats(database);
    SqlCipherSyncStore store(database, profileId);

    const auto conversationId = ConversationId::generate();
    const auto messageId = MessageId::generate();
    const auto envelopeId = EnvelopeId::generate();
    QVERIFY(chats.upsertConversation(conversation(conversationId)).hasValue());

    const QByteArray mlsState("state-1");
    QVERIFY(store
                .commitSend(outgoingMessage(messageId, conversationId, DeviceId::generate()),
                            outbox(envelopeId, messageId, conversationId), mlsState)
                .hasValue());

    // All three writes are present together.
    auto messages = chats.messages(conversationId, 50, std::nullopt);
    QVERIFY(messages.hasValue());
    QCOMPARE(messages.value().size(), 1);
    auto claimed = store.claimDue(3'000, 10, 8'000);
    QVERIFY(claimed.hasValue());
    QCOMPARE(claimed.value().size(), 1);
    auto persistedState = database.loadMlsState(profileId);
    QVERIFY(persistedState.hasValue());
    QCOMPARE(persistedState.value(), mlsState);
}

void SyncStoreTest::rollbackLeavesMlsStateUnchangedWhenMessageWriteFails()
{
    QTemporaryDir directory;
    auto key = SecureBuffer::random(32);
    auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), key);
    QVERIFY(opened.hasValue());
    auto database = std::move(opened).value();
    const auto profileId = ProfileId::generate();
    QVERIFY(seedProfile(database, profileId));

    SqlCipherChatRepository chats(database);
    SqlCipherSyncStore store(database, profileId);

    const auto conversationId = ConversationId::generate();
    QVERIFY(chats.upsertConversation(conversation(conversationId)).hasValue());

    // Baseline: a durable send commits the message, outbox row, and MLS state.
    const auto firstMessageId = MessageId::generate();
    const auto envelopeId = EnvelopeId::generate();
    const QByteArray committedState("state-0");
    QVERIFY(store
                .commitSend(outgoingMessage(firstMessageId, conversationId, DeviceId::generate()),
                            outbox(envelopeId, firstMessageId, conversationId), committedState)
                .hasValue());

    // A second send reusing the same envelope id violates the outbox primary key.
    // The message row is inserted first, then the outbox insert fails; the whole
    // transaction must roll back, and the MLS state must NOT advance.
    const auto secondMessageId = MessageId::generate();
    const QByteArray abandonedState("state-BAD");
    auto result =
        store.commitSend(outgoingMessage(secondMessageId, conversationId, DeviceId::generate()),
                         outbox(envelopeId, secondMessageId, conversationId), abandonedState);
    QVERIFY(!result.hasValue());
    QCOMPARE(result.error().code, RepositoryErrorCode::Conflict);

    // No partial write: only the first message survives, and MLS state is intact.
    auto messages = chats.messages(conversationId, 50, std::nullopt);
    QVERIFY(messages.hasValue());
    QCOMPARE(messages.value().size(), 1);
    QCOMPARE(messages.value().first().id, firstMessageId);
    auto persistedState = database.loadMlsState(profileId);
    QVERIFY(persistedState.hasValue());
    QCOMPARE(persistedState.value(), committedState); // never became "state-BAD"
}

void SyncStoreTest::rollbackDropsMessageAndOutboxWhenMlsWriteFails()
{
    QTemporaryDir directory;
    auto key = SecureBuffer::random(32);
    auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), key);
    QVERIFY(opened.hasValue());
    auto database = std::move(opened).value();

    SqlCipherChatRepository chats(database);
    // Bind the store to a profile that has NO local_profiles row, so the
    // local_mls_state UPSERT (the last statement of commitSend) fails its foreign
    // key. The message and outbox inserted earlier in the same transaction must
    // roll back with it.
    const auto orphanProfileId = ProfileId::generate();
    SqlCipherSyncStore store(database, orphanProfileId);

    const auto conversationId = ConversationId::generate();
    const auto messageId = MessageId::generate();
    QVERIFY(chats.upsertConversation(conversation(conversationId)).hasValue());

    auto result =
        store.commitSend(outgoingMessage(messageId, conversationId, DeviceId::generate()),
                         outbox(EnvelopeId::generate(), messageId, conversationId),
                         QByteArray("state-x"));
    QVERIFY(!result.hasValue());
    QCOMPARE(result.error().code, RepositoryErrorCode::Conflict);

    // Nothing persisted: no message, no outbox, no MLS state.
    QCOMPARE(chats.messages(conversationId, 50, std::nullopt).value().size(), 0);
    QCOMPARE(store.claimDue(10'000, 10, 20'000).value().size(), 0);
    auto persistedState = database.loadMlsState(orphanProfileId);
    QVERIFY(persistedState.hasValue());
    QVERIFY(persistedState.value().isEmpty());
}

void SyncStoreTest::receiveIsIdempotentAndDoesNotAdvanceStateTwice()
{
    QTemporaryDir directory;
    auto key = SecureBuffer::random(32);
    auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), key);
    QVERIFY(opened.hasValue());
    auto database = std::move(opened).value();
    const auto profileId = ProfileId::generate();
    QVERIFY(seedProfile(database, profileId));

    SqlCipherChatRepository chats(database);
    SqlCipherSyncRepository sync(database);
    SqlCipherSyncStore store(database, profileId);

    const auto conversationId = ConversationId::generate();
    const auto senderId = DeviceId::generate();
    const auto envelopeId = EnvelopeId::generate();
    const auto message = incomingMessage(MessageId::generate(), conversationId, senderId, 41);
    QVERIFY(chats.upsertConversation(conversation(conversationId)).hasValue());

    auto first = store.commitReceive(message, envelopeId, 41, QByteArray("recv-1"));
    QVERIFY(first.hasValue());
    QVERIFY(first.value()); // newly applied

    auto second = store.commitReceive(message, envelopeId, 41, QByteArray("recv-2"));
    QVERIFY(second.hasValue());
    QVERIFY(!second.value()); // already seen

    QCOMPARE(chats.messages(conversationId, 50, std::nullopt).value().size(), 1);
    auto seen = store.hasSeen(envelopeId);
    QVERIFY(seen.hasValue());
    QVERIFY(seen.value());
    auto cursor = sync.cursor(senderId);
    QVERIFY(cursor.hasValue());
    QCOMPARE(cursor.value().serverWatermark, quint64(41)); // not regressed
    auto persistedState = database.loadMlsState(profileId);
    QVERIFY(persistedState.hasValue());
    QCOMPARE(persistedState.value(), QByteArray("recv-1")); // second receive did not advance it
}

void SyncStoreTest::controlSendQueuesEnvelopeWithoutAMessageRow()
{
    QTemporaryDir directory;
    auto key = SecureBuffer::random(32);
    auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), key);
    QVERIFY(opened.hasValue());
    auto database = std::move(opened).value();
    const auto profileId = ProfileId::generate();
    QVERIFY(seedProfile(database, profileId));

    SqlCipherChatRepository chats(database);
    SqlCipherSyncStore store(database, profileId);

    const auto conversationId = ConversationId::generate();
    QVERIFY(chats.upsertConversation(conversation(conversationId)).hasValue());

    // A receipt has no visible message; its outbox row references a message id
    // that does not exist in the messages table.
    const auto envelopeId = EnvelopeId::generate();
    QVERIFY(store
                .commitControlSend(outbox(envelopeId, MessageId::generate(), conversationId),
                                   QByteArray("ctrl-1"))
                .hasValue());

    QCOMPARE(chats.messages(conversationId, 50, std::nullopt).value().size(), 0);
    auto claimed = store.claimDue(3'000, 10, 8'000);
    QVERIFY(claimed.hasValue());
    QCOMPARE(claimed.value().size(), 1); // queued and claimable
    auto persistedState = database.loadMlsState(profileId);
    QVERIFY(persistedState.hasValue());
    QCOMPARE(persistedState.value(), QByteArray("ctrl-1"));
}

void SyncStoreTest::controlReceiveIsIdempotent()
{
    QTemporaryDir directory;
    auto key = SecureBuffer::random(32);
    auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), key);
    QVERIFY(opened.hasValue());
    auto database = std::move(opened).value();
    const auto profileId = ProfileId::generate();
    QVERIFY(seedProfile(database, profileId));

    SqlCipherSyncRepository sync(database);
    SqlCipherSyncStore store(database, profileId);

    const auto envelopeId = EnvelopeId::generate();
    const auto senderId = DeviceId::generate();

    auto first = store.commitControlReceive(envelopeId, senderId, 5, QByteArray("cr-1"));
    QVERIFY(first.hasValue());
    QVERIFY(first.value());
    auto second = store.commitControlReceive(envelopeId, senderId, 5, QByteArray("cr-2"));
    QVERIFY(second.hasValue());
    QVERIFY(!second.value());

    QVERIFY(store.hasSeen(envelopeId).value());
    QCOMPARE(sync.cursor(senderId).value().serverWatermark, quint64(5));
    QCOMPARE(database.loadMlsState(profileId).value(), QByteArray("cr-1"));
}

void SyncStoreTest::claimDueLeasesAndReclaimsAfterExpiry()
{
    QTemporaryDir directory;
    auto key = SecureBuffer::random(32);
    auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), key);
    QVERIFY(opened.hasValue());
    auto database = std::move(opened).value();
    const auto profileId = ProfileId::generate();
    QVERIFY(seedProfile(database, profileId));

    SqlCipherChatRepository chats(database);
    SqlCipherSyncStore store(database, profileId);

    const auto conversationId = ConversationId::generate();
    const auto messageId = MessageId::generate();
    QVERIFY(chats.upsertConversation(conversation(conversationId)).hasValue());
    QVERIFY(store
                .commitSend(outgoingMessage(messageId, conversationId, DeviceId::generate()),
                            outbox(EnvelopeId::generate(), messageId, conversationId, 100),
                            QByteArray("s"))
                .hasValue());

    QCOMPARE(store.claimDue(100, 1, 200).value().size(), 1); // leased until 200
    QCOMPARE(store.claimDue(150, 1, 250).value().size(), 0); // still leased
    QCOMPARE(store.claimDue(200, 1, 300).value().size(), 1); // lease expired -> reclaimable
}

void SyncStoreTest::acceptedEnvelopeIsNeverClaimedAgain()
{
    QTemporaryDir directory;
    auto key = SecureBuffer::random(32);
    auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), key);
    QVERIFY(opened.hasValue());
    auto database = std::move(opened).value();
    const auto profileId = ProfileId::generate();
    QVERIFY(seedProfile(database, profileId));

    SqlCipherChatRepository chats(database);
    SqlCipherSyncStore store(database, profileId);

    const auto conversationId = ConversationId::generate();
    const auto messageId = MessageId::generate();
    const auto envelopeId = EnvelopeId::generate();
    QVERIFY(chats.upsertConversation(conversation(conversationId)).hasValue());
    QVERIFY(store
                .commitSend(outgoingMessage(messageId, conversationId, DeviceId::generate()),
                            outbox(envelopeId, messageId, conversationId, 100), QByteArray("s"))
                .hasValue());

    QCOMPARE(store.claimDue(100, 1, 200).value().size(), 1);
    QVERIFY(store.markAccepted(envelopeId).hasValue());
    QCOMPARE(store.claimDue(1'000, 1, 2'000).value().size(), 0);
    QVERIFY(!store.scheduleRetry(envelopeId, 1, 3'000).hasValue());
}

void SyncStoreTest::scheduleRetryReArmsAndIsMonotonic()
{
    QTemporaryDir directory;
    auto key = SecureBuffer::random(32);
    auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), key);
    QVERIFY(opened.hasValue());
    auto database = std::move(opened).value();
    const auto profileId = ProfileId::generate();
    QVERIFY(seedProfile(database, profileId));

    SqlCipherChatRepository chats(database);
    SqlCipherSyncStore store(database, profileId);

    const auto conversationId = ConversationId::generate();
    const auto messageId = MessageId::generate();
    const auto envelopeId = EnvelopeId::generate();
    QVERIFY(chats.upsertConversation(conversation(conversationId)).hasValue());
    QVERIFY(store
                .commitSend(outgoingMessage(messageId, conversationId, DeviceId::generate()),
                            outbox(envelopeId, messageId, conversationId, 100), QByteArray("s"))
                .hasValue());

    QCOMPARE(store.claimDue(100, 1, 200).value().size(), 1);
    QVERIFY(store.scheduleRetry(envelopeId, 1, 1'100).hasValue());
    QVERIFY(!store.scheduleRetry(envelopeId, 1, 1'200).hasValue()); // same attempt is not re-armed
    QVERIFY(!store.scheduleRetry(envelopeId, 0, 1'200).hasValue()); // backward attempt rejected
}

void SyncStoreTest::deliveryStateIsMonotonic()
{
    QTemporaryDir directory;
    auto key = SecureBuffer::random(32);
    auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), key);
    QVERIFY(opened.hasValue());
    auto database = std::move(opened).value();
    const auto profileId = ProfileId::generate();
    QVERIFY(seedProfile(database, profileId));

    SqlCipherChatRepository chats(database);
    SqlCipherSyncStore store(database, profileId);

    const auto conversationId = ConversationId::generate();
    const auto messageId = MessageId::generate();
    QVERIFY(chats.upsertConversation(conversation(conversationId)).hasValue());
    QVERIFY(store
                .commitSend(outgoingMessage(messageId, conversationId, DeviceId::generate()),
                            outbox(EnvelopeId::generate(), messageId, conversationId),
                            QByteArray("s"))
                .hasValue());

    QVERIFY(store.advanceDeliveryState(messageId, DeliveryState::Sending).hasValue());
    auto backwards = store.advanceDeliveryState(messageId, DeliveryState::Queued);
    QVERIFY(!backwards.hasValue());
    QCOMPARE(backwards.error().code, RepositoryErrorCode::Conflict);
}

void SyncStoreTest::capturingStoreCapturesWithoutWritingAndSurrendersOnce()
{
    QTemporaryDir directory;
    auto key = SecureBuffer::random(32);
    auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), key);
    QVERIFY(opened.hasValue());
    auto database = std::move(opened).value();
    const auto profileId = ProfileId::generate();
    QVERIFY(seedProfile(database, profileId));

    // Establish a durably-committed state to load from.
    QVERIFY(database.storeMlsState(profileId, QByteArray("committed-1")).hasValue());

    CapturingMlsStateStore capture(database, profileId);
    QVERIFY(!capture.hasPendingState());
    auto loaded = capture.load();
    QVERIFY(loaded.hasValue());
    QCOMPARE(loaded.value(), QByteArray("committed-1"));

    // store() captures in memory and does NOT touch the database.
    QVERIFY(capture.store(QByteArray("pending-A")).hasValue());
    QVERIFY(capture.hasPendingState());
    QCOMPARE(database.loadMlsState(profileId).value(), QByteArray("committed-1"));
    QCOMPARE(capture.load().value(), QByteArray("committed-1")); // load still last committed

    // takePendingState() surrenders the blob once, then clears.
    QCOMPARE(capture.takePendingState(), QByteArray("pending-A"));
    QVERIFY(!capture.hasPendingState());
    QCOMPARE(capture.takePendingState(), QByteArray()); // nothing pending now
}

void SyncStoreTest::commitMlsStatePersistsAloneAndUpdatesFromCapture()
{
    QTemporaryDir directory;
    auto key = SecureBuffer::random(32);
    auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), key);
    QVERIFY(opened.hasValue());
    auto database = std::move(opened).value();
    const auto profileId = ProfileId::generate();
    QVERIFY(seedProfile(database, profileId));

    SqlCipherSyncStore store(database, profileId);

    // An empty blob is rejected, and nothing is persisted.
    QVERIFY(!store.commitMlsState(QByteArray()).hasValue());
    QVERIFY(database.loadMlsState(profileId).value().isEmpty());

    // A direct commit persists the blob alone; loadMlsState returns exactly it.
    const QByteArray first("kp-state-1");
    QVERIFY(store.commitMlsState(first).hasValue());
    auto afterFirst = database.loadMlsState(profileId);
    QVERIFY(afterFirst.hasValue());
    QCOMPARE(afterFirst.value(), first);

    // A subsequent capturing mutation's surrendered blob updates it in place:
    // store() captures without writing, takePendingState() surrenders it, and
    // commitMlsState() makes it durable.
    CapturingMlsStateStore capture(database, profileId);
    QVERIFY(capture.store(QByteArray("kp-state-2")).hasValue());
    QVERIFY(capture.hasPendingState());
    QCOMPARE(database.loadMlsState(profileId).value(), first); // not yet written
    QVERIFY(store.commitMlsState(capture.takePendingState()).hasValue());
    QVERIFY(!capture.hasPendingState());
    QCOMPARE(database.loadMlsState(profileId).value(), QByteArray("kp-state-2"));
}

QTEST_GUILESS_MAIN(SyncStoreTest)
#include "tst_syncstore.moc"
