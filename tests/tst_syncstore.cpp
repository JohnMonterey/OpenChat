#include "storage/CapturingMlsStateStore.h"
#include "storage/SqlCipherChatRepository.h"
#include "storage/SqlCipherContactRepository.h"
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

ContactRecord incomingContact(const AccountId &accountId, qint64 nowMs = 1'000)
{
    return ContactRecord{accountId,      QStringLiteral("peer"), QStringLiteral("Peer"),
                         ContactState::PendingIncoming, std::nullopt, nowMs,
                         nowMs};
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
    void commitHandshakeReceiveStashesAndAdvancesWatermark();
    void commitHandshakeReceiveIsIdempotentOnRedelivery();
    void commitHandshakeReceiveDropsBlockedSenderButConsumesEnvelope();
    void commitHandshakeAcceptPromotesContactAndDeletesStash();
    void commitHandshakeAcceptWithEmptyKeyBindsNull();
    void commitHandshakeAcceptRollsBackWhenNotPendingIncoming();
    void deletePendingHandshakeRemovesRowAndListIsOrdered();
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

void SyncStoreTest::commitHandshakeReceiveStashesAndAdvancesWatermark()
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
    const auto senderAccountId = AccountId::generate();
    const auto senderDeviceId = DeviceId::generate();
    const auto conversationId = ConversationId::generate();
    const QByteArray welcome("mls-welcome-bytes");

    auto stashed = store.commitHandshakeReceive(envelopeId, senderAccountId, senderDeviceId,
                                                conversationId, welcome, 2'500, 17);
    QVERIFY(stashed.hasValue());
    QCOMPARE(stashed.value(), HandshakeReceiveOutcome::Stashed);

    // Queryable by conversation id, with every field round-tripped.
    auto loaded = store.loadPendingHandshake(conversationId);
    QVERIFY(loaded.hasValue());
    QVERIFY(loaded.value().has_value());
    const PendingHandshakeRecord &record = *loaded.value();
    QCOMPARE(record.conversationId, conversationId);
    QCOMPARE(record.senderAccountId, senderAccountId);
    QCOMPARE(record.senderDeviceId, senderDeviceId);
    QCOMPARE(record.envelopeId, envelopeId);
    QCOMPARE(record.welcome, welcome);
    QCOMPARE(record.receivedAtMs, qint64(2'500));

    auto all = store.pendingHandshakes();
    QVERIFY(all.hasValue());
    QCOMPARE(all.value().size(), 1);

    // The envelope was consumed: the sender device cursor advanced to the watermark.
    QVERIFY(store.hasSeen(envelopeId).value());
    QCOMPARE(sync.cursor(senderDeviceId).value().serverWatermark, quint64(17));

    // Receive never joins, so no MLS state is written.
    QVERIFY(database.loadMlsState(profileId).value().isEmpty());
}

void SyncStoreTest::commitHandshakeReceiveIsIdempotentOnRedelivery()
{
    QTemporaryDir directory;
    auto key = SecureBuffer::random(32);
    auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), key);
    QVERIFY(opened.hasValue());
    auto database = std::move(opened).value();
    const auto profileId = ProfileId::generate();
    QVERIFY(seedProfile(database, profileId));

    SqlCipherSyncStore store(database, profileId);

    const auto envelopeId = EnvelopeId::generate();
    const auto senderAccountId = AccountId::generate();
    const auto senderDeviceId = DeviceId::generate();
    const auto conversationId = ConversationId::generate();
    const QByteArray welcome("welcome-1");

    auto first = store.commitHandshakeReceive(envelopeId, senderAccountId, senderDeviceId,
                                              conversationId, welcome, 1'000, 3);
    QVERIFY(first.hasValue());
    QCOMPARE(first.value(), HandshakeReceiveOutcome::Stashed);

    // A redelivery of the SAME envelope is a no-op: already-seen, no duplicate stash.
    auto second = store.commitHandshakeReceive(envelopeId, senderAccountId, senderDeviceId,
                                               conversationId, welcome, 1'000, 3);
    QVERIFY(second.hasValue());
    QCOMPARE(second.value(), HandshakeReceiveOutcome::AlreadySeen);

    QCOMPARE(store.pendingHandshakes().value().size(), 1);
}

void SyncStoreTest::commitHandshakeReceiveDropsBlockedSenderButConsumesEnvelope()
{
    QTemporaryDir directory;
    auto key = SecureBuffer::random(32);
    auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), key);
    QVERIFY(opened.hasValue());
    auto database = std::move(opened).value();
    const auto profileId = ProfileId::generate();
    QVERIFY(seedProfile(database, profileId));

    SqlCipherContactRepository contacts(database);
    SqlCipherSyncRepository sync(database);
    SqlCipherSyncStore store(database, profileId);

    const auto envelopeId = EnvelopeId::generate();
    const auto senderAccountId = AccountId::generate();
    const auto senderDeviceId = DeviceId::generate();
    const auto conversationId = ConversationId::generate();

    // Pre-block the sender in the roster.
    QVERIFY(contacts.block(senderAccountId, 500).hasValue());

    auto dropped = store.commitHandshakeReceive(envelopeId, senderAccountId, senderDeviceId,
                                                conversationId, QByteArray("welcome"), 2'000, 9);
    QVERIFY(dropped.hasValue());
    QCOMPARE(dropped.value(), HandshakeReceiveOutcome::DroppedBlocked);

    // No stash was left by a blocked peer.
    QVERIFY(!store.loadPendingHandshake(conversationId).value().has_value());
    QVERIFY(store.pendingHandshakes().value().isEmpty());

    // The envelope is still consumed so the relay stops redelivering it.
    QVERIFY(store.hasSeen(envelopeId).value());
    QCOMPARE(sync.cursor(senderDeviceId).value().serverWatermark, quint64(9));
}

void SyncStoreTest::commitHandshakeAcceptPromotesContactAndDeletesStash()
{
    QTemporaryDir directory;
    auto key = SecureBuffer::random(32);
    auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), key);
    QVERIFY(opened.hasValue());
    auto database = std::move(opened).value();
    const auto profileId = ProfileId::generate();
    QVERIFY(seedProfile(database, profileId));

    SqlCipherContactRepository contacts(database);
    SqlCipherSyncStore store(database, profileId);

    const auto envelopeId = EnvelopeId::generate();
    const auto senderAccountId = AccountId::generate();
    const auto senderDeviceId = DeviceId::generate();
    const auto conversationId = ConversationId::generate();

    // A PendingIncoming contact and a stashed Welcome for its group.
    QVERIFY(contacts.recordIncomingRequest(incomingContact(senderAccountId)).hasValue());
    QVERIFY(store
                .commitHandshakeReceive(envelopeId, senderAccountId, senderDeviceId, conversationId,
                                        QByteArray("welcome"), 2'000, 4)
                .hasValue());

    const QByteArray joinedState("joined-mls-state");
    const QByteArray peerKey(32, 'k');
    auto accepted =
        store.commitHandshakeAccept(senderAccountId, conversationId, 5'000, joinedState, peerKey);
    QVERIFY(accepted.hasValue());

    // All effects landed atomically: contact flipped to Accepted with the
    // conversation id AND the authenticated peer signing key, MLS state persisted,
    // stash gone.
    auto contact = contacts.find(senderAccountId);
    QVERIFY(contact.hasValue());
    QVERIFY(contact.value().has_value());
    QCOMPARE(contact.value()->state, ContactState::Accepted);
    QVERIFY(contact.value()->conversationId.has_value());
    QCOMPARE(*contact.value()->conversationId, conversationId);
    QVERIFY(contact.value()->peerSigningKey.has_value());
    QCOMPARE(*contact.value()->peerSigningKey, peerKey);
    QVERIFY(!contact.value()->verified); // accept never asserts verification
    QCOMPARE(database.loadMlsState(profileId).value(), joinedState);
    QVERIFY(!store.loadPendingHandshake(conversationId).value().has_value());
}

void SyncStoreTest::commitHandshakeAcceptWithEmptyKeyBindsNull()
{
    QTemporaryDir directory;
    auto key = SecureBuffer::random(32);
    auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), key);
    QVERIFY(opened.hasValue());
    auto database = std::move(opened).value();
    const auto profileId = ProfileId::generate();
    QVERIFY(seedProfile(database, profileId));

    SqlCipherContactRepository contacts(database);
    SqlCipherSyncStore store(database, profileId);

    const auto envelopeId = EnvelopeId::generate();
    const auto senderAccountId = AccountId::generate();
    const auto senderDeviceId = DeviceId::generate();
    const auto conversationId = ConversationId::generate();

    QVERIFY(contacts.recordIncomingRequest(incomingContact(senderAccountId)).hasValue());
    QVERIFY(store
                .commitHandshakeReceive(envelopeId, senderAccountId, senderDeviceId, conversationId,
                                        QByteArray("welcome"), 2'000, 4)
                .hasValue());

    // An empty peer key still accepts (the safety number is simply unavailable
    // until a later backfill), binding NULL rather than a zero-length blob.
    auto accepted = store.commitHandshakeAccept(senderAccountId, conversationId, 5'000,
                                                QByteArray("joined-mls-state"), QByteArray());
    QVERIFY(accepted.hasValue());

    auto contact = contacts.find(senderAccountId);
    QVERIFY(contact.hasValue());
    QVERIFY(contact.value().has_value());
    QCOMPARE(contact.value()->state, ContactState::Accepted);
    QVERIFY(!contact.value()->peerSigningKey.has_value());
}

void SyncStoreTest::commitHandshakeAcceptRollsBackWhenNotPendingIncoming()
{
    QTemporaryDir directory;
    auto key = SecureBuffer::random(32);
    auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), key);
    QVERIFY(opened.hasValue());
    auto database = std::move(opened).value();
    const auto profileId = ProfileId::generate();
    QVERIFY(seedProfile(database, profileId));

    SqlCipherContactRepository contacts(database);
    SqlCipherSyncStore store(database, profileId);

    // A durably-committed baseline MLS state that a failed accept must never touch.
    const QByteArray baseline("baseline-state");
    QVERIFY(store.commitMlsState(baseline).hasValue());

    // Absent contact: no row matches, Conflict, and nothing is written.
    {
        const auto conversationId = ConversationId::generate();
        auto result = store.commitHandshakeAccept(AccountId::generate(), conversationId, 5'000,
                                                  QByteArray("nope"), QByteArray(32, 'k'));
        QVERIFY(!result.hasValue());
        QCOMPARE(result.error().code, RepositoryErrorCode::Conflict);
        QCOMPARE(database.loadMlsState(profileId).value(), baseline);
    }

    // Blocked contact: a stash exists but the peer was blocked mid-flight. Accept
    // rolls back wholesale -- MLS state unchanged AND the stash left intact.
    {
        const auto envelopeId = EnvelopeId::generate();
        const auto senderAccountId = AccountId::generate();
        const auto senderDeviceId = DeviceId::generate();
        const auto conversationId = ConversationId::generate();
        QVERIFY(store
                    .commitHandshakeReceive(envelopeId, senderAccountId, senderDeviceId,
                                            conversationId, QByteArray("welcome"), 2'000, 6)
                    .hasValue());
        QVERIFY(contacts.block(senderAccountId, 3'000).hasValue());

        auto result = store.commitHandshakeAccept(senderAccountId, conversationId, 5'000,
                                                  QByteArray("nope"), QByteArray(32, 'k'));
        QVERIFY(!result.hasValue());
        QCOMPARE(result.error().code, RepositoryErrorCode::Conflict);
        QCOMPARE(database.loadMlsState(profileId).value(), baseline);
        QVERIFY(store.loadPendingHandshake(conversationId).value().has_value());
        // The wholesale rollback bound no peer key to the blocked contact.
        QVERIFY(!contacts.find(senderAccountId).value().value().peerSigningKey.has_value());
    }

    // Already-accepted contact: a second accept is a no-op transition, Conflict.
    {
        const auto envelopeId = EnvelopeId::generate();
        const auto senderAccountId = AccountId::generate();
        const auto senderDeviceId = DeviceId::generate();
        const auto conversationId = ConversationId::generate();
        QVERIFY(contacts.recordIncomingRequest(incomingContact(senderAccountId)).hasValue());
        QVERIFY(contacts.markAccepted(senderAccountId, ConversationId::generate(), 4'000).hasValue());
        QVERIFY(store
                    .commitHandshakeReceive(envelopeId, senderAccountId, senderDeviceId,
                                            conversationId, QByteArray("welcome"), 2'000, 8)
                    .hasValue());

        auto result = store.commitHandshakeAccept(senderAccountId, conversationId, 5'000,
                                                  QByteArray("nope"), QByteArray(32, 'k'));
        QVERIFY(!result.hasValue());
        QCOMPARE(result.error().code, RepositoryErrorCode::Conflict);
        QCOMPARE(database.loadMlsState(profileId).value(), baseline);
        QVERIFY(store.loadPendingHandshake(conversationId).value().has_value());
        // The already-accepted contact keeps its NULL peer key: the failed second
        // accept wrote nothing.
        QVERIFY(!contacts.find(senderAccountId).value().value().peerSigningKey.has_value());
    }
}

void SyncStoreTest::deletePendingHandshakeRemovesRowAndListIsOrdered()
{
    QTemporaryDir directory;
    auto key = SecureBuffer::random(32);
    auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), key);
    QVERIFY(opened.hasValue());
    auto database = std::move(opened).value();
    const auto profileId = ProfileId::generate();
    QVERIFY(seedProfile(database, profileId));

    SqlCipherSyncStore store(database, profileId);

    const auto senderAccountId = AccountId::generate();
    const auto first = ConversationId::generate();
    const auto middle = ConversationId::generate();
    const auto last = ConversationId::generate();

    // Insert out of chronological order to prove the ORDER BY received_at_ms ASC.
    QVERIFY(store
                .commitHandshakeReceive(EnvelopeId::generate(), senderAccountId, DeviceId::generate(),
                                        last, QByteArray("w-last"), 3'000, 3)
                .hasValue());
    QVERIFY(store
                .commitHandshakeReceive(EnvelopeId::generate(), senderAccountId, DeviceId::generate(),
                                        first, QByteArray("w-first"), 1'000, 4)
                .hasValue());
    QVERIFY(store
                .commitHandshakeReceive(EnvelopeId::generate(), senderAccountId, DeviceId::generate(),
                                        middle, QByteArray("w-middle"), 2'000, 5)
                .hasValue());

    auto ordered = store.pendingHandshakes();
    QVERIFY(ordered.hasValue());
    QCOMPARE(ordered.value().size(), 3);
    QCOMPARE(ordered.value().at(0).conversationId, first);
    QCOMPARE(ordered.value().at(1).conversationId, middle);
    QCOMPARE(ordered.value().at(2).conversationId, last);

    // Deleting one row removes exactly it.
    QVERIFY(store.deletePendingHandshake(middle).hasValue());
    QVERIFY(!store.loadPendingHandshake(middle).value().has_value());
    QCOMPARE(store.pendingHandshakes().value().size(), 2);

    // Deleting an absent conversation is an idempotent no-op success.
    QVERIFY(store.deletePendingHandshake(middle).hasValue());
    QCOMPARE(store.pendingHandshakes().value().size(), 2);
}

QTEST_GUILESS_MAIN(SyncStoreTest)
#include "tst_syncstore.moc"
