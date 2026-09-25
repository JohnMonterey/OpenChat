#include <QtTest>

#include "models/CallParticipantModel.h"
#include "models/ContactListModel.h"
#include "models/MessageListModel.h"
#include "models/StagedAttachmentModel.h"

using OpenChat::AttachmentTransferState;
using OpenChat::CallParticipantModel;
using OpenChat::CallParticipantRow;
using OpenChat::Contact;
using OpenChat::ContactListModel;
using OpenChat::Message;
using OpenChat::MessageDeliveryState;
using OpenChat::MessageDirection;
using OpenChat::MessageKind;
using OpenChat::MessageListModel;
using OpenChat::Presence;
using OpenChat::StagedAttachment;
using OpenChat::StagedAttachmentModel;

namespace {

QVector<Contact> seedContacts()
{
    return {
        {"michael", "Michael", Presence::Available, true, "landscape"},
        {"sarah", "Sarah", Presence::Away, true, "sarah"},
        {"tom", "Tom", Presence::Offline, false, "mono"},
    };
}

// A photo someone is sending: three of its five parts are here.
Message incomingPhoto()
{
    Message message{MessageDirection::Incoming, QStringLiteral("Look"), QTime(9, 30), MessageKind::Attachment,
                    QDate(2026, 9, 25)};
    message.stableId = QStringLiteral("00aa");
    message.attachmentKind = 1;
    message.fileName = QStringLiteral("ferry.jpg");
    message.mimeType = QStringLiteral("image/jpeg");
    message.byteCount = 1'100'000;
    message.mediaWidth = 2048;
    message.mediaHeight = 1536;
    message.transferDone = 3;
    message.transferTotal = 5;
    message.transferPeer = QStringLiteral("Alice");
    message.hasPreview = true;
    return message;
}

QList<int> changedRoles(const QSignalSpy &spy, int at)
{
    return spy.at(at).at(2).value<QList<int>>();
}

} // namespace

class ModelsTest final : public QObject
{
    Q_OBJECT

private slots:
    void activitySortPreservesSelectionAndSearch()
    {
        ContactListModel model;
        model.setContacts(seedContacts());
        QVERIFY(model.selectContact("michael"));
        model.setActivity("tom", 200, 12);
        model.setActivity("sarah", 100, 2);
        QCOMPARE(model.contactAt(0)->id, QString("tom"));
        QCOMPARE(model.data(model.index(0), ContactListModel::UnreadCountRole).toInt(), 12);
        QCOMPARE(model.contactAt(1)->id, QString("sarah"));
        QCOMPARE(model.data(model.index(2), ContactListModel::SelectedRole).toBool(), true);
        model.setQuery("sar");
        model.setActivity("tom", 300, 13);
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.contactAt(0)->id, QString("sarah"));
        model.setQuery("");
        model.setActivity("michael", 400, 0);
        QCOMPARE(model.contactAt(0)->id, QString("michael"));
        QCOMPARE(model.data(model.index(0), ContactListModel::SelectedRole).toBool(), true);
    }

    void contactsExposeApprovedGroups()
    {
        ContactListModel model;
        model.setContacts({
            {"michael", "Michael", Presence::Available, true, "landscape"},
            {"tom", "Tom", Presence::Offline, false, "mono"},
        });

        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(model.data(model.index(0), ContactListModel::FavoriteRole).toBool(), true);
        QCOMPARE(model.data(model.index(1), ContactListModel::StatusTextRole).toString(), "Offline");
        QCOMPARE(model.favoriteCount(), 1);
        QCOMPARE(model.regularCount(), 1);
    }

    void anOfflineContactReadsOfflineWhateverTheyLastWrote()
    {
        Contact bob{"bob", "Bob", Presence::Busy, false, "mono"};
        bob.statusText = "Not available";
        ContactListModel model;
        model.setContacts({bob});
        const auto line = [&] {
            return model.data(model.index(0), ContactListModel::StatusTextRole).toString();
        };

        QCOMPARE(line(), QString("Not available"));
        QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
        model.setPresence("bob", Presence::Offline);
        QCOMPARE(line(), QString("Offline"));
        QCOMPARE(changed.count(), 1);
        QVERIFY(changed.first().at(2).value<QList<int>>().contains(ContactListModel::StatusTextRole));
        model.setPresence("bob", Presence::Away);
        QCOMPARE(line(), QString("Not available"));
    }

    void searchFiltersCaseInsensitively()
    {
        ContactListModel model;
        model.setContacts(seedContacts());

        model.setQuery("SAR");

        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.data(model.index(0), ContactListModel::NameRole).toString(), "Sarah");
        QCOMPARE(model.favoriteCount(), 1);
        QCOMPARE(model.regularCount(), 0);
    }

    void selectionIsExclusiveAndSurvivesFiltering()
    {
        ContactListModel model;
        model.setContacts(seedContacts());

        QVERIFY(model.selectContact("michael"));
        QCOMPARE(model.data(model.index(0), ContactListModel::SelectedRole).toBool(), true);
        QCOMPARE(model.data(model.index(1), ContactListModel::SelectedRole).toBool(), false);

        model.setQuery("tom");
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.data(model.index(0), ContactListModel::SelectedRole).toBool(), false);

        model.setQuery({});
        QCOMPARE(model.data(model.index(0), ContactListModel::SelectedRole).toBool(), true);
    }

    void invalidSelectionDoesNotChangeCurrentContact()
    {
        ContactListModel model;
        model.setContacts(seedContacts());
        QVERIFY(model.selectContact("michael"));

        QVERIFY(!model.selectContact("missing"));

        QCOMPARE(model.data(model.index(0), ContactListModel::SelectedRole).toBool(), true);
    }

    void outgoingMessageTrimsAndAppends()
    {
        MessageListModel model;

        QVERIFY(model.appendOutgoing("  Hello  ", QTime(10, 19)));

        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.data(model.index(0), MessageListModel::BodyRole).toString(), "Hello");
        QCOMPARE(model.data(model.index(0), MessageListModel::DirectionRole).toInt(),
                 static_cast<int>(OpenChat::MessageDirection::Outgoing));
        QCOMPARE(model.data(model.index(0), MessageListModel::TimestampRole).toString(), "10:19 AM");
    }

    void datesCreateDividersOnlyAtCalendarDayBoundaries()
    {
        MessageListModel model;
        model.setMessages({
            {OpenChat::MessageDirection::Incoming, QStringLiteral("First"), QTime(10, 15),
             OpenChat::MessageKind::Text, QDate(2010, 5, 24)},
            {OpenChat::MessageDirection::Outgoing, QStringLiteral("Same day"), QTime(10, 16),
             OpenChat::MessageKind::Text, QDate(2010, 5, 24)},
            {OpenChat::MessageDirection::Incoming, QStringLiteral("Next day"), QTime(9, 0),
             OpenChat::MessageKind::Text, QDate(2010, 5, 25)},
        });

        QCOMPARE(model.data(model.index(0), MessageListModel::DateLabelRole).toString(),
                 QStringLiteral("May 24, 2010"));
        QCOMPARE(model.data(model.index(0), MessageListModel::ShowDateDividerRole).toBool(), true);
        QCOMPARE(model.data(model.index(1), MessageListModel::ShowDateDividerRole).toBool(), false);
        QCOMPARE(model.data(model.index(2), MessageListModel::DateLabelRole).toString(),
                 QStringLiteral("May 25, 2010"));
        QCOMPARE(model.data(model.index(2), MessageListModel::ShowDateDividerRole).toBool(), true);
    }

    void datedOutgoingMessageRetainsItsCalendarDate()
    {
        MessageListModel model;

        QVERIFY(model.appendOutgoing(
            QStringLiteral("After midnight"),
            QDateTime(QDate(2026, 8, 15), QTime(0, 1))));

        QCOMPARE(model.data(model.index(0), MessageListModel::DateLabelRole).toString(),
                 QStringLiteral("August 15, 2026"));
        QCOMPARE(model.data(model.index(0), MessageListModel::TimestampRole).toString(),
                 QStringLiteral("12:01 AM"));
    }

    void whitespaceMessageIsRejected()
    {
        MessageListModel model;

        QVERIFY(!model.appendOutgoing(" \t\n ", QTime(10, 19)));
        QCOMPARE(model.rowCount(), 0);
    }

    void messageCountNotifies()
    {
        MessageListModel model;
        QSignalSpy countChanged(&model, &MessageListModel::countChanged);

        model.setMessages({
            {OpenChat::MessageDirection::Incoming, QStringLiteral("Seed"), QTime(10, 15),
             OpenChat::MessageKind::Text},
        });
        QCOMPARE(model.count(), 1);
        QCOMPARE(countChanged.count(), 1);

        QVERIFY(model.appendOutgoing(QStringLiteral("Reply"), QTime(10, 16)));
        QCOMPARE(model.count(), 2);
        QCOMPARE(countChanged.count(), 2);
    }

    void attachmentRowsExposeEveryRole()
    {
        MessageListModel model;
        Message audio{MessageDirection::Outgoing, QString(), QTime(9, 31), MessageKind::Attachment,
                      QDate(2026, 9, 25)};
        audio.stableId = QStringLiteral("00bb");
        audio.attachmentKind = 3;
        audio.byteCount = 2'516'582;
        audio.durationMs = 190'000;
        audio.peaks = {0, 128, 255};
        audio.transferState = AttachmentTransferState::Ready;
        audio.transferDone = audio.transferTotal = 12;
        model.setMessages({incomingPhoto(), audio});
        const auto data = [&model](int row, int role) { return model.data(model.index(row), role); };

        // Appended: the roles QML already binds to keep their numbers.
        QCOMPARE(int(MessageListModel::AttachmentKindRole), int(MessageListModel::SenderAccountRole) + 1);
        const QHash<int, QByteArray> names = model.roleNames();
        for (const char *name : {"attachmentKind", "fileName", "mimeType", "byteCount", "sizeText", "mediaWidth",
                                 "mediaHeight", "durationMs", "peaks", "transferState", "transferReason",
                                 "transferProgress", "transferText", "hasPreview", "previewRevision", "canCancel",
                                 "canRetry", "canSave"})
            QVERIFY2(names.values().contains(QByteArray(name)), name);

        QCOMPARE(data(0, MessageListModel::KindRole).toInt(), 4);
        QCOMPARE(data(0, MessageListModel::AttachmentKindRole).toInt(), 1);
        QCOMPARE(data(0, MessageListModel::FileNameRole).toString(), QStringLiteral("ferry.jpg"));
        QCOMPARE(data(0, MessageListModel::MimeTypeRole).toString(), QStringLiteral("image/jpeg"));
        QCOMPARE(data(0, MessageListModel::ByteCountRole).toDouble(), 1'100'000.0);
        QCOMPARE(data(0, MessageListModel::SizeTextRole).toString(), QStringLiteral("1.0 MB"));
        QCOMPARE(data(0, MessageListModel::MediaWidthRole).toInt(), 2048);
        QCOMPARE(data(0, MessageListModel::MediaHeightRole).toInt(), 1536);
        QCOMPARE(data(0, MessageListModel::TransferStateRole).toInt(), 0);
        QCOMPARE(data(0, MessageListModel::TransferProgressRole).toReal(), 0.6);
        QCOMPARE(data(0, MessageListModel::TransferTextRole).toString(), QStringLiteral("Receiving… 3 of 5"));
        QVERIFY(data(0, MessageListModel::HasPreviewRole).toBool());
        QCOMPARE(data(0, MessageListModel::PreviewRevisionRole).toInt(), 0);
        QVERIFY(!data(0, MessageListModel::CanCancelRole).toBool());
        QVERIFY(!data(0, MessageListModel::CanRetryRole).toBool());
        QVERIFY(!data(0, MessageListModel::CanSaveRole).toBool());
        QVERIFY(!data(0, MessageListModel::EditableRole).toBool());

        QCOMPARE(data(1, MessageListModel::DurationMsRole).toDouble(), 190'000.0);
        QCOMPARE(data(1, MessageListModel::PeaksRole).toList(), (QVariantList{0, 128, 255}));
        QCOMPARE(data(1, MessageListModel::SizeTextRole).toString(), QStringLiteral("2.4 MB"));
        QCOMPARE(data(1, MessageListModel::TransferProgressRole).toReal(), 1.0);
        QCOMPARE(data(1, MessageListModel::TransferTextRole).toString(), QString());
        // Audio is played, not saved; a caption is never edited.
        QVERIFY(!data(1, MessageListModel::CanSaveRole).toBool());
        QVERIFY(!data(1, MessageListModel::EditableRole).toBool());
        // Anything else has none of it.
        model.setMessages({{MessageDirection::Incoming, QStringLiteral("Hi"), QTime(9, 0), MessageKind::Text}});
        QCOMPARE(data(0, MessageListModel::AttachmentKindRole).toInt(), 0);
        QCOMPARE(data(0, MessageListModel::SizeTextRole).toString(), QString());
        QCOMPARE(data(0, MessageListModel::TransferTextRole).toString(), QString());
    }

    void transferTextSaysWhereTheBytesAre()
    {
        MessageListModel model;
        Message photo = incomingPhoto();
        const auto text = [&model](const Message &message) { return model.transferText(message); };
        QCOMPARE(text(photo), QStringLiteral("Receiving… 3 of 5"));
        photo.transferDone = 0;
        QCOMPARE(text(photo), QStringLiteral("Waiting for \u2068Alice\u2069")); // the name set apart
        photo.transferState = AttachmentTransferState::Failed;
        photo.transferReason = 1;
        QCOMPARE(text(photo), QStringLiteral("Couldn't receive this photo"));
        photo.transferReason = 2;
        QCOMPARE(text(photo), QStringLiteral("Not enough space to receive this"));
        photo.transferState = AttachmentTransferState::Cancelled;
        photo.transferReason = 3;
        QCOMPARE(text(photo), QStringLiteral("\u2068Alice\u2069 stopped sending this"));
        photo.transferState = AttachmentTransferState::Unavailable;
        QCOMPARE(text(photo), QStringLiteral("Couldn't show this attachment"));

        Message mine = incomingPhoto();
        mine.direction = MessageDirection::Outgoing;
        mine.transferPeer.clear();
        mine.transferDone = 2;
        QCOMPARE(text(mine), QStringLiteral("Sending… 40%"));
        mine.deliveryState = MessageDeliveryState::Failed;
        QCOMPARE(text(mine), QStringLiteral("Couldn't send"));
        QVERIFY(mine.canRetryTransfer());
        QVERIFY(!mine.canCancelTransfer());
        mine.deliveryState = MessageDeliveryState::Sent;
        QVERIFY(mine.canCancelTransfer());
        mine.transferState = AttachmentTransferState::Cancelled;
        mine.transferReason = 3;
        QCOMPARE(text(mine), QStringLiteral("You stopped sending this"));
        QVERIFY(mine.canRetryTransfer());
        mine.transferReason = 4;
        QCOMPARE(text(mine), QStringLiteral("Couldn't send"));

        // A call holds this device's bytes back, and its bubbles say so.
        mine.transferState = AttachmentTransferState::Transferring;
        model.setMessages({incomingPhoto(), mine});
        QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
        model.setCallActive(true);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(changed.first().at(0).toModelIndex().row(), 1);
        QCOMPARE(changedRoles(changed, 0), QList<int>{MessageListModel::TransferTextRole});
        QCOMPARE(model.data(model.index(1), MessageListModel::TransferTextRole).toString(),
                 QStringLiteral("Waiting for the call to end"));
        QCOMPARE(model.data(model.index(0), MessageListModel::TransferTextRole).toString(),
                 QStringLiteral("Receiving… 3 of 5"));
        model.setCallActive(true);
        QCOMPARE(changed.count(), 1);
    }

    void transfersAndPreviewsChangeOnlyTheirRoles()
    {
        MessageListModel model;
        model.setMessages({incomingPhoto()});
        QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);

        QVERIFY(model.updateTransfer(QStringLiteral("00aa"), AttachmentTransferState::Transferring, 0, 4, 5));
        QCOMPARE(changed.count(), 1);
        const QList<int> roles = changedRoles(changed, 0);
        QVERIFY(roles.contains(MessageListModel::TransferStateRole));
        QVERIFY(roles.contains(MessageListModel::TransferProgressRole));
        QVERIFY(roles.contains(MessageListModel::TransferTextRole));
        QVERIFY(roles.contains(MessageListModel::CanSaveRole));
        QVERIFY(!roles.contains(MessageListModel::BodyRole));
        QVERIFY(!roles.contains(MessageListModel::PreviewRevisionRole));
        QCOMPARE(model.data(model.index(0), MessageListModel::TransferTextRole).toString(),
                 QStringLiteral("Receiving… 4 of 5"));
        // The same again changes nothing.
        QVERIFY(model.updateTransfer(QStringLiteral("00aa"), AttachmentTransferState::Transferring, 0, 4, 5));
        QCOMPARE(changed.count(), 1);
        QVERIFY(model.updateTransfer(QStringLiteral("00aa"), AttachmentTransferState::Ready, 0, 5, 5));
        QVERIFY(model.data(model.index(0), MessageListModel::CanSaveRole).toBool());
        QVERIFY(!model.updateTransfer(QStringLiteral("nothing"), AttachmentTransferState::Ready, 0, 1, 1));

        QVERIFY(model.bumpPreview(QStringLiteral("00aa")));
        QCOMPARE(changedRoles(changed, 2),
                 (QList<int>{MessageListModel::HasPreviewRole, MessageListModel::PreviewRevisionRole}));
        QCOMPARE(model.data(model.index(0), MessageListModel::PreviewRevisionRole).toInt(), 1);

        // An attachment's actions follow its message's delivery too.
        Message mine = incomingPhoto();
        mine.stableId = QStringLiteral("00cc");
        mine.direction = MessageDirection::Outgoing;
        mine.deliveryState = MessageDeliveryState::Sending;
        model.appendMessage(mine);
        QVERIFY(model.updateDeliveryState(QStringLiteral("00cc"), MessageDeliveryState::Failed));
        const QList<int> delivery = changedRoles(changed, changed.count() - 1);
        QVERIFY(delivery.contains(MessageListModel::CanRetryRole));
        QVERIFY(delivery.contains(MessageListModel::TransferTextRole));
        QVERIFY(model.data(model.index(1), MessageListModel::CanRetryRole).toBool());
    }

    void sizesAndLengthsReadAsCardsPrintThem()
    {
        QCOMPARE(MessageListModel::sizeText(0), QStringLiteral("0 bytes"));
        QCOMPARE(MessageListModel::sizeText(1), QStringLiteral("1 byte"));
        QCOMPARE(MessageListModel::sizeText(1023), QStringLiteral("1023 bytes"));
        QCOMPARE(MessageListModel::sizeText(1024), QStringLiteral("1 KB"));
        QCOMPARE(MessageListModel::sizeText(340 * 1024 + 100), QStringLiteral("340 KB"));
        QCOMPARE(MessageListModel::sizeText(2'516'582), QStringLiteral("2.4 MB"));
        QCOMPARE(MessageListModel::sizeText(16LL * 1024 * 1024), QStringLiteral("16 MB"));
        QCOMPARE(MessageListModel::durationText(42'000), QStringLiteral("0:42"));
        QCOMPARE(MessageListModel::durationText(245'999), QStringLiteral("4:05"));
        QCOMPARE(MessageListModel::durationText(-5), QStringLiteral("0:00"));
    }

    void stagedCardsSignalOnlyWhatMoved()
    {
        StagedAttachmentModel model;
        QSignalSpy count(&model, &StagedAttachmentModel::countChanged);
        QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
        QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
        const QHash<int, QByteArray> names = model.roleNames();
        for (const char *name : {"stagedId", "kind", "name", "sizeText", "progress", "ready", "failed", "error",
                                 "notice", "previewKey", "durationText"})
            QVERIFY2(names.values().contains(QByteArray(name)), name);

        StagedAttachment video;
        video.id = QStringLiteral("v");
        video.kind = 2;
        video.name = QStringLiteral("walk.mp4");
        model.append(video);
        StagedAttachment file;
        file.id = QStringLiteral("f");
        file.name = QStringLiteral("plan.pdf");
        file.byteCount = 2048;
        model.append(file);
        QCOMPARE(model.count(), 2);
        QCOMPARE(count.count(), 2);
        QCOMPARE(inserted.count(), 2);
        const auto data = [&model](int row, int role) { return model.data(model.index(row), role); };
        QCOMPARE(data(0, StagedAttachmentModel::SizeTextRole).toString(), QString());
        QCOMPARE(data(1, StagedAttachmentModel::SizeTextRole).toString(), QStringLiteral("2 KB"));
        QCOMPARE(data(0, StagedAttachmentModel::DurationTextRole).toString(), QString());

        // Progress in steps of a percent, whatever the importer reports.
        QVERIFY(model.setProgress(QStringLiteral("v"), 0.004));
        QCOMPARE(changed.count(), 0);
        QVERIFY(model.setProgress(QStringLiteral("v"), 0.25));
        QCOMPARE(changed.count(), 1);
        QCOMPARE(changedRoles(changed, 0), QList<int>{StagedAttachmentModel::ProgressRole});
        QCOMPARE(data(0, StagedAttachmentModel::ProgressRole).toReal(), 0.25);

        // Ready: only the roles that moved.
        video.ready = true;
        video.progress = 1.0;
        video.byteCount = 3 * 1024 * 1024;
        video.durationMs = 60'000;
        video.previewKey = QStringLiteral("staged:v");
        video.notice = QStringLiteral("Only the first minute will be sent.");
        QVERIFY(model.update(video));
        const QList<int> roles = changedRoles(changed, 1);
        QVERIFY(roles.contains(StagedAttachmentModel::ReadyRole));
        QVERIFY(roles.contains(StagedAttachmentModel::SizeTextRole));
        QVERIFY(roles.contains(StagedAttachmentModel::PreviewKeyRole));
        QVERIFY(roles.contains(StagedAttachmentModel::DurationTextRole));
        QVERIFY(!roles.contains(StagedAttachmentModel::NameRole));
        QVERIFY(!roles.contains(StagedAttachmentModel::ErrorRole));
        QCOMPARE(data(0, StagedAttachmentModel::DurationTextRole).toString(), QStringLiteral("1:00"));
        QCOMPARE(data(0, StagedAttachmentModel::NoticeRole).toString(),
                 QStringLiteral("Only the first minute will be sent."));
        QVERIFY(model.update(video));
        QCOMPARE(changed.count(), 2);

        QVERIFY(model.remove(QStringLiteral("f")));
        QVERIFY(!model.remove(QStringLiteral("f")));
        QCOMPARE(model.count(), 1);
        model.clear();
        QCOMPARE(model.count(), 0);
        QCOMPARE(count.count(), 4);
    }

    // A group call tile opens its member's profile by account, contact or
    // not, so every row carries the account the call knows for it.
    void callParticipantsExposeAccountIds()
    {
        CallParticipantModel model;
        const QHash<int, QByteArray> roles = model.roleNames();
        QCOMPARE(roles.value(CallParticipantModel::AccountIdRole), QByteArray("accountId"));
        // Appended: the roles QML already binds to keep their numbers.
        QCOMPARE(int(CallParticipantModel::AccountIdRole),
                 int(CallParticipantModel::ScreenSharingRole) + 1);
        QCOMPARE(roles.value(CallParticipantModel::DeviceIdRole), QByteArray("deviceId"));
        QCOMPARE(roles.value(CallParticipantModel::ScreenSharingRole), QByteArray("screenSharing"));

        const QString contactAccount = QStringLiteral("0a1b2c3d4e5f60718293a4b5c6d7e8f9");
        const QString strangerAccount = QStringLiteral("f9e8d7c6b5a4938271605f4e3d2c1b0a");
        CallParticipantRow contact{QStringLiteral("d1"), QStringLiteral("Jessica"),
                                   QStringLiteral("jessica"), QString(), true, false, false, 0.0};
        contact.accountId = contactAccount;
        CallParticipantRow stranger{QStringLiteral("d2"), QStringLiteral("dana"),
                                    QStringLiteral("userpfp_none"), QStringLiteral("Ringing…"),
                                    false, true, false, 0.0};
        stranger.accountId = strangerAccount;
        // A member who joined mid-call before the roster named them.
        const CallParticipantRow unknown{QStringLiteral("d3"), QStringLiteral("Unknown"),
                                         QStringLiteral("userpfp_none"), QString(), true, false,
                                         false, 0.0};
        model.setParticipants({contact, stranger, unknown});

        QCOMPARE(model.rowCount(), 3);
        QCOMPARE(model.data(model.index(0), CallParticipantModel::AccountIdRole).toString(),
                 contactAccount);
        QCOMPARE(model.data(model.index(1), CallParticipantModel::AccountIdRole).toString(),
                 strangerAccount);
        const QVariant none = model.data(model.index(2), CallParticipantModel::AccountIdRole);
        QVERIFY(none.isValid());
        QVERIFY(none.toString().isEmpty());
        // The other roles are where they were.
        QCOMPARE(model.data(model.index(1), CallParticipantModel::NameRole).toString(),
                 QStringLiteral("dana"));
        QCOMPARE(model.data(model.index(1), CallParticipantModel::DeviceIdRole).toString(),
                 QStringLiteral("d2"));

        // The roster names the late joiner: the same devices in the same order
        // update in place, and the tile learns the account.
        QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
        QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
        CallParticipantRow named = unknown;
        named.name = QStringLiteral("Ryan");
        named.accountId = QStringLiteral("00112233445566778899aabbccddeeff");
        model.setParticipants({contact, stranger, named});
        QCOMPARE(reset.count(), 0);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(model.data(model.index(2), CallParticipantModel::AccountIdRole).toString(),
                 named.accountId);
        QCOMPARE(model.participants().at(2).accountId, named.accountId);
    }
};

QTEST_MAIN(ModelsTest)

#include "tst_models.moc"
