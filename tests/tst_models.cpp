#include <QtTest>

#include "models/ContactListModel.h"
#include "models/MessageListModel.h"

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

    void whitespaceMessageIsRejected()
    {
        MessageListModel model;

        QVERIFY(!model.appendOutgoing(" \t\n ", QTime(10, 19)));
        QCOMPARE(model.rowCount(), 0);
    }
};

QTEST_MAIN(ModelsTest)

#include "tst_models.moc"
