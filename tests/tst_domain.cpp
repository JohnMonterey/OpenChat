#include <QtTest>

#include "core/Result.h"
#include "domain/ChatTypes.h"
#include "domain/Identifiers.h"

using OpenChat::DeliveryState;
using OpenChat::MessageId;
using OpenChat::Result;
using OpenChat::canTransition;

class DomainTest final : public QObject
{
    Q_OBJECT

private slots:
    void identifiersRejectMalformedAndNullValues()
    {
        QVERIFY(!MessageId::fromBytes(QByteArray(15, '\1')).has_value());
        QVERIFY(!MessageId::fromBytes(QByteArray(17, '\1')).has_value());
        QVERIFY(!MessageId::fromBytes(QByteArray(16, '\0')).has_value());

        const QByteArray validBytes(16, '\1');
        const auto parsed = MessageId::fromBytes(validBytes);
        QVERIFY(parsed.has_value());
        QCOMPARE(parsed->bytes(), validBytes);
        QCOMPARE(parsed->toHex(), QStringLiteral("01010101010101010101010101010101"));
    }

    void generatedIdentifiersAreValidAndDistinct()
    {
        const MessageId first = MessageId::generate();
        const MessageId second = MessageId::generate();

        QCOMPARE(first.bytes().size(), 16);
        QVERIFY(first != second);
        QVERIFY(MessageId::fromBytes(first.bytes()).has_value());
    }

    void deliveryStateRejectsRegressionsAndSkippedReceipts()
    {
        QVERIFY(canTransition(DeliveryState::Draft, DeliveryState::Queued));
        QVERIFY(canTransition(DeliveryState::Queued, DeliveryState::Sending));
        QVERIFY(canTransition(DeliveryState::Sending, DeliveryState::Sent));
        QVERIFY(canTransition(DeliveryState::Sent, DeliveryState::Delivered));
        QVERIFY(canTransition(DeliveryState::Delivered, DeliveryState::Read));
        QVERIFY(canTransition(DeliveryState::Sending, DeliveryState::Failed));

        QVERIFY(!canTransition(DeliveryState::Delivered, DeliveryState::Sending));
        QVERIFY(!canTransition(DeliveryState::Sent, DeliveryState::Read));
        QVERIFY(!canTransition(DeliveryState::Sent, DeliveryState::Failed));
        QVERIFY(!canTransition(DeliveryState::Failed, DeliveryState::Queued));
    }

    void resultSeparatesValuesFromErrors()
    {
        const auto success = Result<int, QString>::success(42);
        QVERIFY(success.hasValue());
        QCOMPARE(success.value(), 42);

        const auto failure = Result<int, QString>::failure(QStringLiteral("locked"));
        QVERIFY(!failure.hasValue());
        QCOMPARE(failure.error(), QStringLiteral("locked"));

        const auto voidSuccess = Result<void, QString>::success();
        QVERIFY(voidSuccess.hasValue());
        const auto voidFailure = Result<void, QString>::failure(QStringLiteral("disk-full"));
        QVERIFY(!voidFailure.hasValue());
        QCOMPARE(voidFailure.error(), QStringLiteral("disk-full"));
    }
};

QTEST_MAIN(DomainTest)

#include "tst_domain.moc"
