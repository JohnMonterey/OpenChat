#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QtTest>

#include "controllers/ChatController.h"

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
};

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    qputenv("QT_QUICK_BACKEND", QByteArrayLiteral("software"));
    QGuiApplication application(argc, argv);
    QmlLoadTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_qmlload.moc"
