#include <QColor>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QFile>
#include <QImage>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QSettings>
#include <QUrl>
#include <QWindow>
#include <QtTest>
#include <QQuickWindow>

#include <optional>

#include "controllers/CallController.h"
#include "controllers/ChatController.h"
#include "controllers/ContactController.h"
#include "controllers/OnboardingController.h"
#include "models/RequestListModel.h"
#include "render/AvatarArtwork.h"
#include "app/AppearanceSettings.h"
#include "render/CallVideoItem.h"
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

// Types `text` (ASCII) into whatever has focus in `window`, one key at a time.
void typeText(QWindow *window, const QString &text)
{
    for (const QChar character : text)
        QTest::keyClick(window, character.toLatin1());
}

} // namespace

class QmlLoadTest final : public QObject
{
    Q_OBJECT

private slots:
    void init() { QSettings().setValue(QStringLiteral("Appearance/darkMode"), false); }

    void requiredStructure()
    {
        OpenChat::ChatController controller;
        controller.setLocalUserName(QStringLiteral("Developer"));
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
        QObject *localUserName =
            root->findChild<QObject *>(QStringLiteral("localUserName"));
        QVERIFY(localUserName);
        QCOMPARE(localUserName->property("text").toString(), QStringLiteral("Developer"));

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

    void bottomNavigationIconsStayAboveLabels()
    {
        OpenChat::ChatController controller;
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{QStringLiteral("chatController"), QVariant::fromValue(&controller)}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");
        QCOMPARE(engine.rootObjects().size(), 1);
        auto *window = qobject_cast<QWindow *>(engine.rootObjects().constFirst());
        QVERIFY(window);

        for (const QSize size : {QSize(720, 560), QSize(920, 680), QSize(1440, 900)}) {
            window->resize(size);
            QCoreApplication::processEvents();
            QRectF referenceIcon;
            qreal referenceLabelY = 0;
            qreal referenceTabHeight = 0;
            for (const QString &name : {QStringLiteral("call"), QStringLiteral("chat"),
                                        QStringLiteral("settings")}) {
                auto *icon = window->findChild<QQuickItem *>(name + QStringLiteral("Icon"));
                auto *label = window->findChild<QQuickItem *>(name + QStringLiteral("TabLabel"));
                auto *tab = window->findChild<QQuickItem *>(name + QStringLiteral("Tab"));
                QVERIFY(icon);
                QVERIFY(label);
                QVERIFY(tab);
                QVERIFY(icon->height() > 0);
                QVERIFY2(icon->y() + icon->height() < label->y(),
                         qPrintable(name + QStringLiteral(" icon overlaps its label")));
                const QRectF iconRect(icon->x(), icon->y(), icon->width(), icon->height());
                if (name == QStringLiteral("call")) {
                    referenceIcon = iconRect;
                    referenceLabelY = label->y();
                    referenceTabHeight = tab->height();
                } else {
                    QCOMPARE(iconRect, referenceIcon);
                    QCOMPARE(label->y(), referenceLabelY);
                    QCOMPARE(tab->height(), referenceTabHeight);
                }
            }
        }
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

    void darkModeSwitchUpdatesTheAppAndRemembersTheChoice()
    {
        OpenChat::ChatController chats;
        chats.setLocalUserName(QStringLiteral("Developer"));
        chats.setNavSection(OpenChat::ChatController::NavSection::Settings);
        chats.setCurrentSettingsCategory(5);
        OpenChat::ContactController contacts;
        contacts.enableForPreview();
        contacts.setMockInvite(QStringLiteral("OPENCHAT-INV-TEST-0001"));
        OpenChat::CallController calls;
        QQmlApplicationEngine engine;
        engine.setInitialProperties({{QStringLiteral("chatController"), QVariant::fromValue(&chats)},
                                     {QStringLiteral("contactController"), QVariant::fromValue(&contacts)},
                                     {QStringLiteral("callController"), QVariant::fromValue(&calls)}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");
        QCOMPARE(engine.rootObjects().size(), 1);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *toggle = findVisualItem(window->contentItem(), QStringLiteral("darkModeSwitch"));
        QVERIFY(toggle && toggle->isVisible());
        auto *appearance = engine.singletonInstance<OpenChat::AppearanceSettings *>(
            "OpenChat.Native", "AppearanceSettings");
        auto *theme = engine.singletonInstance<QObject *>("OpenChat", "Theme");
        QVERIFY(appearance && theme);
        QVERIFY(!appearance->darkMode());
        const QColor lightBackground = window->color();
        const QPoint switchCentre = toggle->mapToScene(QPointF(toggle->width() / 2,
                                                               toggle->height() / 2)).toPoint();
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, switchCentre);
        QTRY_VERIFY(appearance->darkMode());
        QVERIFY(toggle->property("checked").toBool());
        QVERIFY(window->color().lightnessF() < 0.2);
        for (const char *role : {"sidebarTop", "fieldBackground", "incomingTop", "outgoingTop",
                                 "callBackdropTop", "tooltipBottom", "panelBackground"})
            QVERIFY2(theme->property(role).value<QColor>().lightnessF() < 0.35, role);
        QVERIFY(theme->property("textPrimary").value<QColor>().lightnessF() > 0.8);
        QVERIFY(QSettings().value(QStringLiteral("Appearance/darkMode")).toBool());
        // A new QML engine, as used when the application opens its next window,
        // loads the saved preference without relying on this window's state.
        {
            QQmlEngine nextEngine;
            nextEngine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
            QQmlComponent component(&nextEngine);
            component.setData("import QtQuick; import OpenChat; QtObject { property bool dark: Theme.darkMode }", QUrl());
            std::unique_ptr<QObject> restored(component.create());
            QVERIFY2(restored, qPrintable(component.errorString()));
            QVERIFY(restored->property("dark").toBool());
        }
        const QString captureDir = qEnvironmentVariable("OPENCHAT_DARK_CAPTURE_DIR");
        const auto capture = [&](const QString &name) {
            QTest::qWait(80);
            if (!captureDir.isEmpty())
                return window->grabWindow().save(captureDir + QLatin1Char('/') + name + QStringLiteral(".png"));
            return true;
        };
        QVERIFY(capture(QStringLiteral("settings")));
        chats.setNavSection(OpenChat::ChatController::NavSection::Chat);
        QVERIFY(capture(QStringLiteral("chat")));
        contacts.openDialog();
        QVERIFY(capture(QStringLiteral("add-contact")));
        contacts.closeDialog();
        contacts.setMockSafetyNumber(QStringLiteral("12345 67890 24680 13579 11223 44556 77889 90011"), false,
                                    QStringLiteral("Jessica"));
        contacts.openSafetyNumberPreview();
        QVERIFY(capture(QStringLiteral("verification")));
        contacts.closeSafetyNumber();
        calls.enableForPreview(OpenChat::CallState::Active, QStringLiteral("Jessica"),
                               QStringLiteral("jessica"), true, false);
        QVERIFY(capture(QStringLiteral("call")));
        calls.enableForPreview(OpenChat::CallState::Idle, QString(), QString(), false, false);
        // Onboarding uses the same singleton and therefore the same saved theme.
        OpenChat::OnboardingController onboarding;
        QQmlComponent onboardingComponent(&engine, QUrl::fromLocalFile(
            QStringLiteral(OPENCHAT_SOURCE_DIR "/qml/OpenChat/Onboarding.qml")));
        std::unique_ptr<QObject> screen(onboardingComponent.createWithInitialProperties(
            {{QStringLiteral("controller"), QVariant::fromValue(&onboarding)}}));
        auto *onboardingItem = qobject_cast<QQuickItem *>(screen.get());
        QVERIFY2(onboardingItem, qPrintable(onboardingComponent.errorString()));
        onboardingItem->setParentItem(window->contentItem());
        onboardingItem->setSize(window->size());
        QVERIFY(capture(QStringLiteral("onboarding")));
        screen.reset();
        chats.setNavSection(OpenChat::ChatController::NavSection::Settings);
        toggle->forceActiveFocus();
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_VERIFY(!appearance->darkMode());
        QCOMPARE(window->color(), lightBackground);
        QVERIFY(!QSettings().value(QStringLiteral("Appearance/darkMode")).toBool());
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

    void failedMessageShowsRetryBelowBubble()
    {
        QQmlEngine engine;
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        QQmlComponent component(&engine);
        component.loadFromModule("OpenChat", "MessageDelegate");
        QScopedPointer<QObject> item(component.createWithInitialProperties({
            {"direction", 1}, {"deliveryState", 6}, {"body", "hello"},
            {"timestamp", "10:15 AM"}, {"kind", 0}, {"dateLabel", ""},
            {"showDateDivider", false}, {"senderName", ""}, {"width", 540}}));
        QVERIFY2(item, qPrintable(component.errorString()));
        auto *retry = item->findChild<QObject *>("messageRetry");
        QVERIFY(retry);
        QVERIFY(retry->property("visible").toBool());
        QCOMPARE(retry->property("color").value<QColor>(), QColor("#c62828"));
        QVERIFY(retry->property("y").toDouble() >= item->property("bubbleHeight").toDouble());
        item->setProperty("deliveryState", 3);
        QVERIFY(!retry->property("visible").toBool());
    }

    void messageTimestampFormatting()
    {
        QQmlEngine engine;
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        QQmlComponent component(&engine);
        component.loadFromModule("OpenChat", "MessageDelegate");
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));

        QScopedPointer<QObject> delegate(component.createWithInitialProperties(
            {{QStringLiteral("deliveryState"), 0},
             {QStringLiteral("direction"), 0},
             {QStringLiteral("body"), QStringLiteral("Hello")},
             {QStringLiteral("timestamp"), QStringLiteral("10:15 AM")},
             {QStringLiteral("kind"), 0},
             {QStringLiteral("dateLabel"), QStringLiteral("May 24, 2010")},
             {QStringLiteral("showDateDivider"), false},
             {QStringLiteral("senderName"), QString()},
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
                {{QStringLiteral("deliveryState"), 0},
             {QStringLiteral("direction"), 0},
                 {QStringLiteral("body"), body},
                 {QStringLiteral("timestamp"), QStringLiteral("10:15 AM")},
                 {QStringLiteral("kind"), 0},
                 {QStringLiteral("dateLabel"), QStringLiteral("May 24, 2010")},
                 {QStringLiteral("showDateDivider"), false},
             {QStringLiteral("senderName"), QString()},
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

    void friendStatusBubbleFollowsAvatarHover()
    {
        OpenChat::ChatController controller;
        QVector<OpenChat::Contact> contacts;
        const QString status = QStringLiteral("Taking a little break — back after coffee ☕");
        for (int i = 0; i < controller.contacts()->rowCount(); ++i) {
            auto contact = *controller.contacts()->contactAt(i);
            if (contact.id == QStringLiteral("alex"))
                contact.statusText = status;
            contacts.append(contact);
        }
        controller.contacts()->setContacts(contacts);
        QQmlApplicationEngine engine;
        engine.setInitialProperties({{QStringLiteral("chatController"), QVariant::fromValue(&controller)}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");
        QCOMPARE(engine.rootObjects().size(), 1);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *category = window->findChild<QQuickItem *>(QStringLiteral("contactsCategory"));
        QVERIFY(category);
        auto *row = findVisualItem(category, QStringLiteral("contactRow_alex"));
        QVERIFY(row);
        auto *avatar = row->findChild<QQuickItem *>(QStringLiteral("contactAvatar"));
        auto *bubble = row->findChild<QQuickItem *>(QStringLiteral("contactStatusBubble_alex"));
        QVERIFY(avatar && bubble);
        auto *text = bubble->findChild<QQuickItem *>(QStringLiteral("contactStatusBubbleText"));
        QVERIFY(text);
        const QPoint avatarPoint = avatar->mapToScene(QPointF(avatar->width() / 2,
                                                              avatar->height() / 2)).toPoint();
        const QPoint namePoint = row->mapToScene(QPointF(row->width() - 30, 14)).toPoint();
        QTest::mouseMove(window, namePoint);
        QVERIFY(!bubble->isVisible());
        QTest::mouseMove(window, avatarPoint);
        QVERIFY(!bubble->property("shown").toBool()); // passing over does not flash a tooltip
        QTRY_COMPARE(bubble->opacity(), 1.0);
        QCOMPARE(text->property("text").toString(), status);
        QCOMPARE(text->property("textFormat").toInt(), 0); // PlainText
        QVERIFY(bubble->parentItem() == window->contentItem());
        QVERIFY(bubble->x() > avatarPoint.x());
        QVERIFY(bubble->y() >= 0);
        QVERIFY(bubble->x() + bubble->width() <= window->width());
        QVERIFY(bubble->y() + bubble->height() <= window->height());
        QVERIFY(bubble->findChild<QObject *>(QStringLiteral("statusBubbleSurface")));
        const QString capture = qEnvironmentVariable("OPENCHAT_STATUS_BUBBLE_CAPTURE");
        if (!capture.isEmpty())
            QVERIFY(window->grabWindow().save(capture));
        // The subtitle remains bound live, including text that resembles HTML.
        QVERIFY(row->setProperty("statusText", QStringLiteral("<b>Back soon</b>")));
        QCOMPARE(text->property("text").toString(), QStringLiteral("<b>Back soon</b>"));
        QTest::mouseMove(window, namePoint);
        QTRY_VERIFY(!bubble->isVisible());
        QTest::mouseMove(window, avatarPoint);
        QTRY_COMPARE(bubble->opacity(), 1.0);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, avatarPoint);
        QCOMPARE(controller.currentContactId(), QStringLiteral("alex"));
        QTRY_VERIFY(!bubble->isVisible());
        QTest::mouseMove(window, namePoint);
        QVERIFY(row->setProperty("statusText", QString()));
        QTest::mouseMove(window, avatarPoint);
        QVERIFY(!row->property("avatarHovered").toBool());
    }

    void localProfileEditorsAreWiredToTheController()
    {
        OpenChat::ChatController controller;
        controller.setLocalUserName(QStringLiteral("Developer"));
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{QStringLiteral("chatController"), QVariant::fromValue(&controller)}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");
        QCOMPARE(engine.rootObjects().size(), 1);
        QObject *root = engine.rootObjects().constFirst();
        auto *window = qobject_cast<QQuickWindow *>(root);
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));

        // Everything is present but dormant: no shades, no menu, no editor.
        auto *avatarButton = findVisualItem(window->contentItem(), QStringLiteral("localAvatarButton"));
        auto *avatarShade = findVisualItem(window->contentItem(), QStringLiteral("localAvatarHoverShade"));
        auto *statusEditor = findVisualItem(window->contentItem(), QStringLiteral("localStatusEditor"));
        auto *statusShade = findVisualItem(window->contentItem(), QStringLiteral("localStatusHoverShade"));
        auto *statusText = findVisualItem(window->contentItem(), QStringLiteral("localStatusText"));
        auto *statusInput = findVisualItem(window->contentItem(), QStringLiteral("localStatusInput"));
        auto *presenceButton = findVisualItem(window->contentItem(), QStringLiteral("localPresenceButton"));
        auto *presenceShade = findVisualItem(window->contentItem(), QStringLiteral("localPresenceHoverShade"));
        auto *presenceMenu = findVisualItem(window->contentItem(), QStringLiteral("localPresenceMenu"));
        auto *notice = findVisualItem(window->contentItem(), QStringLiteral("profileNotice"));
        QVERIFY(avatarButton && avatarShade && statusEditor && statusShade && statusText
                && statusInput && presenceButton && presenceShade && presenceMenu && notice);
        QVERIFY(root->findChild<QObject *>(QStringLiteral("localAvatarFileDialog")));
        QVERIFY(!avatarShade->isVisible());
        QVERIFY(!statusShade->isVisible());
        QVERIFY(!statusInput->isVisible());
        QVERIFY(!presenceShade->isVisible());
        QVERIFY(!presenceMenu->isVisible());
        QVERIFY(!notice->isVisible());
        QCOMPARE(statusText->property("text").toString(), QStringLiteral("Available"));

        // Hovering the picture darkens it and shows the plus; hovering the
        // status line tints the field; hovering the bead darkens it.
        const auto centre = [](QQuickItem *item) {
            return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
        };
        QTest::mouseMove(window, centre(avatarButton));
        QTRY_VERIFY(avatarShade->isVisible());
        QTest::mouseMove(window, centre(statusEditor));
        QTRY_VERIFY(statusShade->isVisible());
        QTRY_VERIFY(!avatarShade->isVisible());
        QTest::mouseMove(window, centre(presenceButton));
        QTRY_VERIFY(presenceShade->isVisible());
        QTRY_VERIFY(!statusShade->isVisible());

        // Clicking the bead opens the picker with every presence; choosing one
        // applies it, closes the picker, and the bead and line follow.
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centre(presenceButton));
        QTRY_VERIFY(presenceMenu->isVisible());
        for (const int value : {0, 1, 2, 3}) {
            QVERIFY2(findVisualItem(window->contentItem(),
                                    QStringLiteral("presenceOption_%1").arg(value)),
                     qPrintable(QStringLiteral("presence option %1").arg(value)));
        }
        auto *busy = findVisualItem(window->contentItem(), QStringLiteral("presenceOption_3"));
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centre(busy));
        QTRY_COMPARE(controller.localPresence(), 3);
        QTRY_VERIFY(!presenceMenu->isVisible());
        QCOMPARE(statusText->property("text").toString(), QStringLiteral("Busy"));
        auto *sidebar = root->findChild<QObject *>(QStringLiteral("contactSidebar"));
        QVERIFY(sidebar);
        // Clicking away closes an open picker without choosing.
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centre(presenceButton));
        QTRY_VERIFY(presenceMenu->isVisible());
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, QPoint(window->width() - 40, window->height() - 40));
        QTRY_VERIFY(!presenceMenu->isVisible());
        QCOMPARE(controller.localPresence(), 3);

        // Clicking the status line turns it into an editor holding the
        // current text; Enter commits the new text to the controller.
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centre(statusEditor));
        QTRY_VERIFY(statusInput->isVisible());
        QVERIFY(!statusText->isVisible());
        QCOMPARE(statusInput->property("text").toString(), QStringLiteral("Busy"));
        QVERIFY(statusInput->hasActiveFocus());
        typeText(window, QStringLiteral("Heads down until 4"));
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_COMPARE(controller.localStatusText(), QStringLiteral("Heads down until 4"));
        QTRY_VERIFY(!statusInput->isVisible());
        QCOMPARE(statusText->property("text").toString(), QStringLiteral("Heads down until 4"));

        // Escape abandons an edit.
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centre(statusEditor));
        QTRY_VERIFY(statusInput->isVisible());
        QCOMPARE(statusInput->property("text").toString(), QStringLiteral("Heads down until 4"));
        typeText(window, QStringLiteral("nope"));
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!statusInput->isVisible());
        QCOMPARE(controller.localStatusText(), QStringLiteral("Heads down until 4"));

        // Keeping the presence name as-is leaves the status unset.
        controller.setLocalStatusText(QString());
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centre(statusEditor));
        QTRY_VERIFY(statusInput->isVisible());
        QCOMPARE(statusInput->property("text").toString(), QStringLiteral("Busy"));
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_VERIFY(!statusInput->isVisible());
        QVERIFY(controller.localStatusText().isEmpty());

        // A refused picture surfaces its reason under the status line, and the
        // sidebar avatar follows the controller's picture key.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString junk = dir.filePath(QStringLiteral("junk.png"));
        {
            QFile file(junk);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("not a picture");
        }
        QVERIFY(!controller.setLocalAvatarFromFile(QUrl::fromLocalFile(junk)));
        QTRY_VERIFY(notice->isVisible());
        controller.clearProfileNotice();
        QTRY_VERIFY(!notice->isVisible());
        QImage photo(80, 80, QImage::Format_RGB32);
        photo.fill(QColor("#35618f"));
        const QString good = dir.filePath(QStringLiteral("me.png"));
        QVERIFY(photo.save(good, "PNG"));
        QVERIFY(controller.setLocalAvatarFromFile(QUrl::fromLocalFile(good)));
        auto *avatar = root->findChild<QObject *>(QStringLiteral("localUserAvatar"));
        QVERIFY(avatar);
        QTRY_COMPARE(avatar->property("avatarKey").toString(), controller.localAvatarKey());
        QVERIFY(controller.localAvatarKey().startsWith(QStringLiteral("blob:")));
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

        // The friend-requests category supersedes the favorites "Requests"
        // category while a request is pending, and the add affordance is present.
        QObject *requestsPanel =
            root->findChild<QObject *>(QStringLiteral("requestsPanel"));
        QObject *favoritesCategory =
            root->findChild<QObject *>(QStringLiteral("favoritesCategory"));
        QObject *addContactButton =
            root->findChild<QObject *>(QStringLiteral("addContactButton"));
        QVERIFY(requestsPanel);
        QVERIFY(favoritesCategory);
        QVERIFY(addContactButton);
        QVERIFY(requestsPanel->property("visible").toBool());
        QTRY_VERIFY(requestsPanel->property("height").toReal() > 40.0); // header + one row
        QVERIFY(!favoritesCategory->property("visible").toBool());
        QVERIFY(addContactButton->property("visible").toBool());

        // The seeded request's delegate is keyed by its requestId hex, and both of
        // its accept / decline buttons target that same id.
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

    void friendRequestsCategoryHidesWithoutRequests()
    {
        OpenChat::ChatController chatController;
        OpenChat::ContactController contactController;
        contactController.enableForPreview();

        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{QStringLiteral("chatController"), QVariant::fromValue(&chatController)},
             {QStringLiteral("contactController"), QVariant::fromValue(&contactController)}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");
        QCOMPARE(engine.rootObjects().size(), 1);
        QObject *root = engine.rootObjects().constFirst();

        QObject *requestsPanel = root->findChild<QObject *>(QStringLiteral("requestsPanel"));
        QVERIFY(requestsPanel);
        QVERIFY(!requestsPanel->property("visible").toBool());
        QCOMPARE(requestsPanel->property("height").toReal(), 0.0);

        // A request arriving reveals the category; resolving it hides it again.
        contactController.addMockRequest(QStringLiteral("@dave"), QStringLiteral("wants to chat"));
        QCoreApplication::processEvents();
        QVERIFY(requestsPanel->property("visible").toBool());
        OpenChat::RequestListModel *model = contactController.requests();
        const QString requestId =
            model->data(model->index(0), OpenChat::RequestListModel::IdRole).toString();
        contactController.decline(requestId);
        QCoreApplication::processEvents();
        QVERIFY(!requestsPanel->property("visible").toBool());
    }

    void searchAndFindRowSendsFriendRequest()
    {
        OpenChat::ChatController chatController;
        OpenChat::ContactController contactController;
        contactController.enableForPreview();
        contactController.setMockDirectory({QStringLiteral("alice")});

        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{QStringLiteral("chatController"), QVariant::fromValue(&chatController)},
             {QStringLiteral("contactController"), QVariant::fromValue(&contactController)}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");
        QCOMPARE(engine.rootObjects().size(), 1);
        QObject *root = engine.rootObjects().constFirst();

        QObject *result = root->findChild<QObject *>(QStringLiteral("directoryResult"));
        QObject *name = root->findChild<QObject *>(QStringLiteral("directoryResultName"));
        QObject *subtitle =
            root->findChild<QObject *>(QStringLiteral("directoryResultSubtitle"));
        QObject *button = root->findChild<QObject *>(QStringLiteral("sendRequestButton"));
        QVERIFY(result);
        QVERIFY(name);
        QVERIFY(subtitle);
        QVERIFY(button);
        QVERIFY(!result->property("visible").toBool());
        QCOMPARE(result->property("height").toReal(), 0.0);

        // Typing a username that exists shows them with the grey add affordance.
        chatController.setSearchQuery(QStringLiteral("alice"));
        contactController.lookup(QStringLiteral("alice"));
        QCoreApplication::processEvents();
        QVERIFY(result->property("visible").toBool());
        QCOMPARE(name->property("text").toString(), QStringLiteral("@alice"));
        QTRY_COMPARE(contactController.lookupState(),
                     OpenChat::ContactController::LookupState::Found);
        QCoreApplication::processEvents();
        QVERIFY(button->property("visible").toBool());
        QVERIFY(!button->property("sent").toBool());
        QCOMPARE(subtitle->property("text").toString(), QStringLiteral("Send a friend request"));

        // Clicking it sends the request; the affordance turns into a sent mark.
        contactController.requestLookup();
        QCoreApplication::processEvents();
        QVERIFY(button->property("sent").toBool());
        QCOMPARE(subtitle->property("text").toString(), QStringLiteral("Request sent"));

        // Clearing the search hides the row again.
        chatController.setSearchQuery(QString());
        contactController.lookup(QString());
        QCoreApplication::processEvents();
        QVERIFY(!result->property("visible").toBool());
    }

    void safetyNumberDialogRendersWithController()
    {
        // A preview ContactController seeded like the --verify sub-mode: a preset
        // grouped safety number and a display label, injected alongside the chat
        // controller. The number is set before load; the dialog is revealed only
        // when the controller opens it, mirroring the add-contact overlay.
        const QString number =
            QStringLiteral("12345 67890 24680 13579 11223 44556 77889 90011 22334 45566 "
                           "77889 90011");
        OpenChat::ChatController chatController;
        OpenChat::ContactController contactController;
        contactController.enableForPreview();
        contactController.setMockSafetyNumber(number, /*verified*/ false,
                                              QStringLiteral("@ada"));

        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{QStringLiteral("chatController"), QVariant::fromValue(&chatController)},
             {QStringLiteral("contactController"), QVariant::fromValue(&contactController)}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");

        QCOMPARE(engine.rootObjects().size(), 1);
        QObject *root = engine.rootObjects().constFirst();

        // The dialog is present but hidden until the controller opens the surface.
        QObject *dialog = root->findChild<QObject *>(QStringLiteral("safetyNumberDialog"));
        QVERIFY(dialog);
        QVERIFY(!dialog->property("visible").toBool());

        contactController.openSafetyNumberPreview();
        QCoreApplication::processEvents();
        QVERIFY(dialog->property("visible").toBool());

        // Every key element is reachable and bound to the controller's surface.
        QObject *contactLabel =
            root->findChild<QObject *>(QStringLiteral("safetyNumberContactLabel"));
        QObject *numberText =
            root->findChild<QObject *>(QStringLiteral("safetyNumberText"));
        QObject *markVerifiedButton =
            root->findChild<QObject *>(QStringLiteral("markVerifiedButton"));
        QObject *closeButton =
            root->findChild<QObject *>(QStringLiteral("safetyNumberClose"));
        QVERIFY(contactLabel);
        QVERIFY(numberText);
        QVERIFY(markVerifiedButton);
        QVERIFY(closeButton);
        QCOMPARE(contactLabel->property("text").toString(), QStringLiteral("@ada"));
        QCOMPARE(numberText->property("text").toString(), number);
        QVERIFY(numberText->property("visible").toBool());
        // Not yet verified and a non-empty number: the verify action is enabled.
        QVERIFY(markVerifiedButton->property("enabled").toBool());

        // Marking verified reveals the badge and disables the button; the flip flows
        // from the controller through the bound surface.
        QObject *badge =
            root->findChild<QObject *>(QStringLiteral("safetyNumberVerifiedBadge"));
        QVERIFY(badge);
        QVERIFY(!badge->property("visible").toBool());
        contactController.markVerified();
        QCoreApplication::processEvents();
        QVERIFY(contactController.safetyNumberVerified());
        QVERIFY(badge->property("visible").toBool());
        QVERIFY(!markVerifiedButton->property("enabled").toBool());
    }

    void callScreenReplacesTheConversationHeader()
    {
        // The defining behaviour of the in-call surface: while a call is up, the
        // conversation header — the contact's name, picture and call buttons —
        // is gone, and one panel showing BOTH people takes its place. If the two
        // were ever visible together the contact would be pictured twice.
        OpenChat::ChatController chatController;
        chatController.setLocalUserName(QStringLiteral("Developer"));
        OpenChat::CallController callController;
        callController.setLocalIdentity(chatController.localUserName(),
                                        chatController.localAvatarKey());

        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{QStringLiteral("chatController"), QVariant::fromValue(&chatController)},
             {QStringLiteral("callController"), QVariant::fromValue(&callController)}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");

        QCOMPARE(engine.rootObjects().size(), 1);
        QObject *root = engine.rootObjects().constFirst();

        auto *conversationHeader =
            root->findChild<QQuickItem *>(QStringLiteral("conversationHeader"));
        QVERIFY(conversationHeader);
        // Out of a call nothing changes: the conversation header is what shows,
        // and the call surface is present but hidden.
        QVERIFY(conversationHeader->isVisible());
        auto *idleCallHeader = root->findChild<QQuickItem *>(QStringLiteral("callHeader"));
        QVERIFY(idleCallHeader);
        QVERIFY(!idleCallHeader->isVisible());

        callController.enableForPreview(OpenChat::CallState::Active,
                                        QStringLiteral("Jessica"),
                                        QStringLiteral("jessica"),
                                        /*remoteSpeaking=*/true, /*localSpeaking=*/false);
        QCoreApplication::processEvents();

        auto *callHeader = root->findChild<QQuickItem *>(QStringLiteral("callHeader"));
        QVERIFY(callHeader);
        QVERIFY(callHeader->isVisible());
        QVERIFY(!conversationHeader->isVisible());
        // The slot grows to make room for the two callers rather than cropping.
        auto *slot = root->findChild<QQuickItem *>(QStringLiteral("conversationHeaderSlot"));
        QVERIFY(slot);
        QVERIFY(slot->height() > conversationHeader->implicitHeight());

        // Both callers are shown, in one place, named.
        auto *local = root->findChild<QQuickItem *>(QStringLiteral("localParticipant"));
        auto *remote = root->findChild<QQuickItem *>(QStringLiteral("remoteParticipant"));
        QVERIFY(local);
        QVERIFY(remote);
        QVERIFY(local->isVisible());
        QVERIFY(remote->isVisible());
        QCOMPARE(local->property("name").toString(), QStringLiteral("Developer"));
        QCOMPARE(remote->property("name").toString(), QStringLiteral("Jessica"));
        QCOMPARE(remote->property("avatarKey").toString(), QStringLiteral("jessica"));
        // Side by side, so neither is subordinate to the other.
        const QPointF localScene = local->mapToScene(QPointF(0, 0));
        const QPointF remoteScene = remote->mapToScene(QPointF(0, 0));
        QVERIFY(localScene.x() < remoteScene.x());
        QCOMPARE(localScene.y(), remoteScene.y());
        QCOMPARE(local->width(), remote->width());
    }

    void videoFitsEachCameraAndReturnsToAvatars()
    {
        OpenChat::ChatController chats;
        OpenChat::CallController calls;
        calls.enableForPreview(OpenChat::CallState::Active, QStringLiteral("Jessica"),
                               QStringLiteral("jessica"), true, false);
        QQmlApplicationEngine engine;
        engine.setInitialProperties({{QStringLiteral("chatController"), QVariant::fromValue(&chats)},
                                     {QStringLiteral("callController"), QVariant::fromValue(&calls)}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");
        QCOMPARE(engine.rootObjects().size(), 1);
        auto *root = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(root);
        auto *local = root->findChild<QQuickItem *>(QStringLiteral("localParticipant"));
        auto *remote = root->findChild<QQuickItem *>(QStringLiteral("remoteParticipant"));
        auto *localVideo = local->findChild<QQuickItem *>(QStringLiteral("participantVideo"));
        auto *remoteVideo = remote->findChild<QQuickItem *>(QStringLiteral("participantVideo"));
        auto *slot = root->findChild<QQuickItem *>(QStringLiteral("conversationHeaderSlot"));
        auto *camera = root->findChild<QQuickItem *>(QStringLiteral("cameraCallButton"));
        QVERIFY(localVideo && remoteVideo && slot && camera);
        QVERIFY(camera->isVisible());
        QVERIFY(camera->property("cameraIcon").toBool());
        QImage wide(640, 360, QImage::Format_RGB32);
        QImage portrait(360, 640, QImage::Format_RGB32);
        wide.fill(Qt::blue);
        portrait.fill(Qt::green);
        for (const QSize windowSize : {QSize(720, 560), QSize(900, 680), QSize(1024, 768)}) {
            root->resize(windowSize);
            calls.setPreviewVideo(wide, QImage());
            QCoreApplication::processEvents();
            QVERIFY(localVideo->isVisible());
            QVERIFY(!remoteVideo->isVisible());
            QVERIFY(localVideo->width() > 74);
            QCOMPARE(remote->property("pictureWidth").toReal(), 74.0);
            QVERIFY(qAbs(localVideo->width() / localVideo->height() - 16.0 / 9.0) < 0.001);
            calls.setPreviewVideo(wide, portrait);
            QCoreApplication::processEvents();
            QVERIFY(remoteVideo->isVisible());
            QVERIFY(qAbs(remoteVideo->width() / remoteVideo->height() - 9.0 / 16.0) < 0.001);
            QVERIFY(qAbs(localVideo->width() / localVideo->height() - 16.0 / 9.0) < 0.001);
            QVERIFY(local->mapToItem(slot, QPointF()).x() >= 0);
            QVERIFY(remote->mapToItem(slot, QPointF(remote->width(), 0)).x() <= slot->width());
            QVERIFY(camera->mapToItem(slot, QPointF(0, camera->height())).y() <= slot->height());
        }
        calls.setPreviewVideo(QImage(), QImage());
        QCoreApplication::processEvents();
        QVERIFY(!localVideo->isVisible());
        QVERIFY(!remoteVideo->isVisible());
        QCOMPARE(local->property("pictureWidth").toReal(), 74.0);
        QTRY_COMPARE(slot->height(), 212.0);
    }

    void theTalkingCallerIsRingedInGreen()
    {
        OpenChat::ChatController chatController;
        OpenChat::CallController callController;
        callController.enableForPreview(OpenChat::CallState::Active, QStringLiteral("Jessica"),
                                        QStringLiteral("jessica"), /*remoteSpeaking=*/true,
                                        /*localSpeaking=*/false);

        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{QStringLiteral("chatController"), QVariant::fromValue(&chatController)},
             {QStringLiteral("callController"), QVariant::fromValue(&callController)}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");
        QCOMPARE(engine.rootObjects().size(), 1);
        QObject *root = engine.rootObjects().constFirst();

        auto *local = root->findChild<QQuickItem *>(QStringLiteral("localParticipant"));
        auto *remote = root->findChild<QQuickItem *>(QStringLiteral("remoteParticipant"));
        QVERIFY(local && remote);

        auto *localRing = local->findChild<QQuickItem *>(QStringLiteral("speakingRing"));
        auto *remoteRing = remote->findChild<QQuickItem *>(QStringLiteral("speakingRing"));
        auto *remoteGlow = remote->findChild<QQuickItem *>(QStringLiteral("speakingGlow"));
        auto *localGlow = local->findChild<QQuickItem *>(QStringLiteral("speakingGlow"));
        QVERIFY(localRing && remoteRing && remoteGlow && localGlow);

        const QColor speaking = remoteRing->property("border")
                                    .value<QObject *>()
                                    ->property("color")
                                    .value<QColor>();
        const QColor quiet =
            localRing->property("border").value<QObject *>()->property("color").value<QColor>();

        // The talker's ring is green; the listener's is not.
        QVERIFY2(speaking.greenF() > speaking.redF() && speaking.greenF() > speaking.blueF(),
                 "the speaking ring is not green");
        QVERIFY(speaking != quiet);
        // Green means green, not "slightly greener": it must be unmistakable.
        QVERIFY(speaking.greenF() - speaking.redF() > 0.3);
        // Only the talker glows.
        QVERIFY(remoteGlow->opacity() > 0.0);
        QCOMPARE(localGlow->opacity(), 0.0);
        // The ring surrounds the picture rather than covering it.
        auto *avatar = remote->findChild<QQuickItem *>(QStringLiteral("roundedAvatarArtwork"));
        QVERIFY(avatar);
        QVERIFY(remoteRing->width() > avatar->width());
        QVERIFY(remoteRing->height() > avatar->height());

        // Hand the floor to the other caller and the ring follows the voice.
        callController.enableForPreview(OpenChat::CallState::Active, QStringLiteral("Jessica"),
                                        QStringLiteral("jessica"), /*remoteSpeaking=*/false,
                                        /*localSpeaking=*/true);
        QCoreApplication::processEvents();
        QTRY_COMPARE(localRing->property("border")
                         .value<QObject *>()
                         ->property("color")
                         .value<QColor>(),
                     speaking);
        QTRY_VERIFY(localGlow->opacity() > 0.0);
    }

    void thePlusNextToTheNameMakesAndManagesAGroup()
    {
        OpenChat::ChatController controller;
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{QStringLiteral("chatController"), QVariant::fromValue(&controller)}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");
        QCOMPARE(engine.rootObjects().size(), 1);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));

        auto *plus = window->findChild<QQuickItem *>(QStringLiteral("addToGroupButton"));
        auto *picker = window->findChild<QQuickItem *>(QStringLiteral("groupMemberPicker"));
        auto *title = window->findChild<QQuickItem *>(QStringLiteral("conversationTitle"));
        auto *leave = window->findChild<QQuickItem *>(QStringLiteral("leaveGroupButton"));
        auto *editor = window->findChild<QQuickItem *>(QStringLiteral("groupTitleEditor"));
        auto *input = window->findChild<QQuickItem *>(QStringLiteral("groupTitleInput"));
        QVERIFY(plus && picker && title && leave && editor && input);
        // In a one-to-one chat: the plus is there, the picker is closed, no
        // leave button, and the name is not editable.
        QVERIFY(plus->isVisible());
        QVERIFY(!picker->isVisible());
        QVERIFY(!leave->isVisible());
        QCOMPARE(title->property("text").toString(), QStringLiteral("Michael"));
        QVERIFY(QMetaObject::invokeMethod(editor, "beginEditing"));
        QVERIFY(!editor->property("editing").toBool());

        // The plus opens the picker listing everyone else.
        const QPoint plusPoint = plus->mapToScene(QPointF(plus->width() / 2, plus->height() / 2)).toPoint();
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, plusPoint);
        QTRY_VERIFY(picker->isVisible());
        auto *sarah = findVisualItem(picker, QStringLiteral("groupCandidate_sarah"));
        QVERIFY(sarah);
        QVERIFY(!findVisualItem(picker, QStringLiteral("groupCandidate_michael")));
        QVERIFY(picker->mapToScene(QPointF(0, 0)).x() >= 0);
        QVERIFY(picker->mapToScene(QPointF(picker->width(), 0)).x() <= window->width());

        // Picking Sarah starts a group with Michael and Sarah, and opens it.
        const QPoint sarahPoint = sarah->mapToScene(QPointF(sarah->width() / 2, sarah->height() / 2)).toPoint();
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, sarahPoint);
        QTRY_VERIFY(controller.currentIsGroup());
        QVERIFY(!picker->isVisible());
        QCOMPARE(title->property("text").toString(), QStringLiteral("Michael, Sarah"));
        auto *subtitle = window->findChild<QQuickItem *>(QStringLiteral("conversationSubtitle"));
        QVERIFY(subtitle);
        QCOMPARE(subtitle->property("text").toString(), QStringLiteral("You, Michael, Sarah"));
        QVERIFY(leave->isVisible());
        // The group row in the sidebar has the group picture and no bead.
        auto *row = findVisualItem(window->contentItem(),
                                   QStringLiteral("contactRow_") + controller.currentContactId());
        QVERIFY(row);
        QVERIFY(row->property("isGroup").toBool());

        // The title is edited like the status line: type, Enter, done.
        QVERIFY(QMetaObject::invokeMethod(editor, "beginEditing"));
        QVERIFY(editor->property("editing").toBool());
        QVERIFY(input->setProperty("text", QStringLiteral("Weekend plans")));
        QVERIFY(QMetaObject::invokeMethod(editor, "commit"));
        QCOMPARE(controller.currentGroupTitle(), QStringLiteral("Weekend plans"));
        QCOMPARE(title->property("text").toString(), QStringLiteral("Weekend plans"));
        // Escape reverts.
        QVERIFY(QMetaObject::invokeMethod(editor, "beginEditing"));
        QVERIFY(input->setProperty("text", QStringLiteral("nope")));
        QVERIFY(QMetaObject::invokeMethod(editor, "cancel"));
        QCOMPARE(controller.currentGroupTitle(), QStringLiteral("Weekend plans"));

        // The plus in a group adds to it. It moved with the longer title.
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          plus->mapToScene(QPointF(plus->width() / 2, plus->height() / 2)).toPoint());
        QTRY_VERIFY(picker->isVisible());
        auto *alex = findVisualItem(picker, QStringLiteral("groupCandidate_alex"));
        QVERIFY(alex);
        QVERIFY(!findVisualItem(picker, QStringLiteral("groupCandidate_sarah")));
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          alex->mapToScene(QPointF(alex->width() / 2, alex->height() / 2)).toPoint());
        QTRY_COMPARE(controller.currentGroupMemberCount(), 4);

        // Leaving returns to a person.
        QVERIFY(QMetaObject::invokeMethod(leave, "clicked"));
        QTRY_VERIFY(!controller.currentIsGroup());
        QVERIFY(!leave->isVisible());
    }

    void theGroupCallScreenShowsEveryMemberAndWhatTheyAreDoing()
    {
        OpenChat::ChatController chatController;
        chatController.setLocalUserName(QStringLiteral("Developer"));
        OpenChat::CallController callController;
        callController.setLocalIdentity(chatController.localUserName(), chatController.localAvatarKey());
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{QStringLiteral("chatController"), QVariant::fromValue(&chatController)},
             {QStringLiteral("callController"), QVariant::fromValue(&callController)}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");
        QCOMPARE(engine.rootObjects().size(), 1);
        QObject *root = engine.rootObjects().constFirst();

        OpenChat::CallParticipantRow jessica{QStringLiteral("d1"), QStringLiteral("Jessica"),
                                             QStringLiteral("jessica"), QString(), true, false, true, 0.4};
        OpenChat::CallParticipantRow michael{QStringLiteral("d2"), QStringLiteral("Michael"),
                                             QStringLiteral("michael"), QStringLiteral("Ringing…"),
                                             false, true, false, 0.0};
        OpenChat::CallParticipantRow ryan{QStringLiteral("d3"), QStringLiteral("Ryan"),
                                          QStringLiteral("ryan"), QStringLiteral("Declined"), false,
                                          false, false, 0.0};
        callController.enableForGroupPreview(OpenChat::CallState::Ringing, QStringLiteral("Weekend plans"),
                                             {jessica, michael, ryan});
        QCoreApplication::processEvents();

        auto *callHeader = root->findChild<QQuickItem *>(QStringLiteral("callHeader"));
        auto *group = root->findChild<QQuickItem *>(QStringLiteral("groupParticipants"));
        auto *pair = root->findChild<QQuickItem *>(QStringLiteral("callParticipants"));
        auto *groupTitle = root->findChild<QQuickItem *>(QStringLiteral("groupCallTitle"));
        auto *local = root->findChild<QQuickItem *>(QStringLiteral("groupLocalParticipant"));
        QVERIFY(callHeader && group && pair && groupTitle && local);
        QVERIFY(callHeader->isVisible());
        // The group layout replaces the two-person one.
        QVERIFY(group->isVisible());
        QVERIFY(!pair->isVisible());
        QCOMPARE(groupTitle->property("text").toString(), QStringLiteral("Weekend plans"));
        QCOMPARE(local->property("name").toString(), QStringLiteral("Developer"));

        // Every member is on screen with what they are doing; the one talking
        // is ringed green, the one who declined is faded.
        auto *jessicaItem = findVisualItem(group, QStringLiteral("groupParticipant_d1"));
        auto *michaelItem = findVisualItem(group, QStringLiteral("groupParticipant_d2"));
        auto *ryanItem = findVisualItem(group, QStringLiteral("groupParticipant_d3"));
        QVERIFY(jessicaItem && michaelItem && ryanItem);
        QCOMPARE(michaelItem->property("caption").toString(), QStringLiteral("Ringing…"));
        QCOMPARE(ryanItem->property("caption").toString(), QStringLiteral("Declined"));
        QVERIFY(jessicaItem->property("caption").toString().isEmpty());
        QVERIFY(jessicaItem->property("speaking").toBool());
        QVERIFY(ryanItem->property("dimmed").toBool());
        QVERIFY(!michaelItem->property("dimmed").toBool());
        auto *ryanCaption = ryanItem->findChild<QQuickItem *>(QStringLiteral("participantCaption"));
        QVERIFY(ryanCaption && ryanCaption->isVisible());
        // Us first, then the members, left to right.
        QVERIFY(local->mapToScene(QPointF()).x() < jessicaItem->mapToScene(QPointF()).x());
        // The slot grew to fit everyone.
        auto *slot = root->findChild<QQuickItem *>(QStringLiteral("conversationHeaderSlot"));
        QVERIFY(slot);
        QVERIFY(slot->height() >= callHeader->implicitHeight());
        // An incoming group call offers answer/decline like any other.
        auto *accept = root->findChild<QQuickItem *>(QStringLiteral("acceptCallButton"));
        QVERIFY(accept && accept->isVisible());
        auto *status = root->findChild<QQuickItem *>(QStringLiteral("callStatusText"));
        QVERIFY(status);
        QCOMPARE(status->property("text").toString(), QStringLiteral("Incoming group call"));

        // Back to a one-to-one preview: the pair layout returns.
        callController.enableForPreview(OpenChat::CallState::Active, QStringLiteral("Jessica"),
                                        QStringLiteral("jessica"), true, false);
        QCoreApplication::processEvents();
        QVERIFY(pair->isVisible());
        QVERIFY(!group->isVisible());
    }

    void groupMessagesNameTheirSender()
    {
        QQmlEngine engine;
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        QQmlComponent component(&engine);
        component.loadFromModule("OpenChat", "MessageDelegate");
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        const auto make = [&component](int direction, const QString &sender) {
            return component.createWithInitialProperties(
                {{"direction", direction}, {"deliveryState", 0}, {"body", "hello"},
                 {"timestamp", "10:15 AM"}, {"kind", 0}, {"dateLabel", ""},
                 {"showDateDivider", false}, {"senderName", sender}, {"width", 540}});
        };
        QScopedPointer<QObject> named(make(0, QStringLiteral("carol")));
        QScopedPointer<QObject> plain(make(0, QString()));
        QScopedPointer<QObject> outgoing(make(1, QStringLiteral("me")));
        QVERIFY(named && plain && outgoing);
        auto *label = named->findChild<QObject *>(QStringLiteral("messageSender"));
        QVERIFY(label);
        QVERIFY(label->property("visible").toBool());
        QCOMPARE(label->property("text").toString(), QStringLiteral("carol"));
        // The label takes its own room above the bubble; without one the
        // delegate is exactly as tall as before.
        QVERIFY(named->property("implicitHeight").toReal()
                > plain->property("implicitHeight").toReal());
        QVERIFY(!plain->findChild<QObject *>(QStringLiteral("messageSender"))->property("visible").toBool());
        QVERIFY(!outgoing->findChild<QObject *>(QStringLiteral("messageSender"))->property("visible").toBool());
    }

    void theCallScreenOffersTheRightActionForEachStage()
    {
        OpenChat::ChatController chatController;
        OpenChat::CallController callController;
        callController.enableForPreview(OpenChat::CallState::Ringing, QStringLiteral("Jessica"),
                                        QStringLiteral("jessica"), false, false);

        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{QStringLiteral("chatController"), QVariant::fromValue(&chatController)},
             {QStringLiteral("callController"), QVariant::fromValue(&callController)}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");
        QCOMPARE(engine.rootObjects().size(), 1);
        QObject *root = engine.rootObjects().constFirst();

        auto *accept = root->findChild<QQuickItem *>(QStringLiteral("acceptCallButton"));
        auto *decline = root->findChild<QQuickItem *>(QStringLiteral("declineCallButton"));
        auto *mute = root->findChild<QQuickItem *>(QStringLiteral("muteCallButton"));
        auto *end = root->findChild<QQuickItem *>(QStringLiteral("endCallButton"));
        auto *dismiss = root->findChild<QQuickItem *>(QStringLiteral("dismissCallButton"));
        QVERIFY(accept && decline && mute && end && dismiss);

        // Ringing: answer or refuse, and nothing else — there is no call to mute
        // or hang up yet.
        QVERIFY(accept->isVisible());
        QVERIFY(decline->isVisible());
        QVERIFY(!mute->isVisible());
        QVERIFY(!end->isVisible());
        QVERIFY(!dismiss->isVisible());

        callController.enableForPreview(OpenChat::CallState::Active, QStringLiteral("Jessica"),
                                        QStringLiteral("jessica"), true, false);
        QCoreApplication::processEvents();
        QVERIFY(!accept->isVisible());
        QVERIFY(mute->isVisible());
        QVERIFY(end->isVisible());
        // A live call shows how long it has been running.
        auto *status = root->findChild<QQuickItem *>(QStringLiteral("callStatusText"));
        QVERIFY(status);
        QCOMPARE(status->property("text").toString(), QStringLiteral("2:34"));

        callController.enableForPreview(OpenChat::CallState::Ended, QStringLiteral("Jessica"),
                                        QStringLiteral("jessica"), false, false);
        QCoreApplication::processEvents();
        // Ended: only a way back to the conversation, plus why it ended.
        QVERIFY(!mute->isVisible());
        QVERIFY(!end->isVisible());
        QVERIFY(dismiss->isVisible());
    }
};

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    qputenv("QT_QUICK_BACKEND", QByteArrayLiteral("software"));
    QGuiApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("OpenChatTests"));
    QCoreApplication::setApplicationName(QStringLiteral("qml-appearance"));
    QTemporaryDir settingsDirectory;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
    qmlRegisterSingletonType<OpenChat::AppearanceSettings>(
        "OpenChat.Native", 1, 0, "AppearanceSettings",
        [](QQmlEngine *, QJSEngine *) -> QObject * { return new OpenChat::AppearanceSettings; });
    qmlRegisterType<OpenChat::BubbleBackground>(
        "OpenChat.Native", 1, 0, "BubbleBackground");
    qmlRegisterType<OpenChat::CallVideoItem>("OpenChat.Native", 1, 0, "CallVideoItem");
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
