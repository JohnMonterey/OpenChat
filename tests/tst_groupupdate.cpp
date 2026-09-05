#include <QtTest>

#include "domain/GroupUpdate.h"

#include <QCborArray>
#include <QCborValue>

using namespace OpenChat;

// The group control codec: every shape round-trips, and every malformed or
// out-of-bounds shape is refused rather than repaired.
class GroupUpdateTest final : public QObject
{
    Q_OBJECT

private slots:
    void infoRoundTripsWithRoster()
    {
        const GroupMemberInfo alice{AccountId::generate(), DeviceId::generate(),
                                    QStringLiteral("alice")};
        const GroupMemberInfo bob{AccountId::generate(), DeviceId::generate(), QString()};
        const auto message = GroupUpdateMessage::info(QStringLiteral("  Weekend plans "), {alice, bob});
        const auto decoded = decodeGroupUpdate(encodeGroupUpdate(message));
        QVERIFY(decoded.has_value());
        QCOMPARE(decoded->type, GroupUpdateType::Info);
        QCOMPARE(decoded->title, QStringLiteral("Weekend plans"));
        QCOMPARE(decoded->members.size(), 2);
        QCOMPARE(decoded->members.at(0), alice);
        QCOMPARE(decoded->members.at(1), bob);
        QVERIFY(*decoded == message);
    }

    void renameAndLeaveRoundTrip()
    {
        const auto rename = decodeGroupUpdate(encodeGroupUpdate(
            GroupUpdateMessage::rename(QStringLiteral("Book club"))));
        QVERIFY(rename.has_value());
        QCOMPARE(rename->type, GroupUpdateType::Rename);
        QCOMPARE(rename->title, QStringLiteral("Book club"));
        QVERIFY(rename->members.isEmpty());

        const auto leave = decodeGroupUpdate(encodeGroupUpdate(GroupUpdateMessage::leave()));
        QVERIFY(leave.has_value());
        QCOMPARE(leave->type, GroupUpdateType::Leave);
        QVERIFY(leave->title.isEmpty());
    }

    void titleIsCappedOnEncodeAndRejectedOnDecodeWhenTooLong()
    {
        const QString longTitle(maxGroupTitleLength + 20, QLatin1Char('x'));
        const auto capped = decodeGroupUpdate(encodeGroupUpdate(GroupUpdateMessage::rename(longTitle)));
        QVERIFY(capped.has_value());
        QCOMPARE(capped->title.size(), maxGroupTitleLength);

        // Hand-built with an over-long title: refused, not clamped.
        QCborArray fields;
        fields.append(qint64(1));
        fields.append(qint64(GroupUpdateType::Rename));
        fields.append(longTitle);
        fields.append(QCborArray());
        QVERIFY(!decodeGroupUpdate(QCborValue(fields).toCbor()).has_value());
    }

    void rejectsMalformedShapes()
    {
        const auto encode = [](const QCborArray &fields) { return QCborValue(fields).toCbor(); };
        const QByteArray account = AccountId::generate().bytes();
        const QByteArray device = DeviceId::generate().bytes();

        QVERIFY(!decodeGroupUpdate(QByteArray()).has_value());
        QVERIFY(!decodeGroupUpdate(QByteArray("garbage")).has_value());
        // Wrong version.
        QVERIFY(!decodeGroupUpdate(encode({qint64(2), qint64(0), QString(), QCborArray()})).has_value());
        // Unknown type.
        QVERIFY(!decodeGroupUpdate(encode({qint64(1), qint64(9), QString(), QCborArray()})).has_value());
        // Wrong arity.
        QVERIFY(!decodeGroupUpdate(encode({qint64(1), qint64(0), QString()})).has_value());
        // A roster on a rename.
        QCborArray member{account, device, QStringLiteral("x")};
        QVERIFY(!decodeGroupUpdate(encode({qint64(1), qint64(1), QStringLiteral("t"),
                                           QCborArray{member}})).has_value());
        // A title on a leave.
        QVERIFY(!decodeGroupUpdate(encode({qint64(1), qint64(2), QStringLiteral("t"), QCborArray()}))
                     .has_value());
        // A member with a short id.
        QCborArray badMember{QByteArray(15, 'a'), device, QString()};
        QVERIFY(!decodeGroupUpdate(encode({qint64(1), qint64(0), QString(), QCborArray{badMember}}))
                     .has_value());
        // A member with a wrong-typed name.
        QCborArray typedMember{account, device, qint64(3)};
        QVERIFY(!decodeGroupUpdate(encode({qint64(1), qint64(0), QString(), QCborArray{typedMember}}))
                     .has_value());
        // The same device twice.
        QVERIFY(!decodeGroupUpdate(encode({qint64(1), qint64(0), QString(),
                                           QCborArray{member, member}})).has_value());
        // A well-formed one still decodes, so the rejections above are specific.
        QVERIFY(decodeGroupUpdate(encode({qint64(1), qint64(0), QString(), QCborArray{member}}))
                    .has_value());
    }

    void rejectsRostersBeyondTheBound()
    {
        QCborArray members;
        for (int i = 0; i <= maxGroupMembers; ++i)
            members.append(QCborArray{AccountId::generate().bytes(), DeviceId::generate().bytes(),
                                      QString()});
        QCborArray fields{qint64(1), qint64(0), QString(), members};
        QVERIFY(!decodeGroupUpdate(QCborValue(fields).toCbor()).has_value());
    }
};

QTEST_APPLESS_MAIN(GroupUpdateTest)
#include "tst_groupupdate.moc"
