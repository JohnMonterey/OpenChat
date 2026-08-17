#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQuickItem>
#include <QWindow>
#include <QtTest>

#include <optional>

#include "controllers/ChatController.h"
#include "controllers/ContactController.h"
#include "controllers/OnboardingController.h"
#include "models/RequestListModel.h"
#include "render/AvatarArtwork.h"
#include "render/BubbleBackground.h"

namespace {

QQuickItem *findVisualItem(QQuickItem *root, const QString &objectName)
{
    if (!root)
        return nullptr;
    if (root->objectName() == objectName)
        return root;
    for (QQuickItem *child : root->childItems()) {
        if (QQuickItem *match = findVisualItem(child, objectName))
            return match;
    }
    return nullptr;
}

} // namespace

class QmlLoadTest final : public QObject
{
    Q_OBJECT

private slots:
    void requiredStructure()
    {
        OpenChat::ChatController controller;
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{QStringLiteral("chatController"), QVariant::fromValue(&controller)}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");

        QCOMPARE(engine.rootObjects().size(), 1);
        QObject *root = engine.rootObjects().constFirst();
        auto *window = qobject_cast<QWindow *>(root);
        QVERIFY(window);
        QCOMPARE(root->property("minimumWidth").toInt(), 720);
        QCOMPARE(root->property("minimumHeight").toInt(), 560);
        QVERIFY(!(window->flags() & Qt::FramelessWindowHint));
        QVERIFY(!root->findChild<QObject *>(QStringLiteral("aeroWindowFrame")));
        QObject *sidebar =
            root->findChild<QObject *>(QStringLiteral("contactSidebar"));
        QVERIFY(sidebar);
        QVERIFY(root->findChild<QObject *>(QStringLiteral("favoritesCategory")));
        QVERIFY(root->findChild<QObject *>(QStringLiteral("contactsCategory")));
        QVERIFY(root->findChild<QObject *>(QStringLiteral("contactSearch")));

        window->setHeight(560);
        QCoreApplication::processEvents();
        QVERIFY(sidebar->property("contactRowHeight").toDouble() >= 44.0);
        QVERIFY(sidebar->property("contactRowHeight").toDouble() <= 47.0);
    }

    void conversationStructureAndSending()
    {
        OpenChat::ChatController controller;
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{QStringLiteral("chatController"), QVariant::fromValue(&controller)}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");

        QCOMPARE(engine.rootObjects().size(), 1);
        QObject *root = engine.rootObjects().constFirst();
        QVERIFY(root->findChild<QObject *>(QStringLiteral("conversationHeader")));
        QVERIFY(root->findChild<QObject *>(QStringLiteral("videoCallButton")));
        QVERIFY(root->findChild<QObject *>(QStringLiteral("phoneCallButton")));
        QVERIFY(root->findChild<QObject *>(QStringLiteral("videoCallFallback")));
        QVERIFY(root->findChild<QObject *>(QStringLiteral("phoneCallFallback")));
        QVERIFY(root->findChild<QObject *>(QStringLiteral("messageHistory")));
        QVERIFY(root->findChild<QObject *>(QStringLiteral("messageComposer")));

        QObject *input = root->findChild<QObject *>(QStringLiteral("messageInput"));
        QObject *send = root->findChild<QObject *>(QStringLiteral("sendButton"));
        QVERIFY(input);
        QVERIFY(send);
        QCOMPARE(send->property("enabled").toBool(), false);

        const int before = controller.messages()->rowCount();
        QVERIFY(input->setProperty("text", QStringLiteral("Hello")));
        QCoreApplication::processEvents();
        QCOMPARE(send->property("enabled").toBool(), true);
        QVERIFY(QMetaObject::invokeMethod(send, "clicked"));
        QCoreApplication::processEvents();
        QCOMPARE(controller.messages()->rowCount(), before + 1);
        QCOMPARE(controller.composerText(), QString());

        root->setProperty("height", 560);
        QCoreApplication::processEvents();
        QObject *messageList =
            root->findChild<QObject *>(QStringLiteral("messageList"));
        QVERIFY(messageList);
        QTRY_VERIFY(messageList->property("atYEnd").toBool());
    }

    void bottomNavigationSwitchesMainPane()
    {
        OpenChat::ChatController controller;
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{QStringLiteral("chatController"), QVariant::fromValue(&controller)}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");

        QCOMPARE(engine.rootObjects().size(), 1);
        QObject *root = engine.rootObjects().constFirst();

        QObject *bottomNav = root->findChild<QObject *>(QStringLiteral("bottomNav"));
        QObject *callTab = root->findChild<QObject *>(QStringLiteral("callTab"));
        QObject *chatTab = root->findChild<QObject *>(QStringLiteral("chatTab"));
        QObject *settingsTab = root->findChild<QObject *>(QStringLiteral("settingsTab"));
        QObject *conversationPane =
            root->findChild<QObject *>(QStringLiteral("conversationPane"));
        QObject *callView = root->findChild<QObject *>(QStringLiteral("callView"));
        QObject *settingsView = root->findChild<QObject *>(QStringLiteral("settingsView"));
        QObject *chatContactArea =
            root->findChild<QObject *>(QStringLiteral("chatContactArea"));
        QObject *sidebarCallList =
            root->findChild<QObject *>(QStringLiteral("sidebarCallList"));
        QVERIFY(bottomNav);
        QVERIFY(callTab);
        QVERIFY(chatTab);
        QVERIFY(settingsTab);
        QVERIFY(conversationPane);
        QVERIFY(callView);
        QVERIFY(settingsView);
        QVERIFY(chatContactArea);
        QVERIFY(sidebarCallList);

        // Chat is active by default: the conversation pane and its content are
        // present and shown, both placeholder panes are hidden, and only the
        // chat tab reads as active.
        QVERIFY(root->findChild<QObject *>(QStringLiteral("conversationHeader")));
        QVERIFY(root->findChild<QObject *>(QStringLiteral("messageHistory")));
        QVERIFY(root->findChild<QObject *>(QStringLiteral("messageComposer")));
        QVERIFY(conversationPane->property("visible").toBool());
        QVERIFY(!callView->property("visible").toBool());
        QVERIFY(!settingsView->property("visible").toBool());
        QVERIFY(chatTab->property("active").toBool());
        QVERIFY(!callTab->property("active").toBool());
        QVERIFY(!settingsTab->property("active").toBool());

        // The sidebar's middle band swaps with the section: Chat shows the
        // contact list, the call history list is hidden.
        QVERIFY(chatContactArea->property("visible").toBool());
        QVERIFY(!sidebarCallList->property("visible").toBool());

        // Badge labels are backed by the controller's counts, not hardcoded.
        QObject *chatBadge = root->findChild<QObject *>(QStringLiteral("chatBadgeLabel"));
        QObject *callBadge = root->findChild<QObject *>(QStringLiteral("callBadgeLabel"));
        QVERIFY(chatBadge);
        QVERIFY(callBadge);
        QCOMPARE(chatBadge->property("text").toString(),
                 QString::number(controller.chatUnreadCount()));
        QCOMPARE(callBadge->property("text").toString(),
                 QString::number(controller.callMissedCount()));
        QCOMPARE(chatBadge->property("text").toString(), QStringLiteral("3"));
        QCOMPARE(callBadge->property("text").toString(), QStringLiteral("1"));

        // Call: the call placeholder replaces the chat pane exclusively, and the
        // sidebar swaps the contact list for the call history list with its
        // empty state showing.
        controller.setNavSection(OpenChat::ChatController::NavSection::Call);
        QCoreApplication::processEvents();
        QVERIFY(callView->property("visible").toBool());
        QVERIFY(!conversationPane->property("visible").toBool());
        QVERIFY(!settingsView->property("visible").toBool());
        QVERIFY(callTab->property("active").toBool());
        QVERIFY(!chatTab->property("active").toBool());
        QVERIFY(!chatContactArea->property("visible").toBool());
        QVERIFY(sidebarCallList->property("visible").toBool());
        QObject *noCallsYet = root->findChild<QObject *>(QStringLiteral("noCallsYet"));
        QVERIFY(noCallsYet);
        QCOMPARE(noCallsYet->property("text").toString(), QStringLiteral("No calls yet."));

        // Settings: the detail pane replaces the chat pane, the sidebar swaps in
        // the category list, and the detail title tracks the selected category.
        controller.setNavSection(OpenChat::ChatController::NavSection::Settings);
        QCoreApplication::processEvents();
        QVERIFY(settingsView->property("visible").toBool());
        QVERIFY(!conversationPane->property("visible").toBool());
        QVERIFY(!callView->property("visible").toBool());
        QVERIFY(settingsTab->property("active").toBool());
        QVERIFY(!callTab->property("active").toBool());

        QObject *settingsCategoryList =
            root->findChild<QObject *>(QStringLiteral("settingsCategoryList"));
        QObject *settingsDetail =
            root->findChild<QObject *>(QStringLiteral("settingsDetail"));
        QObject *settingsDetailTitle =
            root->findChild<QObject *>(QStringLiteral("settingsDetailTitle"));
        QVERIFY(settingsCategoryList);
        QVERIFY(settingsDetail);
        QVERIFY(settingsDetailTitle);
        QVERIFY(settingsCategoryList->property("visible").toBool());
        QVERIFY(!chatContactArea->property("visible").toBool());
        QVERIFY(!sidebarCallList->property("visible").toBool());
        QCOMPARE(settingsDetailTitle->property("text").toString(), QStringLiteral("General"));

        controller.setCurrentSettingsCategory(3);
        QCoreApplication::processEvents();
        QCOMPARE(settingsDetailTitle->property("text").toString(),
                 QStringLiteral("Notifications"));

        // Return to the default category so later assertions are unaffected.
        controller.setCurrentSettingsCategory(0);
        QCoreApplication::processEvents();

        // Back to Chat restores the conversation pane and the contact list.
        controller.setNavSection(OpenChat::ChatController::NavSection::Chat);
        QCoreApplication::processEvents();
        QVERIFY(conversationPane->property("visible").toBool());
        QVERIFY(!callView->property("visible").toBool());
        QVERIFY(!settingsView->property("visible").toBool());
        QVERIFY(chatTab->property("active").toBool());
        QVERIFY(chatContactArea->property("visible").toBool());
        QVERIFY(!sidebarCallList->property("visible").toBool());
    }

    void messageListStartsAtHistoryTopWithoutStationaryDivider()
    {
        OpenChat::ChatController controller;
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{QStringLiteral("chatController"), QVariant::fromValue(&controller)}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");

        QCOMPARE(engine.rootObjects().size(), 1);
        QObject *root = engine.rootObjects().constFirst();
        QObject *history = root->findChild<QObject *>(QStringLiteral("messageHistory"));
        QObject *messageList = root->findChild<QObject *>(QStringLiteral("messageList"));
        QVERIFY(history);
        QVERIFY(messageList);
        QCOMPARE(messageList->property("y").toReal(), 0.0);

        auto *listItem = qobject_cast<QQuickItem *>(messageList);
        QVERIFY(listItem);
        QTRY_VERIFY(findVisualItem(listItem, QStringLiteral("scrollingDateDivider")));
        QQuickItem *divider =
            findVisualItem(listItem, QStringLiteral("scrollingDateDivider"));
        root->setProperty("height", 560);
        QTRY_VERIFY(messageList->property("atYEnd").toBool());
        const qreal maximumContentY = messageList->property("contentHeight").toReal()
            - messageList->property("height").toReal();
        QVERIFY(maximumContentY > 0.0);
        QVERIFY(QMetaObject::invokeMethod(messageList, "positionViewAtBeginning"));
        QTRY_VERIFY(messageList->property("atYBeginning").toBool());
        const qreal dividerAtBeginning = divider->mapToItem(listItem, QPointF()).y();

        QVERIFY(QMetaObject::invokeMethod(messageList, "positionViewAtEnd"));
        QTRY_VERIFY(messageList->property("atYEnd").toBool());
        const qreal dividerAtEnd = divider->mapToItem(listItem, QPointF()).y();
        QVERIFY(dividerAtEnd < dividerAtBeginning);
    }

    void composerUsesUnifiedAdaptiveInputFrame()
    {
        OpenChat::ChatController controller;
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{QStringLiteral("chatController"), QVariant::fromValue(&controller)}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");

        QCOMPARE(engine.rootObjects().size(), 1);
        QObject *root = engine.rootObjects().constFirst();
        QObject *composer = root->findChild<QObject *>(QStringLiteral("messageComposer"));
        QObject *frame = root->findChild<QObject *>(QStringLiteral("composerInputFrame"));
        QObject *attachment = root->findChild<QObject *>(QStringLiteral("attachmentButton"));
        QObject *input = root->findChild<QObject *>(QStringLiteral("messageInput"));
        QVERIFY(composer);
        QVERIFY(frame);
        QVERIFY(attachment);
        QVERIFY(input);

        QCoreApplication::processEvents();
        const qreal singleLineHeight = frame->property("height").toReal();
        QVERIFY(singleLineHeight >= 46.0);
        QVERIFY(singleLineHeight <= 50.0);
        QCOMPARE(attachment->property("height").toReal(), singleLineHeight);
        QCOMPARE(attachment->property("y").toReal(), 0.0);

        QVERIFY(input->setProperty("text", QStringLiteral("First line\nSecond line\nThird line")));
        QCoreApplication::processEvents();
        const qreal multilineHeight = frame->property("height").toReal();
        QVERIFY(multilineHeight > singleLineHeight);
        QCOMPARE(attachment->property("height").toReal(), multilineHeight);
        QCOMPARE(composer->property("height").toReal(), multilineHeight + 34.0);

        QVERIFY(input->setProperty("text", QString()));
        QCoreApplication::processEvents();
        QCOMPARE(frame->property("height").toReal(), singleLineHeight);
    }

    void messageTimestampFormatting()
    {
        QQmlEngine engine;
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        QQmlComponent component(&engine);
        component.loadFromModule("OpenChat", "MessageDelegate");
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));

        QScopedPointer<QObject> delegate(component.createWithInitialProperties(
            {{QStringLiteral("direction"), 0},
             {QStringLiteral("body"), QStringLiteral("Hello")},
             {QStringLiteral("timestamp"), QStringLiteral("10:15 AM")},
             {QStringLiteral("kind"), 0},
             {QStringLiteral("dateLabel"), QStringLiteral("May 24, 2010")},
             {QStringLiteral("showDateDivider"), false},
             {QStringLiteral("width"), 540}}));
        QVERIFY(delegate);
        QObject *timestamp =
            delegate->findChild<QObject *>(QStringLiteral("messageTimestamp"));
        QVERIFY(timestamp);
        QCOMPARE(timestamp->property("text").toString(), QStringLiteral("10:15 AM"));
    }

    void emptyStatesAndResponsiveSidebar()
    {
        OpenChat::ChatController controller;
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{QStringLiteral("chatController"), QVariant::fromValue(&controller)}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");

        QCOMPARE(engine.rootObjects().size(), 1);
        QObject *root = engine.rootObjects().constFirst();
        auto *window = qobject_cast<QWindow *>(root);
        QVERIFY(window);

        window->setWidth(720);
        QCoreApplication::processEvents();
        QCOMPARE(root->property("sidebarWidth").toInt(), 250);
        window->setWidth(860);
        QCoreApplication::processEvents();
        QCOMPARE(root->property("sidebarWidth").toInt(), 268);
        window->setWidth(1024);
        QCoreApplication::processEvents();
        QCOMPARE(root->property("sidebarWidth").toInt(), 300);

        QObject *noContacts =
            root->findChild<QObject *>(QStringLiteral("noContactsFound"));
        QObject *noMessages =
            root->findChild<QObject *>(QStringLiteral("noMessagesYet"));
        QVERIFY(noContacts);
        QVERIFY(noMessages);
        QVERIFY(!noContacts->property("visible").toBool());
        QVERIFY(!noMessages->property("visible").toBool());

        controller.setSearchQuery(QStringLiteral("does-not-exist"));
        QCoreApplication::processEvents();
        QVERIFY(noContacts->property("visible").toBool());

        QVERIFY(controller.selectContact(QStringLiteral("sarah")));
        QCoreApplication::processEvents();
        QVERIFY(noMessages->property("visible").toBool());
    }

    void securityStatesHideUnverifiedPlaintext()
    {
        OpenChat::ChatController controller;
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{QStringLiteral("chatController"), QVariant::fromValue(&controller)}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");

        QCOMPARE(engine.rootObjects().size(), 1);
        QObject *root = engine.rootObjects().constFirst();
        QObject *banner = root->findChild<QObject *>(QStringLiteral("securityBanner"));
        QObject *messageList = root->findChild<QObject *>(QStringLiteral("messageList"));
        QObject *notice = root->findChild<QObject *>(QStringLiteral("securityNotice"));
        QObject *input = root->findChild<QObject *>(QStringLiteral("messageInput"));
        QObject *send = root->findChild<QObject *>(QStringLiteral("sendButton"));
        QVERIFY(banner);
        QVERIFY(messageList);
        QVERIFY(notice);
        QVERIFY(input);
        QVERIFY(send);

        // Ready: the banner is collapsed and invisible, history is shown, and the
        // security notice is hidden — the approved interface is unchanged.
        QVERIFY(!banner->property("visible").toBool());
        QCOMPARE(banner->property("height").toReal(), 0.0);
        QVERIFY(messageList->property("visible").toBool());
        QVERIFY(!notice->property("visible").toBool());

        // Locked: the message list is withheld, the notice replaces it, no rows
        // remain in the model, and sending is disabled even with composer text.
        controller.setSessionState(OpenChat::ChatController::SessionState::Locked);
        QVERIFY(input->setProperty("text", QStringLiteral("blocked while locked")));
        QCoreApplication::processEvents();
        QVERIFY(banner->property("visible").toBool());
        QVERIFY(banner->property("height").toReal() > 0.0);
        QVERIFY(!messageList->property("visible").toBool());
        QVERIFY(notice->property("visible").toBool());
        QCOMPARE(controller.messages()->rowCount(), 0);
        QCOMPARE(send->property("enabled").toBool(), false);

        // Returning to Ready restores the interface and the composer draft is intact.
        controller.setSessionState(OpenChat::ChatController::SessionState::Ready);
        QCoreApplication::processEvents();
        QVERIFY(!banner->property("visible").toBool());
        QCOMPARE(banner->property("height").toReal(), 0.0);
        QVERIFY(messageList->property("visible").toBool());
        QVERIFY(!notice->property("visible").toBool());
        QCOMPARE(controller.composerText(), QStringLiteral("blocked while locked"));
        QCOMPARE(send->property("enabled").toBool(), true);
    }

    void bubbleWidthFollowsContentWithinLimits()
    {
        QQmlEngine engine;
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        QQmlComponent component(&engine);
        component.loadFromModule("OpenChat", "MessageDelegate");
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));

        const auto bubbleWidth = [&component](const QString &body) {
            QScopedPointer<QObject> delegate(component.createWithInitialProperties(
                {{QStringLiteral("direction"), 0},
                 {QStringLiteral("body"), body},
                 {QStringLiteral("timestamp"), QStringLiteral("10:15 AM")},
                 {QStringLiteral("kind"), 0},
                 {QStringLiteral("dateLabel"), QStringLiteral("May 24, 2010")},
                 {QStringLiteral("showDateDivider"), false},
                 {QStringLiteral("width"), 540}}));
            return delegate ? delegate->property("bubbleWidth").toDouble() : -1.0;
        };

        const qreal shortWidth = bubbleWidth(QStringLiteral("Hi"));
        const qreal referenceWidth = bubbleWidth(QStringLiteral("Hey Daniel!"));
        const qreal wrappedWidth = bubbleWidth(
            QStringLiteral("Pretty good, just working on some stuff. You?"));
        QVERIFY(shortWidth >= 70.0);
        QVERIFY(shortWidth < 100.0);
        QVERIFY(shortWidth < referenceWidth);
        QVERIFY(referenceWidth < wrappedWidth);
        QVERIFY(wrappedWidth <= 360.0);
        QVERIFY(wrappedWidth <= 540.0 * 0.68);
    }

    void onboardingScreensDriveController()
    {
        // An async Starter that reports success with a fixed code, so the screen
        // surfaces exactly what account creation produced, independent of any real
        // ProfileSession. The controller pointer is bound after construction; the
        // Starter is only invoked later, when the Create button is clicked.
        OpenChat::OnboardingController *controllerPtr = nullptr;
        OpenChat::OnboardingController controller(
            [&controllerPtr](const QString &, const QString &) {
                controllerPtr->onCreationSucceeded(QStringLiteral("TEST-CODE-1234-5678"));
            });
        controllerPtr = &controller;

        QQmlEngine engine;
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        QQmlComponent component(&engine);
        component.loadFromModule("OpenChat", "Onboarding");
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));

        QScopedPointer<QObject> root(component.createWithInitialProperties(
            {{QStringLiteral("controller"), QVariant::fromValue(&controller)}}));
        QVERIFY(root);

        QObject *createView =
            root->findChild<QObject *>(QStringLiteral("onboardingCreateView"));
        QObject *recoveryView =
            root->findChild<QObject *>(QStringLiteral("onboardingRecoveryView"));
        QObject *displayNameField =
            root->findChild<QObject *>(QStringLiteral("displayNameField"));
        QObject *handleField =
            root->findChild<QObject *>(QStringLiteral("handleField"));
        QObject *createButton =
            root->findChild<QObject *>(QStringLiteral("createButton"));
        QObject *savedButton =
            root->findChild<QObject *>(QStringLiteral("savedButton"));
        QObject *recoveryCodeText =
            root->findChild<QObject *>(QStringLiteral("recoveryCodeText"));
        QVERIFY(createView);
        QVERIFY(recoveryView);
        QVERIFY(displayNameField);
        QVERIFY(handleField);
        QVERIFY(createButton);
        QVERIFY(savedButton);
        QVERIFY(recoveryCodeText);

        // Create is shown first; the button is disabled until both fields are set.
        QVERIFY(createView->property("visible").toBool());
        QVERIFY(!recoveryView->property("visible").toBool());
        QCOMPARE(createButton->property("enabled").toBool(), false);

        QVERIFY(displayNameField->setProperty("text", QStringLiteral("Ada Lovelace")));
        QCoreApplication::processEvents();
        QCOMPARE(createButton->property("enabled").toBool(), false);
        QVERIFY(handleField->setProperty("text", QStringLiteral("ada")));
        QCoreApplication::processEvents();
        QCOMPARE(controller.displayName(), QStringLiteral("Ada Lovelace"));
        QCOMPARE(controller.handle(), QStringLiteral("ada"));
        QCOMPARE(createButton->property("enabled").toBool(), true);

        QSignalSpy completedSpy(&controller, &OpenChat::OnboardingController::completed);

        // Create advances to the recovery view, which reveals the returned code.
        QVERIFY(QMetaObject::invokeMethod(createButton, "clicked"));
        QCoreApplication::processEvents();
        QCOMPARE(controller.step(), OpenChat::OnboardingController::Step::Recovery);
        QVERIFY(!createView->property("visible").toBool());
        QVERIFY(recoveryView->property("visible").toBool());
        QCOMPARE(recoveryCodeText->property("text").toString(),
                 QStringLiteral("TEST-CODE-1234-5678"));

        // Confirming the code completes onboarding exactly once.
        QVERIFY(QMetaObject::invokeMethod(savedButton, "clicked"));
        QCoreApplication::processEvents();
        QCOMPARE(controller.step(), OpenChat::OnboardingController::Step::Done);
        QCOMPARE(completedSpy.count(), 1);
    }

    void unknownAvatarUsesNeutralFallback()
    {
        QQmlEngine engine;
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        QQmlComponent component(&engine);
        component.loadFromModule("OpenChat", "Avatar");
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));

        QScopedPointer<QObject> avatar(component.createWithInitialProperties(
            {{QStringLiteral("avatarKey"), QStringLiteral("unknown")},
             {QStringLiteral("width"), 44},
             {QStringLiteral("height"), 44}}));
        QVERIFY(avatar);
        QObject *fallback =
            avatar->findChild<QObject *>(QStringLiteral("neutralAvatarFallback"));
        QVERIFY(fallback);
        QVERIFY(fallback->property("visible").toBool());
    }

    void avatarUsesRoundedArtworkMask()
    {
        QQmlEngine engine;
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        QQmlComponent component(&engine);
        component.loadFromModule("OpenChat", "Avatar");
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));

        QScopedPointer<QObject> avatar(component.createWithInitialProperties(
            {{QStringLiteral("avatarKey"), QStringLiteral("landscape")},
             {QStringLiteral("width"), 44},
             {QStringLiteral("height"), 44}}));
        QVERIFY(avatar);
        QVERIFY(avatar->property("usesRoundedArtworkMask").toBool());
        QObject *artwork = avatar->findChild<QObject *>(QStringLiteral("roundedAvatarArtwork"));
        QVERIFY(artwork);
        QCOMPARE(artwork->property("cornerRadius").toReal(), 5.0);
    }

    void contactSurfaceRendersWithController()
    {
        // A preview ContactController seeded like the --add-contact sub-mode: one
        // inbound request and a ready invite, injected alongside the chat controller.
        OpenChat::ChatController chatController;
        OpenChat::ContactController contactController;
        contactController.enableForPreview();
        contactController.addMockRequest(QStringLiteral("New contact request"),
                                         QStringLiteral("ID abcdef0123"));
        contactController.setMockInvite(QStringLiteral("OPENCHAT-INV-TEST-0001"));

        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{QStringLiteral("chatController"), QVariant::fromValue(&chatController)},
             {QStringLiteral("contactController"), QVariant::fromValue(&contactController)}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");

        QCOMPARE(engine.rootObjects().size(), 1);
        QObject *root = engine.rootObjects().constFirst();

        // The requests panel supersedes the favorites "Requests" category, and the
        // add affordance is present.
        QObject *requestsPanel =
            root->findChild<QObject *>(QStringLiteral("requestsPanel"));
        QObject *favoritesCategory =
            root->findChild<QObject *>(QStringLiteral("favoritesCategory"));
        QObject *addContactButton =
            root->findChild<QObject *>(QStringLiteral("addContactButton"));
        QObject *noRequests = root->findChild<QObject *>(QStringLiteral("noRequests"));
        QVERIFY(requestsPanel);
        QVERIFY(favoritesCategory);
        QVERIFY(addContactButton);
        QVERIFY(noRequests);
        QVERIFY(requestsPanel->property("visible").toBool());
        QVERIFY(!favoritesCategory->property("visible").toBool());
        QVERIFY(!noRequests->property("visible").toBool());

        // The seeded request's delegate is keyed by its requestId hex, and each of
        // its accept / decline / block buttons targets that same id.
        OpenChat::RequestListModel *model = contactController.requests();
        QVERIFY(model);
        QCOMPARE(model->count(), 1);
        const QString requestId =
            model->data(model->index(0), OpenChat::RequestListModel::IdRole).toString();
        QVERIFY(!requestId.isEmpty());
        // Repeater delegates are visual children of the panel but are not in its
        // QObject child tree, so they are located through the visual tree.
        auto *panelItem = qobject_cast<QQuickItem *>(requestsPanel);
        QVERIFY(panelItem);
        QVERIFY(findVisualItem(panelItem, QStringLiteral("requestRow_") + requestId));
        QVERIFY(findVisualItem(panelItem, QStringLiteral("requestAccept_") + requestId));
        QVERIFY(findVisualItem(panelItem, QStringLiteral("requestDecline_") + requestId));
        QVERIFY(findVisualItem(panelItem, QStringLiteral("requestBlock_") + requestId));

        // The dialog is present but hidden until the controller opens it.
        QObject *dialog = root->findChild<QObject *>(QStringLiteral("addContactDialog"));
        QVERIFY(dialog);
        QVERIFY(!dialog->property("visible").toBool());

        contactController.openDialog();
        QCoreApplication::processEvents();
        QVERIFY(dialog->property("visible").toBool());

        // Every field, action, and the invite/status surfaces are reachable, and the
        // seeded invite is surfaced read-only in the invite box.
        QObject *handleField = root->findChild<QObject *>(QStringLiteral("addHandleField"));
        QObject *inviteField = root->findChild<QObject *>(QStringLiteral("redeemInviteField"));
        QObject *createInviteButton =
            root->findChild<QObject *>(QStringLiteral("createInviteButton"));
        QObject *myInviteText = root->findChild<QObject *>(QStringLiteral("myInviteText"));
        QObject *addContactStatus =
            root->findChild<QObject *>(QStringLiteral("addContactStatus"));
        QVERIFY(handleField);
        QVERIFY(inviteField);
        QVERIFY(createInviteButton);
        QVERIFY(myInviteText);
        QVERIFY(addContactStatus);
        QVERIFY(contactController.inviteReady());
        QCOMPARE(myInviteText->property("text").toString(),
                 QStringLiteral("OPENCHAT-INV-TEST-0001"));
    }
};

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    qputenv("QT_QUICK_BACKEND", QByteArrayLiteral("software"));
    QGuiApplication application(argc, argv);
    qmlRegisterType<OpenChat::BubbleBackground>(
        "OpenChat.Native", 1, 0, "BubbleBackground");
    qmlRegisterType<OpenChat::AvatarArtwork>(
        "OpenChat.Native", 1, 0, "AvatarArtwork");
    qmlRegisterUncreatableType<OpenChat::ChatController>(
        "OpenChat.Native", 1, 0, "ChatController",
        QStringLiteral("ChatController is provided by the application"));
    qmlRegisterUncreatableType<OpenChat::OnboardingController>(
        "OpenChat.Native", 1, 0, "OnboardingController",
        QStringLiteral("OnboardingController is provided by the application"));
    qmlRegisterUncreatableType<OpenChat::ContactController>(
        "OpenChat.Native", 1, 0, "ContactController",
        QStringLiteral("ContactController is provided by the application"));
    QmlLoadTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_qmlload.moc"
