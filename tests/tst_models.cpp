#include <QtTest>

#include "models/CallParticipantModel.h"
#include "models/ContactListModel.h"
#include "models/MessageListModel.h"

using OpenChat::CallParticipantModel;
using OpenChat::CallParticipantRow;
using OpenChat::Contact;
using OpenChat::ContactListModel;
using OpenChat::MessageListModel;
using OpenChat::Presence;

namespace {

QVector<Contact> seedContacts()
{
    return {
        {"michael", "Michael", Presence::Available, true, "landscape"},
        {"sarah", "Sarah", Presence::Away, true, "sarah"},
        {"tom", "Tom", Presence::Offline, false, "mono"},
    };
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
