#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <qqml.h>

#include "app/AppMetadata.h"
#include "controllers/ChatController.h"
#include "render/BubbleBackground.h"

int main(int argc, char *argv[])
{
    QGuiApplication application(argc, argv);
    QCoreApplication::setApplicationName(OpenChat::AppMetadata::name.toString());
    QCoreApplication::setOrganizationName(QStringLiteral("OpenChat"));

    qmlRegisterType<OpenChat::BubbleBackground>(
        "OpenChat.Native", 1, 0, "BubbleBackground");

    OpenChat::ChatController chatController;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("chatController"), &chatController);
    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &application,
        [] { QCoreApplication::exit(EXIT_FAILURE); },
        Qt::QueuedConnection);
    engine.loadFromModule("OpenChat", "Main");

    if (engine.rootObjects().isEmpty())
        return EXIT_FAILURE;

    return application.exec();
}
