#include "network/MlsTransactionCoordinator.h"

#include <QtTest/QtTest>

#include <vector>

using namespace OpenChat;

class MlsCoordinatorTest final : public QObject
{
    Q_OBJECT

private slots:
    void reentrantSameConversationRunsFifoNotNested();
    void differentConversationsAreIndependent();
    void busyAndPendingReflectLaneState();
};

// A task that, while running, enqueues more work for the SAME conversation must
// not run that work nested: it must complete first, then the queued work runs in
// FIFO order.
void MlsCoordinatorTest::reentrantSameConversationRunsFifoNotNested()
{
    MlsTransactionCoordinator coordinator;
    const ConversationId conversation = ConversationId::generate();
    std::vector<QString> log;

    coordinator.run(conversation, [&] {
        log.push_back(QStringLiteral("A-start"));
        coordinator.run(conversation, [&] { log.push_back(QStringLiteral("B")); });
        coordinator.run(conversation, [&] { log.push_back(QStringLiteral("C")); });
        log.push_back(QStringLiteral("A-end"));
    });

    const std::vector<QString> expected{QStringLiteral("A-start"), QStringLiteral("A-end"),
                                        QStringLiteral("B"), QStringLiteral("C")};
    QCOMPARE(log, expected);
    QVERIFY(!coordinator.isBusy(conversation));
    QCOMPARE(coordinator.pendingCount(conversation), 0);
}

void MlsCoordinatorTest::differentConversationsAreIndependent()
{
    MlsTransactionCoordinator coordinator;
    const ConversationId first = ConversationId::generate();
    const ConversationId second = ConversationId::generate();
    std::vector<QString> log;

    coordinator.run(first, [&] {
        log.push_back(QStringLiteral("first-start"));
        // A different conversation is a separate lane and runs immediately.
        coordinator.run(second, [&] { log.push_back(QStringLiteral("second")); });
        log.push_back(QStringLiteral("first-end"));
    });

    const std::vector<QString> expected{QStringLiteral("first-start"), QStringLiteral("second"),
                                        QStringLiteral("first-end")};
    QCOMPARE(log, expected);
}

void MlsCoordinatorTest::busyAndPendingReflectLaneState()
{
    MlsTransactionCoordinator coordinator;
    const ConversationId conversation = ConversationId::generate();

    QVERIFY(!coordinator.isBusy(conversation));
    coordinator.run(conversation, [&] {
        QVERIFY(coordinator.isBusy(conversation));
        coordinator.run(conversation, [] {});
        QCOMPARE(coordinator.pendingCount(conversation), 1);
    });
    QVERIFY(!coordinator.isBusy(conversation));
    QCOMPARE(coordinator.pendingCount(conversation), 0);
}

QTEST_APPLESS_MAIN(MlsCoordinatorTest)
#include "tst_mlscoordinator.moc"
