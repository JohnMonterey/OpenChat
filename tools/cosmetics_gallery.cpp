// Renders every collectible chat-bubble skin through the application's real
// MessageDelegate and BubbleBackground, and saves the contact sheet in the
// light and dark themes. Not part of the application: it is how the skins are
// reviewed, and it runs headless (offscreen platform, software renderer, the
// same setup the capture tests use).
//
//   openchat-cosmetics-gallery [--output-dir DIR] [--scale 2] [--dump-tiles]

#include "app/AppearanceSettings.h"
#include "app/TextLineSpacing.h"
#include "cosmetics/BubbleSkins.h"
#include "render/BubbleBackground.h"

#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QQmlEngine>
#include <QQuickView>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>

#include <cstring>

namespace {

void settle(int milliseconds)
{
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

} // namespace

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    if (qEnvironmentVariableIsEmpty("QT_QUICK_BACKEND"))
        qputenv("QT_QUICK_BACKEND", QByteArrayLiteral("software"));
    // The scale factor has to be in place before the application exists.
    QByteArray scale = QByteArrayLiteral("2");
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--scale") == 0)
            scale = argv[i + 1];
    }
    qputenv("QT_SCALE_FACTOR", scale);

    QGuiApplication application(argc, argv);
    // Keep the gallery's appearance switches out of the user's real settings.
    QCoreApplication::setOrganizationName(QStringLiteral("OpenChatCosmeticsGallery"));
    QCoreApplication::setApplicationName(QStringLiteral("bubble-skins"));
    QTemporaryDir settingsDirectory;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Render the chat-bubble skin gallery."));
    parser.addHelpOption();
    const QCommandLineOption outputOption(
        QStringLiteral("output-dir"), QStringLiteral("Directory for the PNGs."),
        QStringLiteral("dir"), QStringLiteral(OPENCHAT_SOURCE_DIR "/cosmetics-shots"));
    const QCommandLineOption scaleOption(QStringLiteral("scale"),
                                         QStringLiteral("Device pixel ratio (default 2)."),
                                         QStringLiteral("factor"), QStringLiteral("2"));
    const QCommandLineOption tilesOption(QStringLiteral("dump-tiles"),
                                         QStringLiteral("Also save each skin's raw texture tile."));
    parser.addOptions({outputOption, scaleOption, tilesOption});
    parser.process(application);

    QTextStream err(stderr);
    const QDir output(parser.value(outputOption));
    if (!QDir().mkpath(output.absolutePath())) {
        err << "Cannot create " << output.absolutePath() << Qt::endl;
        return 1;
    }

    qmlRegisterSingletonType<OpenChat::AppearanceSettings>(
        "OpenChat.Native", 1, 0, "AppearanceSettings",
        [](QQmlEngine *, QJSEngine *) -> QObject * { return new OpenChat::AppearanceSettings; });
    qmlRegisterType<OpenChat::BubbleBackground>("OpenChat.Native", 1, 0, "BubbleBackground");
    qmlRegisterType<OpenChat::TextLineSpacing>("OpenChat.Native", 1, 0, "TextLineSpacing");

    QVariantList skins;
    skins.append(QVariantMap{{QStringLiteral("id"), QString()},
                             {QStringLiteral("name"), QStringLiteral("Classic")},
                             {QStringLiteral("description"),
                              QStringLiteral("The default bubble, for reference.")}});
    for (const OpenChat::BubbleSkinInfo &skin : OpenChat::BubbleSkins::catalog()) {
        skins.append(QVariantMap{{QStringLiteral("id"), skin.id},
                                 {QStringLiteral("name"), skin.name},
                                 {QStringLiteral("description"), skin.description}});
    }

    QElapsedTimer warmup;
    warmup.start();
    for (const OpenChat::BubbleSkinInfo &skin : OpenChat::BubbleSkins::catalog()) {
        const QImage tile = OpenChat::BubbleSkins::texture(skin.id);
        if (parser.isSet(tilesOption))
            tile.save(output.filePath(QStringLiteral("tile-%1.png").arg(skin.id)), "PNG");
    }
    err << "Generated " << OpenChat::BubbleSkins::catalog().size() << " skin textures in "
        << warmup.elapsed() << " ms" << Qt::endl;

    QQuickView view;
    view.engine()->addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
    view.setInitialProperties({{QStringLiteral("skins"), skins}});
    view.setResizeMode(QQuickView::SizeViewToRootObject);
    view.setSource(QUrl::fromLocalFile(QStringLiteral(OPENCHAT_SOURCE_DIR
                                                      "/tools/cosmetics_gallery.qml")));
    if (view.status() != QQuickView::Ready) {
        for (const QQmlError &error : view.errors())
            err << error.toString() << Qt::endl;
        return 1;
    }
    view.show();

    auto *appearance = view.engine()->singletonInstance<OpenChat::AppearanceSettings *>(
        "OpenChat.Native", "AppearanceSettings");
    if (!appearance) {
        err << "AppearanceSettings is unavailable" << Qt::endl;
        return 1;
    }

    for (const bool dark : {false, true}) {
        appearance->setDarkMode(dark);
        settle(250);
        const QImage shot = view.grabWindow();
        const QString path = output.filePath(dark ? QStringLiteral("bubble-skins-dark.png")
                                                  : QStringLiteral("bubble-skins-light.png"));
        if (shot.isNull() || !shot.save(path, "PNG")) {
            err << "Could not save " << path << Qt::endl;
            return 1;
        }
        err << "Saved " << path << " (" << shot.width() << "x" << shot.height() << ")"
            << Qt::endl;
    }
    return 0;
}
