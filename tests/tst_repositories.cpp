#include "storage/SqlCipherAttachmentRepository.h"
#include "storage/SqlCipherChatRepository.h"
#include "storage/RepositorySql.h"
#include "storage/SqlCipherDatabase.h"
#include "storage/SqlCipherOutboxRepository.h"
#include "storage/SqlCipherSyncRepository.h"
#include "storage/SqlCipherSyncStore.h"

#include <memory>
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

// The passphrase the raw connections below use; open() takes the same bytes.
constexpr char rawKeyPragma[] = "PRAGMA key = '0123456789abcdef0123456789abcdef';";

SecureBuffer rawKey()
{
    return SecureBuffer::fromBytes("0123456789abcdef0123456789abcdef");
}

// Runs `sql` through a separate SQLCipher connection: production repositories
// intentionally do not expose arbitrary SQL execution.
bool rawExecute(const QString &path, const char *sql)
{
    sqlite3 *handle = nullptr;
    const int opened = sqlite3_open(path.toUtf8().constData(), &handle);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> connection(handle, &sqlite3_close);
    return opened == SQLITE_OK && RepositorySql::execute(handle, rawKeyPragma)
           && RepositorySql::execute(handle, sql);
}

qint64 rawInteger(const QString &path, const char *sql)
{
    sqlite3 *handle = nullptr;
    const int opened = sqlite3_open(path.toUtf8().constData(), &handle);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> connection(handle, &sqlite3_close);
    if (opened != SQLITE_OK || !RepositorySql::execute(handle, rawKeyPragma))
        return -1;
    RepositorySql::Statement statement(handle, sql);
    if (!statement.isValid() || sqlite3_step(statement.get()) != SQLITE_ROW)
        return -1;
    return sqlite3_column_int64(statement.get(), 0);
}

// A photo's descriptor, with a fresh attachment id each time.
AttachmentDescriptor photo(qint64 byteCount = 500'000)
{
    AttachmentDescriptor descriptor;
    descriptor.key = QByteArray(AttachmentLimits::keyBytes, 'k');
    descriptor.kind = AttachmentKind::Image;
    descriptor.byteCount = byteCount;
    descriptor.sha256 = QByteArray(32, 's');
    descriptor.partCount = attachmentPartCount(byteCount);
    descriptor.mimeType = QStringLiteral("image/jpeg");
    descriptor.fileName = QStringLiteral("Harbour.jpg");
    descriptor.width = 1600;
    descriptor.height = 1200;
    descriptor.hasPreview = true;
    return descriptor;
}

MessageRecord withAttachment(MessageRecord message, const AttachmentDescriptor &descriptor)
{
    message.kind = ContentKind::Attachment;
    message.body = QStringLiteral("caption");
    message.sharedId = true;
    message.attachment = descriptor;
    return message;
}

// One attachment each way in `conversationId`, through the SyncStore as the
// engine writes them: sent from `local` to `peer`, and received from `peer`.
struct AttachmentPair final {
    MessageRecord outgoing;
    MessageRecord incoming;
};

AttachmentPair storeAttachmentPair(SqlCipherSyncStore &store, const ConversationId &conversationId,
                                   const DeviceId &local, const DeviceId &peer, quint64 sequence = 1)
{
    const auto outgoingId = MessageId::generate();
    AttachmentPair pair{
        withAttachment(outgoingMessage(outgoingId, conversationId, local), photo()),
        withAttachment(incomingMessage(MessageId::generate(), conversationId, peer, sequence),
                       photo(3 * AttachmentLimits::partBytes))};
    if (!store
             .commitAttachmentSend(pair.outgoing,
                                   {outbox(EnvelopeId::generate(), outgoingId, conversationId)},
                                   peer.bytes(), QByteArray())
             .hasValue()
        || !store.commitReceive(pair.incoming, EnvelopeId::generate(), sequence, QByteArray())
                .hasValue())
        qFatal("storeAttachmentPair");
    return pair;
}

AttachmentRef refOf(const MessageRecord &message)
{
    return AttachmentRef{message.conversationId, message.senderDeviceId, message.attachment->attachmentId};
}

} // namespace

class RepositoryTest final : public QObject
{
    Q_OBJECT

private slots:
    void existingHistoryRemainsReadWhenUpgrading();
    void readStateSurvivesReopeningAndLateMessagesStayUnread();
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
    void upgradingKeepsQueuedEnvelopesAtConversationPriority();
    void attachmentHistoryShowsDescriptorsAndProgress();
    void historyNeverFailsOverAnAttachmentItCannotRead();
    void chatRepositoryStoresAttachmentDescriptorsToo();
    void attachmentDescriptorsReadBackByMessageAndByRef();
    void attachmentStateOnlyLeavesTransferringOnce();
    void outgoingProgressOnlyMovesForward();
    void arrivalsAreCountedOncePerPart();
    void previewsAreBoundedAndKeptPerMessage();
    void orphansAndAbandonedTransfersCanBeCollected();
    void aFailedIncomingAttachmentForgetsWhatArrived();
    void requestsAreRecordedEvenBeforeAnythingArrives();
    void onlyConversationsStillHeldAreLive();
};

void RepositoryTest::existingHistoryRemainsReadWhenUpgrading()
{
    QTemporaryDir directory;
    const auto key = SecureBuffer::fromBytes("0123456789abcdef0123456789abcdef");
    {
        auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), key);
        QVERIFY(opened.hasValue());
        auto database = std::move(opened).value();
        SqlCipherChatRepository chats(database);
        const auto id = ConversationId::generate();
        QVERIFY(chats.upsertConversation(conversation(id)).hasValue());
        QVERIFY(chats.applyIncoming(incomingMessage(MessageId::generate(), id, DeviceId::generate(), 1),
                                    EnvelopeId::generate(), 1).hasValue());
    }
    // Recreate version 13 through a separate SQLCipher connection: production
    // repositories intentionally do not expose arbitrary SQL execution.
    {
        sqlite3 *handle = nullptr;
        QCOMPARE(sqlite3_open(directory.filePath("profile.sqlite3").toUtf8().constData(), &handle), SQLITE_OK);
        const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> connection(handle, &sqlite3_close);
        QVERIFY(RepositorySql::execute(handle, "PRAGMA key = '0123456789abcdef0123456789abcdef';"));
        // Undo 018 (chat attachments), 017 (profile panels) and 016 (profile
        // pages) as well: a version-13 file has none of them.
        QVERIFY(RepositorySql::execute(
            handle, "DROP TABLE message_attachments; DROP TABLE attachment_transfers; "
                    "ALTER TABLE outbox DROP COLUMN priority;"));
        QVERIFY(RepositorySql::execute(
            handle, "DROP VIEW local_page_named_media; DROP VIEW contact_page_named_media; "
                    "DROP TABLE local_page_panel_media; DROP TABLE contact_page_panel_media;"));
        QVERIFY(RepositorySql::execute(
            handle, "DROP TABLE profile_media; DROP TABLE local_profile_page; "
                    "DROP TABLE contact_pages; DROP TABLE contact_page_media; "
                    "DROP TABLE page_deliveries; DROP TABLE page_delivery_media; "
                    "DROP TABLE page_requests;"));
        QVERIFY(RepositorySql::execute(handle, "ALTER TABLE local_profiles DROP COLUMN handle;"));
        QVERIFY(RepositorySql::execute(handle, "ALTER TABLE messages DROP COLUMN shared_id;"));
        QVERIFY(RepositorySql::execute(handle, "ALTER TABLE messages DROP COLUMN edited_at_ms;"));
        QVERIFY(RepositorySql::execute(
            handle, "ALTER TABLE messages DROP COLUMN quoted_sender_device_id;"));
        QVERIFY(RepositorySql::execute(handle, "ALTER TABLE messages DROP COLUMN quoted_body;"));
        QVERIFY(RepositorySql::execute(handle, "DROP INDEX messages_unread;"));
        QVERIFY(RepositorySql::execute(handle, "ALTER TABLE messages DROP COLUMN locally_read;"));
        QVERIFY(RepositorySql::execute(handle, "PRAGMA user_version = 13;"));
    }
    auto reopened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), key);
    QVERIFY(reopened.hasValue());
    auto database = std::move(reopened).value();
    SqlCipherChatRepository chats(database);
    QCOMPARE(chats.activity().value().first().unreadCount, 0);
    QCOMPARE(chats.activity().value().first().lastMessageAtMs, qint64(2'000));
}

void RepositoryTest::readStateSurvivesReopeningAndLateMessagesStayUnread()
{
    QTemporaryDir directory;
    const auto key = SecureBuffer::random(32);
    const auto id = ConversationId::generate();
    const auto sender = DeviceId::generate();
    const auto envelope = EnvelopeId::generate();
    const auto message = incomingMessage(MessageId::generate(), id, sender, 1);
    {
        auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), key);
        QVERIFY(opened.hasValue());
        auto database = std::move(opened).value();
        SqlCipherChatRepository chats(database);
        QVERIFY(chats.upsertConversation(conversation(id)).hasValue());
        QVERIFY(chats.applyIncoming(message, envelope, 1).hasValue());
        QVERIFY(chats.applyIncoming(message, envelope, 1).hasValue());
        QCOMPARE(chats.activity().value().first().unreadCount, 1);
    }
    {
        auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), key);
        QVERIFY(opened.hasValue());
        auto database = std::move(opened).value();
        SqlCipherChatRepository chats(database);
        QCOMPARE(chats.activity().value().first().unreadCount, 1);
        QVERIFY(chats.markConversationRead(id).hasValue());
        auto late = incomingMessage(MessageId::generate(), id, sender, 2);
        late.sentAtMs = 1'000;
        QVERIFY(chats.applyIncoming(late, EnvelopeId::generate(), 2).hasValue());
        QCOMPARE(chats.activity().value().first().unreadCount, 1);
        QCOMPARE(chats.activity().value().first().lastMessageAtMs, qint64(2'000));
        auto event = incomingMessage(MessageId::generate(), id, sender, 3);
        event.kind = ContentKind::System;
        event.sentAtMs = 3'000;
        QVERIFY(chats.saveEvent(event).hasValue());
        QCOMPARE(chats.activity().value().first().unreadCount, 1);
        QCOMPARE(chats.activity().value().first().lastMessageAtMs, qint64(3'000));
        QVERIFY(chats.markConversationRead(id).hasValue());
    }
    auto reopened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), key);
    QVERIFY(reopened.hasValue());
    auto database = std::move(reopened).value();
    SqlCipherChatRepository chats(database);
    QCOMPARE(chats.activity().value().first().unreadCount, 0);
}

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

void RepositoryTest::upgradingKeepsQueuedEnvelopesAtConversationPriority()
{
    QTemporaryDir directory;
    const QString path = directory.filePath("profile.sqlite3");
    const auto conversationId = ConversationId::generate();
    const auto messageId = MessageId::generate();
    const auto envelopeId = EnvelopeId::generate();
    {
        auto opened = SqlCipherDatabase::open(path, rawKey());
        QVERIFY(opened.hasValue());
        auto database = std::move(opened).value();
        SqlCipherChatRepository chats(database);
        QVERIFY(chats.upsertConversation(conversation(conversationId)).hasValue());
        QVERIFY(chats.saveOutgoing(outgoingMessage(messageId, conversationId, DeviceId::generate()),
                                   outbox(envelopeId, messageId, conversationId))
                    .hasValue());
    }
    // Back to version 17, with the envelope still waiting.
    QVERIFY(rawExecute(path, "DROP TABLE message_attachments; DROP TABLE attachment_transfers; "
                             "ALTER TABLE outbox DROP COLUMN priority; PRAGMA user_version = 17;"));
    QCOMPARE(rawInteger(path, "PRAGMA user_version"), qint64(17));

    auto reopened = SqlCipherDatabase::open(path, rawKey());
    QVERIFY(reopened.hasValue());
    auto database = std::move(reopened).value();
    QCOMPARE(rawInteger(path, "PRAGMA user_version"), qint64(18));
    SqlCipherChatRepository chats(database);
    SqlCipherSyncStore store(database, ProfileId::generate());
    SqlCipherAttachmentRepository attachments(database);
    const auto claimed = store.claimDue(3'000, 10, 8'000);
    QVERIFY(claimed.hasValue());
    QCOMPARE(claimed.value().size(), 1);
    QCOMPARE(claimed.value().first().envelopeId, envelopeId);
    QCOMPARE(claimed.value().first().priority, 0);
    QCOMPARE(store.pendingLowPriorityCount().value(), 0);
    const auto history = chats.messages(conversationId, 50, std::nullopt);
    QVERIFY(history.hasValue());
    QCOMPARE(history.value().size(), 1);
    QVERIFY(!history.value().first().attachment);
    QCOMPARE(attachments.receivedBytes().value(), qint64(0));
}

void RepositoryTest::attachmentHistoryShowsDescriptorsAndProgress()
{
    QTemporaryDir directory;
    auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), SecureBuffer::random(32));
    QVERIFY(opened.hasValue());
    auto database = std::move(opened).value();
    SqlCipherChatRepository chats(database);
    SqlCipherSyncStore store(database, ProfileId::generate());
    SqlCipherAttachmentRepository attachments(database);
    const auto conversationId = ConversationId::generate();
    QVERIFY(chats.upsertConversation(conversation(conversationId)).hasValue());
    const auto local = DeviceId::generate();
    const auto peer = DeviceId::generate();
    const AttachmentPair pair = storeAttachmentPair(store, conversationId, local, peer);

    // Sent from here, the parts handed over count; from elsewhere, the parts held.
    QVERIFY(attachments.recordFrameSent(pair.outgoing.id, 2, true).hasValue());
    QVERIFY(attachments.recordPartArrived(refOf(pair.incoming), 1, 1000, 5'000).hasValue());
    QVERIFY(attachments.recordPartArrived(refOf(pair.incoming), 2, 1000, 5'000).hasValue());
    QVERIFY(attachments.setState(pair.incoming.id, AttachmentState::Cancelled,
                                 AttachmentFailure::SenderCancelled)
                .value());
    const auto history = chats.messages(conversationId, 50, std::nullopt);
    QVERIFY(history.hasValue());
    QCOMPARE(history.value().size(), 2);
    for (const MessageRecord &row : history.value()) {
        QVERIFY(row.attachment.has_value());
        if (row.id == pair.outgoing.id) {
            QCOMPARE(*row.attachment, *pair.outgoing.attachment);
            QCOMPARE(row.attachmentDone, 2);
            QCOMPARE(row.attachmentState, int(AttachmentState::Transferring));
        } else {
            QCOMPARE(*row.attachment, *pair.incoming.attachment);
            QCOMPARE(row.attachmentDone, 2);
            QCOMPARE(row.attachmentState, int(AttachmentState::Cancelled));
            QCOMPARE(row.attachmentReason, int(AttachmentFailure::SenderCancelled));
        }
    }
    // A text beside them has none.
    const auto textId = MessageId::generate();
    QVERIFY(chats.saveOutgoing(outgoingMessage(textId, conversationId, local, 9'000),
                               outbox(EnvelopeId::generate(), textId, conversationId))
                .hasValue());
    const auto withText = chats.messages(conversationId, 50, std::nullopt).value();
    QCOMPARE(withText.size(), 3);
    QCOMPARE(withText.first().kind, ContentKind::Text);
    QVERIFY(!withText.first().attachment);
    QCOMPARE(withText.first().attachmentDone, 0);
}

void RepositoryTest::historyNeverFailsOverAnAttachmentItCannotRead()
{
    QTemporaryDir directory;
    const QString path = directory.filePath("profile.sqlite3");
    const auto conversationId = ConversationId::generate();
    const auto local = DeviceId::generate();
    QVector<MessageRecord> rows;
    {
        auto opened = SqlCipherDatabase::open(path, rawKey());
        QVERIFY(opened.hasValue());
        auto database = std::move(opened).value();
        SqlCipherChatRepository chats(database);
        SqlCipherSyncStore store(database, ProfileId::generate());
        QVERIFY(chats.upsertConversation(conversation(conversationId)).hasValue());
        for (quint64 sequence = 1; sequence <= 4; ++sequence)
            rows.append(storeAttachmentPair(store, conversationId, local, DeviceId::generate(), sequence)
                            .incoming);
    }
    // A kind a newer build wrote before a downgrade, a name this version
    // would never store, a descriptor row gone.
    const auto execute = [&](const QString &sql, const MessageRecord &row) {
        return rawExecute(path,
                          (sql.arg(QString::fromLatin1(row.id.bytes().toHex()))).toUtf8().constData());
    };
    QVERIFY(execute(QStringLiteral("UPDATE message_attachments SET kind = 9 WHERE message_id = X'%1'"),
                    rows.at(0)));
    QVERIFY(execute(
        QStringLiteral("UPDATE message_attachments SET file_name = 'a/b' WHERE message_id = X'%1'"),
        rows.at(1)));
    QVERIFY(
        execute(QStringLiteral("DELETE FROM message_attachments WHERE message_id = X'%1'"), rows.at(2)));

    auto reopened = SqlCipherDatabase::open(path, rawKey());
    QVERIFY(reopened.hasValue());
    auto database = std::move(reopened).value();
    SqlCipherChatRepository chats(database);
    SqlCipherAttachmentRepository attachments(database);
    const auto history = chats.messages(conversationId, 50, std::nullopt);
    QVERIFY(history.hasValue());
    QCOMPARE(history.value().size(), 8);
    for (const MessageRecord &row : history.value()) {
        QCOMPARE(row.kind, ContentKind::Attachment);
        const bool unreadable
            = row.id == rows.at(0).id || row.id == rows.at(1).id || row.id == rows.at(2).id;
        QCOMPARE(row.attachment.has_value(), !unreadable);
    }
    // The transfer side treats them as absent too.
    QVERIFY(!attachments.descriptorFor(rows.at(0).id).value());
    QVERIFY(!attachments.descriptorFor(rows.at(1).id).value());
    QVERIFY(!attachments.descriptorFor(rows.at(2).id).value());
    QVERIFY(attachments.descriptorFor(rows.at(3).id).value());
    QCOMPARE(attachments.incomingActive(local).value().size(), 1);
}

void RepositoryTest::chatRepositoryStoresAttachmentDescriptorsToo()
{
    QTemporaryDir directory;
    auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), SecureBuffer::random(32));
    QVERIFY(opened.hasValue());
    auto database = std::move(opened).value();
    SqlCipherChatRepository chats(database);
    const auto conversationId = ConversationId::generate();
    QVERIFY(chats.upsertConversation(conversation(conversationId)).hasValue());

    const auto sent = withAttachment(
        outgoingMessage(MessageId::generate(), conversationId, DeviceId::generate()), photo());
    QVERIFY(chats.saveOutgoing(sent, outbox(EnvelopeId::generate(), sent.id, conversationId)).hasValue());
    const auto received = withAttachment(
        incomingMessage(MessageId::generate(), conversationId, DeviceId::generate(), 3), photo());
    QVERIFY(chats.applyIncoming(received, EnvelopeId::generate(), 3).hasValue());
    const auto history = chats.messages(conversationId, 50, std::nullopt).value();
    QCOMPARE(history.size(), 2);
    for (const MessageRecord &row : history)
        QCOMPARE(*row.attachment, row.id == sent.id ? *sent.attachment : *received.attachment);
}

void RepositoryTest::attachmentDescriptorsReadBackByMessageAndByRef()
{
    QTemporaryDir directory;
    auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), SecureBuffer::random(32));
    QVERIFY(opened.hasValue());
    auto database = std::move(opened).value();
    SqlCipherChatRepository chats(database);
    SqlCipherSyncStore store(database, ProfileId::generate(), [] { return qint64(7'000); });
    SqlCipherAttachmentRepository attachments(database);
    const auto conversationId = ConversationId::generate();
    QVERIFY(chats.upsertConversation(conversation(conversationId)).hasValue());
    const auto local = DeviceId::generate();
    const auto peer = DeviceId::generate();
    const AttachmentPair pair = storeAttachmentPair(store, conversationId, local, peer);

    const auto mine = attachments.descriptorFor(pair.outgoing.id);
    QVERIFY(mine.hasValue() && mine.value().has_value());
    const StoredAttachment &sent = *mine.value();
    QCOMPARE(sent.messageId, pair.outgoing.id);
    QCOMPARE(sent.ref, refOf(pair.outgoing));
    QCOMPARE(sent.flow, MessageFlow::Outgoing);
    QCOMPARE(sent.deliveryState, DeliveryState::Queued);
    QCOMPARE(sent.descriptor, *pair.outgoing.attachment);
    QCOMPARE(sent.descriptor.key, QByteArray(AttachmentLimits::keyBytes, 'k'));
    QCOMPARE(sent.state, AttachmentState::Transferring);
    QCOMPARE(sent.reason, AttachmentFailure::None);
    QCOMPARE(sent.partsSent, 0);
    QVERIFY(!sent.previewSent);
    QCOMPARE(sent.recipients, QList<DeviceId>{peer});
    QCOMPARE(sent.createdAtMs, qint64(7'000));

    const auto theirs = attachments.descriptorByRef(refOf(pair.incoming));
    QVERIFY(theirs.hasValue() && theirs.value().has_value());
    QCOMPARE(theirs.value()->messageId, pair.incoming.id);
    QCOMPARE(theirs.value()->flow, MessageFlow::Incoming);
    QCOMPARE(theirs.value()->deliveryState, DeliveryState::Delivered);
    QVERIFY(theirs.value()->recipients.isEmpty());
    QCOMPARE(theirs.value()->descriptor, *pair.incoming.attachment);

    // Frames are matched on all three of conversation, sender and id.
    AttachmentRef wrongSender = refOf(pair.incoming);
    wrongSender.senderDeviceId = local;
    QVERIFY(!attachments.descriptorByRef(wrongSender).value());
    AttachmentRef wrongConversation = refOf(pair.incoming);
    wrongConversation.conversationId = ConversationId::generate();
    QVERIFY(!attachments.descriptorByRef(wrongConversation).value());
    QVERIFY(!attachments.descriptorFor(MessageId::generate()).value());

    // The delivery state follows the message.
    QVERIFY(store.advanceDeliveryState(pair.outgoing.id, DeliveryState::Sending).hasValue());
    QVERIFY(store.advanceDeliveryState(pair.outgoing.id, DeliveryState::Sent).hasValue());
    QCOMPARE(attachments.descriptorFor(pair.outgoing.id).value()->deliveryState, DeliveryState::Sent);

    // Each direction's active list.
    const auto outgoing = attachments.outgoingActive(local).value();
    QCOMPARE(outgoing.size(), 1);
    QCOMPARE(outgoing.first().messageId, pair.outgoing.id);
    const auto incoming = attachments.incomingActive(local).value();
    QCOMPARE(incoming.size(), 1);
    QCOMPARE(incoming.first().messageId, pair.incoming.id);
}

void RepositoryTest::attachmentStateOnlyLeavesTransferringOnce()
{
    QTemporaryDir directory;
    auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), SecureBuffer::random(32));
    QVERIFY(opened.hasValue());
    auto database = std::move(opened).value();
    SqlCipherChatRepository chats(database);
    SqlCipherSyncStore store(database, ProfileId::generate());
    SqlCipherAttachmentRepository attachments(database);
    const auto conversationId = ConversationId::generate();
    QVERIFY(chats.upsertConversation(conversation(conversationId)).hasValue());
    const auto local = DeviceId::generate();
    const AttachmentPair pair = storeAttachmentPair(store, conversationId, local, DeviceId::generate());

    QVERIFY(attachments.setState(pair.outgoing.id, AttachmentState::Complete, AttachmentFailure::None)
                .value());
    // Finished is final: a late cancel or failure changes nothing.
    QVERIFY(!attachments
                 .setState(pair.outgoing.id, AttachmentState::Cancelled, AttachmentFailure::SendFailed)
                 .value());
    QCOMPARE(attachments.descriptorFor(pair.outgoing.id).value()->state, AttachmentState::Complete);
    QVERIFY(attachments.outgoingActive(local).value().isEmpty());
    // Back to Transferring is not a move this makes; unknown messages change nothing.
    QCOMPARE(attachments.setState(pair.incoming.id, AttachmentState::Transferring, AttachmentFailure::None)
                 .error()
                 .code,
             RepositoryErrorCode::InvalidInput);
    QVERIFY(
        !attachments.setState(MessageId::generate(), AttachmentState::Failed, AttachmentFailure::Invalid)
             .value());
    QVERIFY(attachments.setState(pair.incoming.id, AttachmentState::Failed, AttachmentFailure::NoSpace)
                .value());
    const auto failed = attachments.descriptorFor(pair.incoming.id).value();
    QCOMPARE(failed->state, AttachmentState::Failed);
    QCOMPARE(failed->reason, AttachmentFailure::NoSpace);
    QVERIFY(attachments.incomingActive(local).value().isEmpty());
}

void RepositoryTest::outgoingProgressOnlyMovesForward()
{
    QTemporaryDir directory;
    auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), SecureBuffer::random(32));
    QVERIFY(opened.hasValue());
    auto database = std::move(opened).value();
    SqlCipherChatRepository chats(database);
    SqlCipherSyncStore store(database, ProfileId::generate());
    SqlCipherAttachmentRepository attachments(database);
    const auto conversationId = ConversationId::generate();
    QVERIFY(chats.upsertConversation(conversation(conversationId)).hasValue());
    const auto id = MessageId::generate();
    const auto big = withAttachment(outgoingMessage(id, conversationId, DeviceId::generate()),
                                    photo(AttachmentLimits::maxImageBytes));
    QCOMPARE(big.attachment->partCount, 10);
    QVERIFY(store.commitAttachmentSend(big, {outbox(EnvelopeId::generate(), id, conversationId)},
                                       DeviceId::generate().bytes(), QByteArray())
                .hasValue());

    QVERIFY(attachments.recordFrameSent(id, 0, true).hasValue());
    QVERIFY(attachments.recordFrameSent(id, 4, false).hasValue());
    auto stored = attachments.descriptorFor(id).value();
    QCOMPARE(stored->partsSent, 4);
    QVERIFY(stored->previewSent); // a later call without it does not take it back
    QVERIFY(attachments.recordFrameSent(id, 2, false).hasValue());
    QCOMPARE(attachments.descriptorFor(id).value()->partsSent, 4);
    QVERIFY(attachments.recordFrameSent(id, 99, false).hasValue());
    QCOMPARE(attachments.descriptorFor(id).value()->partsSent, 10);
    QCOMPARE(chats.messages(conversationId, 50, std::nullopt).value().first().attachmentDone, 10);

    QCOMPARE(attachments.recordFrameSent(MessageId::generate(), 1, false).error().code,
             RepositoryErrorCode::NotFound);
    QCOMPARE(attachments.recordFrameSent(id, -1, false).error().code, RepositoryErrorCode::InvalidInput);
}

void RepositoryTest::arrivalsAreCountedOncePerPart()
{
    QTemporaryDir directory;
    auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), SecureBuffer::random(32));
    QVERIFY(opened.hasValue());
    auto database = std::move(opened).value();
    SqlCipherAttachmentRepository attachments(database);
    // Parts may come before any descriptor: nothing else needs to exist.
    const AttachmentRef ref{ConversationId::generate(), DeviceId::generate(), AttachmentId::generate()};
    QVERIFY(!attachments.transfer(ref).value());

    auto arrival = attachments.recordPartArrived(ref, 0, 1'000, 10'000);
    QVERIFY(arrival.hasValue());
    QVERIFY(arrival.value().changed);
    QCOMPARE(arrival.value().haveCount, 1);
    QCOMPARE(arrival.value().haveBytes, qint64(1'000));
    // The same part again (a re-sent frame) changes nothing.
    arrival = attachments.recordPartArrived(ref, 0, 1'000, 11'000);
    QVERIFY(!arrival.value().changed);
    QCOMPARE(arrival.value().haveCount, 1);
    arrival = attachments.recordPartArrived(ref, AttachmentLimits::maxParts - 1, 700, 12'000);
    QVERIFY(arrival.value().changed);
    QCOMPARE(arrival.value().haveCount, 2);
    QCOMPARE(arrival.value().haveBytes, qint64(1'700));

    auto transfer = attachments.transfer(ref).value();
    QVERIFY(transfer.has_value());
    QCOMPARE(transfer->ref, ref);
    QCOMPARE(transfer->haveCount, 2);
    QCOMPARE(transfer->firstSeenMs, qint64(10'000));
    QCOMPARE(transfer->updatedAtMs, qint64(12'000));
    QVERIFY(transfer->hasPart(0));
    QVERIFY(!transfer->hasPart(1));
    QVERIFY(transfer->hasPart(AttachmentLimits::maxParts - 1));
    QVERIFY(!transfer->hasPart(AttachmentLimits::maxParts));
    QVERIFY(!transfer->hasPart(-1));

    // A damaged part is dropped again, once.
    arrival = attachments.clearPart(ref, AttachmentLimits::maxParts - 1, 700, 13'000);
    QVERIFY(arrival.value().changed);
    QCOMPARE(arrival.value().haveCount, 1);
    QCOMPARE(arrival.value().haveBytes, qint64(1'000));
    QVERIFY(!attachments.clearPart(ref, AttachmentLimits::maxParts - 1, 700, 13'000).value().changed);
    // Clearing a part of nothing starts nothing.
    const AttachmentRef unknown{ref.conversationId, ref.senderDeviceId, AttachmentId::generate()};
    QVERIFY(!attachments.clearPart(unknown, 0, 1, 1).value().changed);
    QVERIFY(!attachments.transfer(unknown).value());

    QCOMPARE(attachments.recordPartArrived(ref, AttachmentLimits::maxParts, 1, 1).error().code,
             RepositoryErrorCode::InvalidInput);
    QCOMPARE(attachments.recordPartArrived(ref, -1, 1, 1).error().code, RepositoryErrorCode::InvalidInput);
    QCOMPARE(attachments.recordPartArrived(ref, 1, -1, 1).error().code, RepositoryErrorCode::InvalidInput);
}

void RepositoryTest::previewsAreBoundedAndKeptPerMessage()
{
    QTemporaryDir directory;
    auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), SecureBuffer::random(32));
    QVERIFY(opened.hasValue());
    auto database = std::move(opened).value();
    SqlCipherChatRepository chats(database);
    SqlCipherSyncStore store(database, ProfileId::generate());
    SqlCipherAttachmentRepository attachments(database);
    const auto conversationId = ConversationId::generate();
    QVERIFY(chats.upsertConversation(conversation(conversationId)).hasValue());
    const AttachmentPair pair
        = storeAttachmentPair(store, conversationId, DeviceId::generate(), DeviceId::generate());

    QVERIFY(attachments.preview(pair.incoming.id).value().isEmpty());
    const QByteArray jpeg("\xFF\xD8\xFF preview bytes", 17);
    QVERIFY(attachments.setPreview(pair.incoming.id, jpeg).value());
    QCOMPARE(attachments.preview(pair.incoming.id).value(), jpeg);
    QVERIFY(attachments.preview(pair.outgoing.id).value().isEmpty());
    QVERIFY(!attachments.setPreview(MessageId::generate(), jpeg).value());
    QCOMPARE(
        attachments.setPreview(pair.incoming.id, QByteArray(AttachmentLimits::maxPreviewBytes + 1, 'j'))
            .error()
            .code,
        RepositoryErrorCode::InvalidInput);
    QCOMPARE(attachments.setPreview(pair.incoming.id, QByteArray()).error().code,
             RepositoryErrorCode::InvalidInput);
    // History never carries the preview itself.
    QVERIFY(chats.messages(conversationId, 50, std::nullopt).hasValue());

    // A preview frame that came before its descriptor waits, sealed.
    const AttachmentRef early{conversationId, DeviceId::generate(), AttachmentId::generate()};
    const QByteArray sealed(AttachmentLimits::maxPreviewBytes + AttachmentLimits::controlSealOverhead, 's');
    QVERIFY(attachments.setSealedPreview(early, sealed, 20'000).hasValue());
    auto transfer = attachments.transfer(early).value();
    QCOMPARE(transfer->sealedPreview, sealed);
    QCOMPARE(transfer->haveCount, 0);
    QCOMPARE(transfer->firstSeenMs, qint64(20'000));
    QVERIFY(attachments.setSealedPreview(early, QByteArray(), 21'000).hasValue());
    QVERIFY(attachments.transfer(early).value()->sealedPreview.isEmpty());
    QCOMPARE(attachments.setSealedPreview(early, sealed + 'x', 22'000).error().code,
             RepositoryErrorCode::InvalidInput);
}

void RepositoryTest::orphansAndAbandonedTransfersCanBeCollected()
{
    QTemporaryDir directory;
    auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), SecureBuffer::random(32));
    QVERIFY(opened.hasValue());
    auto database = std::move(opened).value();
    SqlCipherChatRepository chats(database);
    SqlCipherSyncStore store(database, ProfileId::generate());
    SqlCipherAttachmentRepository attachments(database);
    const auto conversationId = ConversationId::generate();
    QVERIFY(chats.upsertConversation(conversation(conversationId)).hasValue());
    const auto peer = DeviceId::generate();
    const AttachmentPair adopted
        = storeAttachmentPair(store, conversationId, DeviceId::generate(), peer, 1);
    const AttachmentPair failed = storeAttachmentPair(store, conversationId, DeviceId::generate(), peer, 2);

    const AttachmentRef oldOrphan{conversationId, peer, AttachmentId::generate()};
    const AttachmentRef newOrphan{conversationId, peer, AttachmentId::generate()};
    const AttachmentRef strangersOrphan{conversationId, DeviceId::generate(), AttachmentId::generate()};
    QVERIFY(attachments.recordPartArrived(oldOrphan, 0, 100, 1'000).hasValue());
    QVERIFY(attachments.recordPartArrived(oldOrphan, 1, 50, 90'000).hasValue());
    QVERIFY(attachments.recordPartArrived(newOrphan, 0, 200, 80'000).hasValue());
    QVERIFY(attachments.recordPartArrived(strangersOrphan, 0, 400, 1'000).hasValue());
    QVERIFY(attachments.recordPartArrived(refOf(adopted.incoming), 0, 1'000, 1'000).hasValue());
    QVERIFY(attachments.recordPartArrived(refOf(failed.incoming), 0, 2'000, 1'000).hasValue());
    QVERIFY(attachments.setState(failed.incoming.id, AttachmentState::Failed, AttachmentFailure::Invalid)
                .value());

    // Orphans are the transfers no descriptor names, by when they started.
    const auto orphans = attachments.orphans(50'000).value();
    QCOMPARE(orphans.size(), 2);
    for (const AttachmentTransferRecord &orphan : orphans)
        QVERIFY(orphan.ref == oldOrphan || orphan.ref == strangersOrphan);
    QCOMPARE(attachments.orphans(100'000).value().size(), 3);
    QCOMPARE(attachments.orphanBytes(conversationId, peer).value(), qint64(350));
    QCOMPARE(attachments.orphanBytes(ConversationId::generate(), peer).value(), qint64(0));
    QCOMPARE(attachments.receivedBytes().value(), qint64(3'750));
    const auto abandoned = attachments.abandonedTransfers().value();
    QCOMPARE(abandoned.size(), 1);
    QCOMPARE(abandoned.first(), refOf(failed.incoming));

    QVERIFY(attachments.deleteTransfer(oldOrphan).hasValue());
    QVERIFY(attachments.deleteTransfer(refOf(failed.incoming)).hasValue());
    QVERIFY(!attachments.transfer(oldOrphan).value());
    QCOMPARE(attachments.orphans(50'000).value().size(), 1);
    QVERIFY(attachments.abandonedTransfers().value().isEmpty());
    QCOMPARE(attachments.receivedBytes().value(), qint64(1'600));
}

void RepositoryTest::aFailedIncomingAttachmentForgetsWhatArrived()
{
    QTemporaryDir directory;
    auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), SecureBuffer::random(32));
    QVERIFY(opened.hasValue());
    auto database = std::move(opened).value();
    SqlCipherChatRepository chats(database);
    SqlCipherSyncStore store(database, ProfileId::generate());
    SqlCipherAttachmentRepository attachments(database);
    const auto conversationId = ConversationId::generate();
    QVERIFY(chats.upsertConversation(conversation(conversationId)).hasValue());
    const AttachmentPair pair
        = storeAttachmentPair(store, conversationId, DeviceId::generate(), DeviceId::generate());
    QVERIFY(attachments.recordPartArrived(refOf(pair.incoming), 0, 1'000, 1'000).hasValue());
    QVERIFY(attachments.recordPartArrived(refOf(pair.incoming), 2, 1'000, 1'000).hasValue());

    QVERIFY(attachments.failIncoming(pair.incoming.id, AttachmentFailure::Invalid).value());
    const auto stored = attachments.descriptorFor(pair.incoming.id).value();
    QCOMPARE(stored->state, AttachmentState::Failed);
    QCOMPARE(stored->reason, AttachmentFailure::Invalid);
    QVERIFY(!attachments.transfer(refOf(pair.incoming)).value());
    QCOMPARE(attachments.receivedBytes().value(), qint64(0));
    // Once is enough; a finished attachment is left alone.
    QVERIFY(!attachments.failIncoming(pair.incoming.id, AttachmentFailure::NoSpace).value());
    QCOMPARE(attachments.descriptorFor(pair.incoming.id).value()->reason, AttachmentFailure::Invalid);
    QVERIFY(attachments.setState(pair.outgoing.id, AttachmentState::Complete, AttachmentFailure::None)
                .value());
    QVERIFY(!attachments.failIncoming(pair.outgoing.id, AttachmentFailure::Invalid).value());
    QVERIFY(!attachments.failIncoming(MessageId::generate(), AttachmentFailure::Invalid).value());
}

void RepositoryTest::requestsAreRecordedEvenBeforeAnythingArrives()
{
    QTemporaryDir directory;
    auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), SecureBuffer::random(32));
    QVERIFY(opened.hasValue());
    auto database = std::move(opened).value();
    SqlCipherAttachmentRepository attachments(database);
    const AttachmentRef ref{ConversationId::generate(), DeviceId::generate(), AttachmentId::generate()};

    QVERIFY(attachments.recordRequest(ref, 5'000).hasValue());
    auto transfer = attachments.transfer(ref).value();
    QCOMPARE(transfer->requestsSent, 1);
    QCOMPARE(transfer->lastRequestMs, qint64(5'000));
    QCOMPARE(transfer->haveCount, 0);
    QVERIFY(attachments.recordPartArrived(ref, 3, 10, 6'000).hasValue());
    QVERIFY(attachments.recordRequest(ref, 9'000).hasValue());
    transfer = attachments.transfer(ref).value();
    QCOMPARE(transfer->requestsSent, 2);
    QCOMPARE(transfer->lastRequestMs, qint64(9'000));
    QCOMPARE(transfer->firstSeenMs, qint64(5'000));
    QVERIFY(transfer->hasPart(3));
}

void RepositoryTest::onlyConversationsStillHeldAreLive()
{
    QTemporaryDir directory;
    auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), SecureBuffer::random(32));
    QVERIFY(opened.hasValue());
    auto database = std::move(opened).value();
    SqlCipherChatRepository chats(database);
    SqlCipherAttachmentRepository attachments(database);
    const auto group = ConversationId::generate();
    QVERIFY(chats.upsertConversation(ConversationRecord{group, group.bytes(), QStringLiteral("Trip"),
                                                        ConversationKind::Group, 1'000})
                .hasValue());
    QVERIFY(attachments.conversationIsLive(group).value());
    QVERIFY(!attachments.conversationIsLive(ConversationId::generate()).value());
    QVERIFY(chats.markConversationLeft(group, 9'000).hasValue());
    QVERIFY(!attachments.conversationIsLive(group).value());
}

QTEST_GUILESS_MAIN(RepositoryTest)
#include "tst_repositories.moc"
