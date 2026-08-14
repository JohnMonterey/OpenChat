#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QtTest>

#include "controllers/ChatController.h"
#include "render/BubbleBackground.h"

class QmlLoadTest final : public QObject
{
    Q_OBJECT

private slots:
    void requiredStructure()
    {
        OpenChat::ChatController controller;
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("chatController"), &controller);
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");

        QCOMPARE(engine.rootObjects().size(), 1);
        QObject *root = engine.rootObjects().constFirst();
        QCOMPARE(root->property("minimumWidth").toInt(), 720);
        QCOMPARE(root->property("minimumHeight").toInt(), 560);
        QVERIFY(root->findChild<QObject *>(QStringLiteral("aeroWindowFrame")));
        QVERIFY(root->findChild<QObject *>(QStringLiteral("contactSidebar")));
        QVERIFY(root->findChild<QObject *>(QStringLiteral("favoritesCategory")));
        QVERIFY(root->findChild<QObject *>(QStringLiteral("contactsCategory")));
        QVERIFY(root->findChild<QObject *>(QStringLiteral("contactSearch")));
    }

    void conversationStructureAndSending()
    {
        OpenChat::ChatController controller;
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("chatController"), &controller);
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");

        QCOMPARE(engine.rootObjects().size(), 1);
        QObject *root = engine.rootObjects().constFirst();
        QVERIFY(root->findChild<QObject *>(QStringLiteral("conversationHeader")));
        QVERIFY(root->findChild<QObject *>(QStringLiteral("videoCallButton")));
        QVERIFY(root->findChild<QObject *>(QStringLiteral("phoneCallButton")));
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
    }
};

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    qputenv("QT_QUICK_BACKEND", QByteArrayLiteral("software"));
    QGuiApplication application(argc, argv);
    qmlRegisterType<OpenChat::BubbleBackground>(
        "OpenChat.Native", 1, 0, "BubbleBackground");
    QmlLoadTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_qmlload.moc"
