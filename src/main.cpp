#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QGuiApplication>
#include <QIcon>
#include <QQuickWindow>
#include <QQmlApplicationEngine>
#include <QTimer>
#include <qqml.h>

#include <algorithm>

#include "app/AppMetadata.h"
#include "controllers/ChatController.h"
#include "render/AvatarArtwork.h"
#include "render/BubbleBackground.h"

int main(int argc, char *argv[])
{
    QGuiApplication application(argc, argv);
    QCoreApplication::setApplicationName(OpenChat::AppMetadata::name.toString());
    QCoreApplication::setOrganizationName(QStringLiteral("OpenChat"));
    QGuiApplication::setWindowIcon(
        QIcon(QStringLiteral(":/qt/qml/OpenChat/assets/icons/openchat.svg")));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("OpenChat secure chat client"));
    parser.addHelpOption();
    const QCommandLineOption captureOption(
        QStringLiteral("capture"), QStringLiteral("Save a rendered window capture to <path>."),
        QStringLiteral("path"));
    const QCommandLineOption delayOption(
        QStringLiteral("capture-delay"), QStringLiteral("Wait <milliseconds> before capture."),
        QStringLiteral("milliseconds"), QStringLiteral("500"));
    const QCommandLineOption widthOption(
        QStringLiteral("width"), QStringLiteral("Override window width."),
        QStringLiteral("pixels"));
    const QCommandLineOption heightOption(
        QStringLiteral("height"), QStringLiteral("Override window height."),
        QStringLiteral("pixels"));
    parser.addOptions({captureOption, delayOption, widthOption, heightOption});
    parser.process(application);

    qmlRegisterType<OpenChat::BubbleBackground>(
        "OpenChat.Native", 1, 0, "BubbleBackground");
    qmlRegisterType<OpenChat::AvatarArtwork>(
        "OpenChat.Native", 1, 0, "AvatarArtwork");
    qmlRegisterUncreatableType<OpenChat::ChatController>(
        "OpenChat.Native", 1, 0, "ChatController",
        QStringLiteral("ChatController is provided by the application"));

    OpenChat::ChatController chatController;
    QQmlApplicationEngine engine;
    engine.setInitialProperties(
        {{QStringLiteral("chatController"), QVariant::fromValue(&chatController)}});
    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &application,
        [] { QCoreApplication::exit(EXIT_FAILURE); },
        Qt::QueuedConnection);
    engine.loadFromModule("OpenChat", "Main");

    if (engine.rootObjects().isEmpty())
        return EXIT_FAILURE;

    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
    if (!window)
        return EXIT_FAILURE;

    bool widthValid = false;
    bool heightValid = false;
    const int requestedWidth = parser.value(widthOption).toInt(&widthValid);
    const int requestedHeight = parser.value(heightOption).toInt(&heightValid);
    if (widthValid && requestedWidth >= OpenChat::AppMetadata::minimumWidth)
        window->setWidth(requestedWidth);
    if (heightValid && requestedHeight >= OpenChat::AppMetadata::minimumHeight)
        window->setHeight(requestedHeight);

    if (parser.isSet(captureOption)) {
        bool delayValid = false;
        const int requestedDelay = parser.value(delayOption).toInt(&delayValid);
        const int delay = delayValid ? std::max(0, requestedDelay) : 500;
        const QString capturePath = QDir::current().absoluteFilePath(parser.value(captureOption));
        QTimer::singleShot(delay, window, [window, capturePath] {
            const bool saved = window->grabWindow().save(capturePath, "PNG");
            QCoreApplication::exit(saved ? EXIT_SUCCESS : EXIT_FAILURE);
        });
    }

    return application.exec();
}
