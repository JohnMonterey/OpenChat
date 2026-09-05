#include "storage/SqlCipherChatRepository.h"
#include "storage/SqlCipherDatabase.h"
#include "storage/SqlCipherOutboxRepository.h"
#include "storage/SqlCipherSyncRepository.h"

#include <QTemporaryDir>
#include <QtTest/QTest>

using namespace OpenChat;

namespace {

ConversationRecord conversation(const ConversationId &id)
{
    return ConversationRecord{id, QByteArray("mls-group"), QStringLiteral("Michael"),
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
                        dueAtMs, 0, OutboxState::Pending};
}

} // namespace

class RepositoryTest final : public QObject
{
    Q_OBJECT

private slots:
    void messageAndOutboxCommitAtomically();
    void failedOutgoingWriteRollsBackBothRows();
    void incomingEnvelopeIsIdempotent();
    void deliveryStateCannotMoveBackward();
    void expiredLeaseMakesEnvelopeClaimableAgain();
    void acceptedEnvelopeIsNeverClaimedAgain();
    void messagesPageBeforeAnchor();
    void retryDelayIsCappedAndJittered();
    void retryAttemptCannotMoveBackwardOrRepeat();
    void watermarkOnlyMovesForward();
    void groupRosterTitleAndLeaveArePersisted();
};

void RepositoryTest::groupRosterTitleAndLeaveArePersisted()
{
    QTemporaryDir directory;
    auto key = SecureBuffer::random(32);
    auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), key);
    QVERIFY(opened.hasValue());
    auto database = std::move(opened).value();
    SqlCipherChatRepository chats(database);

    const auto group = ConversationId::generate();
    QVERIFY(chats.upsertConversation(ConversationRecord{group, group.bytes(), QStringLiteral("Trip"),
                                                        ConversationKind::Group, 1'000})
                .hasValue());
    // A group row reads back as a group with no members and not left.
    auto all = chats.conversations();
    QVERIFY(all.hasValue());
    QCOMPARE(all.value().size(), 1);
    QCOMPARE(all.value().first().kind, ConversationKind::Group);
    QCOMPARE(all.value().first().leftAtMs, qint64(0));
    QVERIFY(chats.groupMembers(group).value().isEmpty());

    // Members are keyed by device; re-upserting one refreshes its name.
    const auto alice = AccountId::generate();
    const auto aliceDevice = DeviceId::generate();
    const auto bobDevice = DeviceId::generate();
    QVERIFY(chats.upsertGroupMember({group, alice, aliceDevice, QStringLiteral("ali"), 2'000}).hasValue());
    QVERIFY(chats.upsertGroupMember({group, AccountId::generate(), bobDevice, QStringLiteral("bob"), 3'000})
                .hasValue());
    QVERIFY(chats.upsertGroupMember({group, alice, aliceDevice, QStringLiteral("alice"), 4'000}).hasValue());
    auto members = chats.groupMembers(group);
    QVERIFY(members.hasValue());
    QCOMPARE(members.value().size(), 2);
    QCOMPARE(members.value().at(0).displayName, QStringLiteral("alice")); // joined first, renamed
    QCOMPARE(members.value().at(0).deviceId, aliceDevice);
    QCOMPARE(members.value().at(1).displayName, QStringLiteral("bob"));

    // Removing one leaves the other; removing an unknown device is harmless.
    QVERIFY(chats.removeGroupMember(group, aliceDevice).hasValue());
    QVERIFY(chats.removeGroupMember(group, DeviceId::generate()).hasValue());
    QCOMPARE(chats.groupMembers(group).value().size(), 1);

    // The title changes in place; an unknown conversation is reported.
    QVERIFY(chats.setConversationTitle(group, QStringLiteral("Road trip")).hasValue());
    QCOMPARE(chats.conversations().value().first().title, QStringLiteral("Road trip"));
    QVERIFY(!chats.setConversationTitle(ConversationId::generate(), QStringLiteral("x")).hasValue());

    // Leaving keeps the row (and its members) but stamps it.
    QVERIFY(chats.markConversationLeft(group, 9'000).hasValue());
    QCOMPARE(chats.conversations().value().first().leftAtMs, qint64(9'000));
    QCOMPARE(chats.groupMembers(group).value().size(), 1);
    QVERIFY(!chats.markConversationLeft(ConversationId::generate(), 1).hasValue());
}

void RepositoryTest::messageAndOutboxCommitAtomically()
{
    QTemporaryDir directory;
    auto key = SecureBuffer::random(32);
    auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), key);
    QVERIFY(opened.hasValue());
    auto database = std::move(opened).value();
    SqlCipherChatRepository chats(database);
    SqlCipherOutboxRepository outgoing(database);

    const auto conversationId = ConversationId::generate();
    const auto messageId = MessageId::generate();
    QVERIFY(chats.upsertConversation(conversation(conversationId)).hasValue());
    QVERIFY(chats.saveOutgoing(outgoingMessage(messageId, conversationId, DeviceId::generate()),
                               outbox(EnvelopeId::generate(), messageId, conversationId))
                .hasValue());

    auto messages = chats.messages(conversationId, 50, std::nullopt);
    QVERIFY(messages.hasValue());
    QCOMPARE(messages.value().size(), 1);
    auto claimed = outgoing.claimDue(3'000, 10, 8'000);
    QVERIFY(claimed.hasValue());
    QCOMPARE(claimed.value().size(), 1);
}

void RepositoryTest::failedOutgoingWriteRollsBackBothRows()
{
    QTemporaryDir directory;
    auto key = SecureBuffer::random(32);
    auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), key);
    QVERIFY(opened.hasValue());
    auto database = std::move(opened).value();
    SqlCipherChatRepository chats(database);
    SqlCipherOutboxRepository outgoing(database);

    const auto conversationId = ConversationId::generate();
    QVERIFY(chats.upsertConversation(conversation(conversationId)).hasValue());
    const auto envelopeId = EnvelopeId::generate();
    const auto firstMessageId = MessageId::generate();
    QVERIFY(chats.saveOutgoing(
                     outgoingMessage(firstMessageId, conversationId, DeviceId::generate()),
                     outbox(envelopeId, firstMessageId, conversationId))
                .hasValue());

    const auto secondMessageId = MessageId::generate();
    auto result = chats.saveOutgoing(
        outgoingMessage(secondMessageId, conversationId, DeviceId::generate()),
        outbox(envelopeId, secondMessageId, conversationId));
    QVERIFY(!result.hasValue());

    auto messages = chats.messages(conversationId, 50, std::nullopt);
    QCOMPARE(messages.value().size(), 1);
    QCOMPARE(messages.value().first().id, firstMessageId);
    QCOMPARE(outgoing.claimDue(3'000, 10, 8'000).value().size(), 1);
}

void RepositoryTest::incomingEnvelopeIsIdempotent()
{
    QTemporaryDir directory;
    auto key = SecureBuffer::random(32);
    auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), key);
    QVERIFY(opened.hasValue());
    auto database = std::move(opened).value();
    SqlCipherChatRepository chats(database);
    SqlCipherSyncRepository sync(database);

    const auto conversationId = ConversationId::generate();
    const auto senderId = DeviceId::generate();
    const auto envelopeId = EnvelopeId::generate();
    const auto message = incomingMessage(MessageId::generate(), conversationId, senderId, 41);
    QVERIFY(chats.upsertConversation(conversation(conversationId)).hasValue());
    QVERIFY(chats.applyIncoming(message, envelopeId, 41).hasValue());
    QVERIFY(chats.applyIncoming(message, envelopeId, 41).hasValue());

    QCOMPARE(chats.messages(conversationId, 50, std::nullopt).value().size(), 1);
    auto seen = sync.hasSeen(envelopeId);
    QVERIFY(seen.hasValue());
    QVERIFY(seen.value());
}

void RepositoryTest::deliveryStateCannotMoveBackward()
{
    QTemporaryDir directory;
    auto key = SecureBuffer::random(32);
    auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), key);
    QVERIFY(opened.hasValue());
    auto database = std::move(opened).value();
    SqlCipherChatRepository chats(database);

    const auto conversationId = ConversationId::generate();
    const auto messageId = MessageId::generate();
    QVERIFY(chats.upsertConversation(conversation(conversationId)).hasValue());
    QVERIFY(chats.saveOutgoing(outgoingMessage(messageId, conversationId, DeviceId::generate()),
                               outbox(EnvelopeId::generate(), messageId, conversationId))
                .hasValue());
    QVERIFY(chats.advanceDeliveryState(messageId, DeliveryState::Sending).hasValue());
    auto backwards = chats.advanceDeliveryState(messageId, DeliveryState::Queued);
    QVERIFY(!backwards.hasValue());
    QCOMPARE(backwards.error().code, RepositoryErrorCode::Conflict);
}

void RepositoryTest::expiredLeaseMakesEnvelopeClaimableAgain()
{
    QTemporaryDir directory;
    auto key = SecureBuffer::random(32);
    auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), key);
    QVERIFY(opened.hasValue());
    auto database = std::move(opened).value();
    SqlCipherChatRepository chats(database);
    SqlCipherOutboxRepository outgoing(database);

    const auto conversationId = ConversationId::generate();
    const auto messageId = MessageId::generate();
    QVERIFY(chats.upsertConversation(conversation(conversationId)).hasValue());
    QVERIFY(chats.saveOutgoing(outgoingMessage(messageId, conversationId, DeviceId::generate()),
                               outbox(EnvelopeId::generate(), messageId, conversationId, 100))
                .hasValue());

    QCOMPARE(outgoing.claimDue(100, 1, 200).value().size(), 1);
    QCOMPARE(outgoing.claimDue(150, 1, 250).value().size(), 0);
    QCOMPARE(outgoing.claimDue(200, 1, 300).value().size(), 1);
}

void RepositoryTest::watermarkOnlyMovesForward()
{
    QTemporaryDir directory;
    auto key = SecureBuffer::random(32);
    auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), key);
    QVERIFY(opened.hasValue());
    auto database = std::move(opened).value();
    SqlCipherSyncRepository sync(database);
    const auto deviceId = DeviceId::generate();

    QVERIFY(sync.advanceWatermark(deviceId, 20, 2'000).hasValue());
    QVERIFY(sync.advanceWatermark(deviceId, 10, 3'000).hasValue());
    auto cursor = sync.cursor(deviceId);
    QVERIFY(cursor.hasValue());
    QCOMPARE(cursor.value().serverWatermark, quint64(20));
    QCOMPARE(cursor.value().updatedAtMs, qint64(2'000));
}

void RepositoryTest::acceptedEnvelopeIsNeverClaimedAgain()
{
    QTemporaryDir directory;
    auto key = SecureBuffer::random(32);
    auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), key);
    QVERIFY(opened.hasValue());
    auto database = std::move(opened).value();
    SqlCipherChatRepository chats(database);
    SqlCipherOutboxRepository outgoing(database);

    const auto conversationId = ConversationId::generate();
    const auto messageId = MessageId::generate();
    const auto envelopeId = EnvelopeId::generate();
    QVERIFY(chats.upsertConversation(conversation(conversationId)).hasValue());
    QVERIFY(chats.saveOutgoing(outgoingMessage(messageId, conversationId, DeviceId::generate()),
                               outbox(envelopeId, messageId, conversationId, 100))
                .hasValue());
    QCOMPARE(outgoing.claimDue(100, 1, 200).value().size(), 1);
    QVERIFY(outgoing.markAccepted(envelopeId).hasValue());
    QCOMPARE(outgoing.claimDue(1'000, 1, 2'000).value().size(), 0);
    QVERIFY(!outgoing.scheduleRetry(envelopeId, 1, 3'000).hasValue());
}

void RepositoryTest::messagesPageBeforeAnchor()
{
    QTemporaryDir directory;
    auto key = SecureBuffer::random(32);
    auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), key);
    QVERIFY(opened.hasValue());
    auto database = std::move(opened).value();
    SqlCipherChatRepository chats(database);

    const auto conversationId = ConversationId::generate();
    const auto senderId = DeviceId::generate();
    const auto oldestId = MessageId::generate();
    const auto middleId = MessageId::generate();
    const auto newestId = MessageId::generate();
    QVERIFY(chats.upsertConversation(conversation(conversationId)).hasValue());
    for (const auto &[id, sentAt] : {std::pair{oldestId, qint64(100)},
                                     std::pair{middleId, qint64(200)},
                                     std::pair{newestId, qint64(300)}}) {
        QVERIFY(chats.saveOutgoing(outgoingMessage(id, conversationId, senderId, sentAt),
                                   outbox(EnvelopeId::generate(), id, conversationId))
                    .hasValue());
    }

    auto firstPage = chats.messages(conversationId, 1, std::nullopt);
    QVERIFY(firstPage.hasValue());
    QCOMPARE(firstPage.value().size(), 1);
    QCOMPARE(firstPage.value().first().id, newestId);
    auto secondPage = chats.messages(conversationId, 2, newestId);
    QVERIFY(secondPage.hasValue());
    QCOMPARE(secondPage.value().size(), 2);
    QCOMPARE(secondPage.value().at(0).id, middleId);
    QCOMPARE(secondPage.value().at(1).id, oldestId);
}

void RepositoryTest::retryDelayIsCappedAndJittered()
{
    QCOMPARE(retryDelayMs(0, 0), qint64(1'000));
    QCOMPARE(retryDelayMs(3, 250), qint64(8'250));
    QCOMPARE(retryDelayMs(40, 1'000), qint64(301'000));
    QCOMPARE(retryDelayMs(-1, 500), qint64(1'500));
    QCOMPARE(retryDelayMs(4, -20), qint64(16'000));
}

void RepositoryTest::retryAttemptCannotMoveBackwardOrRepeat()
{
    QTemporaryDir directory;
    auto key = SecureBuffer::random(32);
    auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), key);
    QVERIFY(opened.hasValue());
    auto database = std::move(opened).value();
    SqlCipherChatRepository chats(database);
    SqlCipherOutboxRepository outgoing(database);

    const auto conversationId = ConversationId::generate();
    const auto messageId = MessageId::generate();
    const auto envelopeId = EnvelopeId::generate();
    QVERIFY(chats.upsertConversation(conversation(conversationId)).hasValue());
    QVERIFY(chats.saveOutgoing(outgoingMessage(messageId, conversationId, DeviceId::generate()),
                               outbox(envelopeId, messageId, conversationId, 100))
                .hasValue());
    QCOMPARE(outgoing.claimDue(100, 1, 200).value().size(), 1);
    QVERIFY(outgoing.scheduleRetry(envelopeId, 1, 1'100).hasValue());
    QVERIFY(!outgoing.scheduleRetry(envelopeId, 1, 1'200).hasValue());
    QVERIFY(!outgoing.scheduleRetry(envelopeId, 0, 1'200).hasValue());
}

QTEST_GUILESS_MAIN(RepositoryTest)
#include "tst_repositories.moc"
