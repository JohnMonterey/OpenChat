// Renders every profile cosmetic through the real QML components and saves
// labelled PNG sheets, in the light and the dark theme, plus the chat window
// with a loadout equipped. A design tool run by hand, not part of the app:
//
//   openchat-profile-gallery [--out DIR] [--scale N] [--page NAME]...
//
// Pages: avatar-frames, presence-beads, name-flair, profile-scenes,
// in-context. By default every page is rendered into <source>/cosmetics-shots
// at a device pixel ratio of 2. Settings live in a throwaway directory, so the
// user's own appearance choices are never read or written.

#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QQuickItem>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>
#include <qqml.h>

#include <cstdio>

#include "app/AppearanceSettings.h"
#include "app/MicrophoneSettings.h"
#include "app/VoiceEffectHost.h"
#include "app/ComposerEditing.h"
#include "app/TextLineSpacing.h"
#include "app/TransportSettings.h"
#include "case/DailyCaseController.h"
#include "controllers/CallController.h"
#include "controllers/ChatController.h"
#include "controllers/ContactController.h"
#include "controllers/OnboardingController.h"
#include "cosmetics/CosmeticTypes.h"
#include "render/AvatarArtwork.h"
#include "render/BubbleBackground.h"
#include "render/CallVideoItem.h"

namespace {

void registerTypes()
{
    qmlRegisterType<OpenChat::DailyCaseController>("OpenChat.Native", 1, 0, "DailyCaseController");
    qmlRegisterSingletonType<OpenChat::AppearanceSettings>(
        "OpenChat.Native", 1, 0, "AppearanceSettings",
        [](QQmlEngine *, QJSEngine *) -> QObject * { return new OpenChat::AppearanceSettings; });
    qmlRegisterSingletonType<OpenChat::MicrophoneSettings>(
        "OpenChat.Native", 1, 0, "MicrophoneSettings",
        [](QQmlEngine *, QJSEngine *) -> QObject * { return new OpenChat::MicrophoneSettings; });
    qmlRegisterSingletonType<OpenChat::VoiceEffectHost>(
        "OpenChat.Native", 1, 0, "VoiceEffectHost",
        [](QQmlEngine *, QJSEngine *) -> QObject * { return new OpenChat::VoiceEffectHost; });
    qmlRegisterType<OpenChat::ComposerEditing>("OpenChat.Native", 1, 0, "ComposerEditing");
    qmlRegisterType<OpenChat::TextLineSpacing>("OpenChat.Native", 1, 0, "TextLineSpacing");
    qmlRegisterSingletonType<OpenChat::TransportSettings>(
        "OpenChat.Native", 1, 0, "TransportSettings",
        [](QQmlEngine *, QJSEngine *) -> QObject * { return new OpenChat::TransportSettings; });
    qmlRegisterType<OpenChat::BubbleBackground>("OpenChat.Native", 1, 0, "BubbleBackground");
    qmlRegisterType<OpenChat::CallVideoItem>("OpenChat.Native", 1, 0, "CallVideoItem");
    qmlRegisterType<OpenChat::AvatarArtwork>("OpenChat.Native", 1, 0, "AvatarArtwork");
    OpenChat::registerCosmeticQmlTypes();
    qmlRegisterUncreatableType<OpenChat::ChatController>(
        "OpenChat.Native", 1, 0, "ChatController", QStringLiteral("provided by the gallery"));
    qmlRegisterUncreatableType<OpenChat::OnboardingController>(
        "OpenChat.Native", 1, 0, "OnboardingController", QStringLiteral("provided by the gallery"));
    qmlRegisterUncreatableType<OpenChat::ContactController>(
        "OpenChat.Native", 1, 0, "ContactController", QStringLiteral("provided by the gallery"));
    qmlRegisterUncreatableType<OpenChat::CallController>(
        "OpenChat.Native", 1, 0, "CallController", QStringLiteral("provided by the gallery"));
}

void collectItems(QQuickItem *item, QList<QQuickItem *> &out)
{
    out.append(item);
    for (QQuickItem *child : item->childItems())
        collectItems(child, out);
}

// Magnifies real rendered pixels: every item named "zoomSource:<key>" is
// copied from the grab, scaled up nearest-neighbour, into the item named
// "zoomTarget:<key>", so the sheet shows exactly what the app draws.
void applyZooms(QQuickWindow *window, QImage &image)
{
    QList<QQuickItem *> items;
    collectItems(window->contentItem(), items);
    const qreal dpr = window->effectiveDevicePixelRatio();
    image.setDevicePixelRatio(1.0);
    QPainter painter(&image);
    for (QQuickItem *source : items) {
        if (!source->objectName().startsWith(QLatin1String("zoomSource:")))
            continue;
        const QString key = source->objectName().mid(11);
        for (QQuickItem *target : items) {
            if (target->objectName() != QLatin1String("zoomTarget:") + key)
                continue;
            const QRectF from = source->mapRectToScene(QRectF(0, 0, source->width(), source->height()));
            const QRectF to = target->mapRectToScene(QRectF(0, 0, target->width(), target->height()));
            const QRect fromPx(qRound(from.x() * dpr), qRound(from.y() * dpr),
                               qRound(from.width() * dpr), qRound(from.height() * dpr));
            const QRect toPx(qRound(to.x() * dpr), qRound(to.y() * dpr), qRound(to.width() * dpr),
                             qRound(to.height() * dpr));
            painter.drawImage(toPx.topLeft(), image.copy(fromPx).scaled(toPx.size(), Qt::IgnoreAspectRatio,
                                                                        Qt::FastTransformation));
        }
    }
    painter.end();
    image.setDevicePixelRatio(dpr);
}

// Lets the window lay out and render, then grabs it.
QImage settleAndGrab(QQuickWindow *window, int settleMs)
{
    window->show();
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < settleMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return window->grabWindow();
}

void resetAppearance(bool dark)
{
    QSettings settings;
    settings.setValue(QStringLiteral("Appearance/darkMode"), dark);
    for (const char *key : {"Appearance/avatarFrame", "Appearance/presenceBead",
                            "Appearance/nameFlair", "Appearance/profileScene"})
        settings.remove(QLatin1String(key));
    settings.sync();
}

bool renderSheet(const QString &page, bool dark, const QString &outDir)
{
    resetAppearance(dark);
    OpenChat::ChatController shortName;
    shortName.setLocalUserName(QStringLiteral("Daniel"));
    OpenChat::ChatController longName;
    longName.setLocalUserName(QStringLiteral("Maximilian Alexander von Hohenberg"));
    longName.setLocalStatusText(QStringLiteral("Out walking the dog, back soon"));

    QQmlApplicationEngine engine;
    engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
    engine.rootContext()->setContextProperty(QStringLiteral("galleryShortName"), &shortName);
    engine.rootContext()->setContextProperty(QStringLiteral("galleryLongName"), &longName);
    engine.rootContext()->setContextProperty(QStringLiteral("galleryDark"), dark);
    const QString file = QStringLiteral(OPENCHAT_SOURCE_DIR "/tools/profile_gallery/%1.qml")
                             .arg(page == QLatin1String("avatar-frames")    ? QStringLiteral("FramesSheet")
                                  : page == QLatin1String("presence-beads") ? QStringLiteral("BeadsSheet")
                                  : page == QLatin1String("name-flair")     ? QStringLiteral("FlairSheet")
                                  : page == QLatin1String("rarity-ladder")  ? QStringLiteral("RaritySheet")
                                  : page == QLatin1String("case-reveals")   ? QStringLiteral("RevealSheet")
                                                                            : QStringLiteral("ScenesSheet"));
    engine.load(QUrl::fromLocalFile(file));
    if (engine.rootObjects().isEmpty())
        return false;
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
    if (!window)
        return false;
    QImage image = settleAndGrab(window, 700);
    applyZooms(window, image);
    const QString path = QDir(outDir).filePath(
        QStringLiteral("%1-%2.png").arg(page, dark ? QStringLiteral("dark") : QStringLiteral("light")));
    const bool saved = image.save(path, "PNG");
    std::printf("%s %s (%dx%d)\n", saved ? "wrote" : "FAILED", qPrintable(path), image.width(),
                image.height());
    return saved;
}

// The real chat window, with a loadout equipped through the saved settings
// exactly as a user's choice would be.
bool renderContext(bool dark, const QString &outDir)
{
    resetAppearance(dark);
    {
        QSettings settings;
        settings.setValue(QStringLiteral("Appearance/avatarFrame"), QStringLiteral("frame.frost"));
        settings.setValue(QStringLiteral("Appearance/presenceBead"), QStringLiteral("bead.gem"));
        settings.setValue(QStringLiteral("Appearance/nameFlair"), QStringLiteral("flair.holo"));
        settings.setValue(QStringLiteral("Appearance/profileScene"), QStringLiteral("scene.aurora"));
        settings.sync();
    }
    // Only what the window's (scratch) account has unboxed is worn.
    if (!OpenChat::LocalCosmeticInventory().grant(
            QStringLiteral("preview"), {QStringLiteral("frame.frost"), QStringLiteral("bead.gem"),
                                        QStringLiteral("flair.holo"), QStringLiteral("scene.aurora")}))
        return false;
    OpenChat::ChatController chat;
    chat.setLocalUserName(QStringLiteral("Daniel"));
    chat.setLocalStatusText(QStringLiteral("Snowed in, send cocoa"));
    QQmlApplicationEngine engine;
    engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
    engine.setInitialProperties({{QStringLiteral("chatController"), QVariant::fromValue(&chat)}});
    engine.loadFromModule("OpenChat", "Main");
    if (engine.rootObjects().isEmpty())
        return false;
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
    if (!window)
        return false;
    window->setWidth(860);
    window->setHeight(680);
    const QImage image = settleAndGrab(window, 900);
    const QString path = QDir(outDir).filePath(
        QStringLiteral("in-context-%1.png").arg(dark ? QStringLiteral("dark") : QStringLiteral("light")));
    const bool saved = image.save(path, "PNG");
    std::printf("%s %s (%dx%d)\n", saved ? "wrote" : "FAILED", qPrintable(path), image.width(),
                image.height());
    return saved;
}

} // namespace

int main(int argc, char **argv)
{
    QByteArray scale = "2";
    for (int i = 1; i + 1 < argc; ++i) {
        if (qstrcmp(argv[i], "--scale") == 0)
            scale = argv[i + 1];
    }
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    if (qEnvironmentVariableIsEmpty("QT_QUICK_BACKEND"))
        qputenv("QT_QUICK_BACKEND", "software");
    qputenv("QT_SCALE_FACTOR", scale);

    QTemporaryDir scratch;
    qputenv("XDG_DATA_HOME", scratch.path().toUtf8());
    QGuiApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("OpenChatGallery"));
    QCoreApplication::setApplicationName(QStringLiteral("profile-gallery"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, scratch.path());

    QCommandLineParser parser;
    parser.addHelpOption();
    const QCommandLineOption outOption(QStringLiteral("out"), QStringLiteral("Output directory."),
                                       QStringLiteral("dir"),
                                       QStringLiteral(OPENCHAT_SOURCE_DIR "/cosmetics-shots"));
    const QCommandLineOption scaleOption(QStringLiteral("scale"),
                                         QStringLiteral("Device pixel ratio (default 2)."),
                                         QStringLiteral("n"));
    const QCommandLineOption pageOption(QStringLiteral("page"), QStringLiteral("Only this page."),
                                        QStringLiteral("name"));
    parser.addOptions({outOption, scaleOption, pageOption});
    parser.process(application);

    registerTypes();
    const QString outDir = parser.value(outOption);
    QDir().mkpath(outDir);
    QStringList pages = parser.values(pageOption);
    if (pages.isEmpty())
        pages = {QStringLiteral("avatar-frames"), QStringLiteral("presence-beads"),
                 QStringLiteral("name-flair"), QStringLiteral("profile-scenes"),
                 QStringLiteral("rarity-ladder"), QStringLiteral("case-reveals"),
                 QStringLiteral("in-context")};
    bool ok = true;
    for (const QString &page : pages) {
        for (const bool dark : {false, true}) {
            ok = (page == QLatin1String("in-context") ? renderContext(dark, outDir)
                                                      : renderSheet(page, dark, outDir))
                 && ok;
        }
    }
    return ok ? 0 : 1;
}
