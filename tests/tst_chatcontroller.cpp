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

    void navigationDefaultsToChatAndTransitionsOnce()
    {
        ChatController controller;

        QCOMPARE(controller.navSection(), ChatController::NavSection::Chat);
        QCOMPARE(controller.chatUnreadCount(), 3);
        QCOMPARE(controller.callMissedCount(), 1);
        QCOMPARE(controller.callCount(), 0);

        QSignalSpy navSpy(&controller, &ChatController::navSectionChanged);

        controller.setNavSection(ChatController::NavSection::Call);
        QCOMPARE(controller.navSection(), ChatController::NavSection::Call);
        QCOMPARE(navSpy.count(), 1);

        // Re-selecting the current section is a no-op and emits nothing further.
        controller.setNavSection(ChatController::NavSection::Call);
        QCOMPARE(navSpy.count(), 1);

        controller.setNavSection(ChatController::NavSection::Settings);
        QCOMPARE(controller.navSection(), ChatController::NavSection::Settings);
        QCOMPARE(navSpy.count(), 2);

        controller.setNavSection(ChatController::NavSection::Chat);
        QCOMPARE(controller.navSection(), ChatController::NavSection::Chat);
        QCOMPARE(navSpy.count(), 3);
    }

    void settingsCategoriesDriveSelectionAndElements()
    {
        ChatController controller;

        const QStringList categories = controller.settingsCategories();
        QCOMPARE(categories.size(), 7);
        QCOMPARE(categories.first(), QStringLiteral("General"));

        // Defaults to the first category.
        QCOMPARE(controller.currentSettingsCategory(), 0);
        QCOMPARE(controller.currentSettingsCategoryName(), QStringLiteral("General"));
        QVERIFY(!controller.currentSettingsElements().isEmpty());

        QSignalSpy categorySpy(&controller,
                               &ChatController::currentSettingsCategoryChanged);

        // Selecting a different category updates the index, name, and elements
        // and emits exactly once.
        controller.setCurrentSettingsCategory(2);
        QCOMPARE(controller.currentSettingsCategory(), 2);
        QCOMPARE(categorySpy.count(), 1);
        QCOMPARE(controller.currentSettingsCategoryName(), QStringLiteral("Privacy"));
        const QStringList privacy = controller.currentSettingsElements();
        QCOMPARE(privacy, (QStringList{QStringLiteral("Read receipts"),
                                       QStringLiteral("Who can contact me"),
                                       QStringLiteral("Blocked contacts"),
                                       QStringLiteral("Typing indicators")}));

        // Re-selecting the same category is a no-op and emits nothing further.
        controller.setCurrentSettingsCategory(2);
        QCOMPARE(categorySpy.count(), 1);

        // Out-of-range selections are ignored, leaving the current selection.
        controller.setCurrentSettingsCategory(-1);
        controller.setCurrentSettingsCategory(99);
        QCOMPARE(controller.currentSettingsCategory(), 2);
        QCOMPARE(categorySpy.count(), 1);
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
