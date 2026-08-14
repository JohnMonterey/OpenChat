#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQuickItem>
#include <QWindow>
#include <QtTest>

#include "controllers/ChatController.h"
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
    QmlLoadTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_qmlload.moc"
