#include <QtTest>

#include "controllers/ChatController.h"

using OpenChat::ChatController;
using OpenChat::MessageListModel;

class ChatControllerTest final : public QObject
{
    Q_OBJECT

private slots:
    void startsOnMichaelWithReferenceConversation()
    {
        ChatController controller;

        QCOMPARE(controller.currentContactName(), "Michael");
        QCOMPARE(controller.currentStatusText(), "Available");
        QCOMPARE(controller.currentAvatarKey(), "michael");
        QCOMPARE(controller.messages()->rowCount(), 5);
        QCOMPARE(controller.messages()->data(controller.messages()->index(0),
                                              MessageListModel::BodyRole).toString(),
                 "Hey Daniel!");
    }

    void whitespaceCannotSend()
    {
        ChatController controller;

        controller.setComposerText("   \t ");

        QVERIFY(!controller.canSend());
        QVERIFY(!controller.sendMessage());
        QCOMPARE(controller.messages()->rowCount(), 5);
    }

    void sendTrimsAppendsAndClears()
    {
        ChatController controller;
        controller.setComposerText("  A local message  ");
        const int previousCount = controller.messages()->rowCount();

        QVERIFY(controller.canSend());
        QVERIFY(controller.sendMessage());

        QCOMPARE(controller.messages()->rowCount(), previousCount + 1);
        QCOMPARE(controller.messages()->data(controller.messages()->index(previousCount),
                                              MessageListModel::BodyRole).toString(),
                 "A local message");
        QCOMPARE(controller.composerText(), QString());
        QVERIFY(!controller.canSend());
    }

    void selectionChangesHeaderAndConversation()
    {
        ChatController controller;

        QVERIFY(controller.selectContact("sarah"));

        QCOMPARE(controller.currentContactName(), "Sarah");
        QCOMPARE(controller.currentStatusText(), "Away");
        QCOMPARE(controller.currentAvatarKey(), "sarah");
        QCOMPARE(controller.messages()->rowCount(), 0);
    }

    void searchUpdatesVisibleCounts()
    {
        ChatController controller;

        controller.setSearchQuery("tom");

        QCOMPARE(controller.contacts()->rowCount(), 1);
        QCOMPARE(controller.contacts()->favoriteCount(), 0);
        QCOMPARE(controller.contacts()->regularCount(), 1);
        QCOMPARE(controller.searchQuery(), "tom");
    }

    void defaultsToReadyWithVisiblePlaintext()
    {
        ChatController controller;

        QCOMPARE(controller.sessionState(), ChatController::SessionState::Ready);
        QVERIFY(controller.plaintextVisible());
        QVERIFY(controller.sessionStateText().isEmpty());
        QVERIFY(controller.securityNoticeText().isEmpty());
        QCOMPARE(controller.messages()->rowCount(), 5);
    }

    void lockWithholdsPlaintextAndPreservesComposer()
    {
        ChatController controller;
        controller.setComposerText("draft that must survive a lock");
        QVERIFY(controller.canSend());

        QSignalSpy stateSpy(&controller, &ChatController::sessionStateChanged);
        controller.setSessionState(ChatController::SessionState::Locked);

        QCOMPARE(stateSpy.count(), 1);
        QVERIFY(!controller.plaintextVisible());
        QCOMPARE(controller.messages()->rowCount(), 0);
        QVERIFY(!controller.securityNoticeText().isEmpty());
        QVERIFY(!controller.canSend());
        QVERIFY(!controller.sendMessage());
        QCOMPARE(controller.composerText(), "draft that must survive a lock");

        controller.setSessionState(ChatController::SessionState::Ready);
        QVERIFY(controller.plaintextVisible());
        QCOMPARE(controller.messages()->rowCount(), 5);
        QVERIFY(controller.canSend());
    }

    void offlineShowsHistoryButDefersSending()
    {
        ChatController controller;

        controller.setSessionState(ChatController::SessionState::Offline);
        QVERIFY(controller.plaintextVisible());
        QCOMPARE(controller.messages()->rowCount(), 5);
        QVERIFY(!controller.sessionStateText().isEmpty());

        controller.setComposerText("not sent while offline");
        QVERIFY(!controller.canSend());
        QVERIFY(!controller.sendMessage());
        QCOMPARE(controller.messages()->rowCount(), 5);
    }

    void quarantineAndDeviceChangeWithholdPlaintext()
    {
        ChatController controller;

        for (const auto state : {ChatController::SessionState::Quarantined,
                                 ChatController::SessionState::DeviceChanged}) {
            controller.setSessionState(state);
            QVERIFY(!controller.plaintextVisible());
            QCOMPARE(controller.messages()->rowCount(), 0);
            QVERIFY(!controller.securityNoticeText().isEmpty());
        }

        controller.setSessionState(ChatController::SessionState::Ready);
        QVERIFY(controller.plaintextVisible());
        QCOMPARE(controller.messages()->rowCount(), 5);
        QVERIFY(controller.securityNoticeText().isEmpty());
    }
};

QTEST_MAIN(ChatControllerTest)

#include "tst_chatcontroller.moc"
