#include <QGuiApplication>
#include <QQmlApplicationEngine>

#include "app/AppMetadata.h"

int main(int argc, char *argv[])
{
    QGuiApplication application(argc, argv);
    QCoreApplication::setApplicationName(OpenChat::AppMetadata::name.toString());
    QCoreApplication::setOrganizationName(QStringLiteral("OpenChat"));

    QQmlApplicationEngine engine;
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

