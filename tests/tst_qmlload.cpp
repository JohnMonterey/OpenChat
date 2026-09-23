#include <QColor>
#include <QClipboard>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QSettings>
#include <QUrl>
#include <QWindow>
#include <QtTest>
#include <QScopeGuard>
#include <QStyleHints>
#include <QQuickWindow>

#include <algorithm>
#include <memory>
#include <optional>

#include "controllers/CallController.h"
#include "controllers/ChatController.h"
#include "controllers/ContactController.h"
#include "controllers/CrashReportController.h"
#include "controllers/OnboardingController.h"
#include "controllers/VoiceDebugController.h"
#include "models/RequestListModel.h"
#include "render/AvatarArtwork.h"
#include "app/AppearanceSettings.h"
#include "app/MemorySettings.h"
#include "app/MicrophoneSettings.h"
#include "app/VoiceEffectHost.h"
#include "app/ComposerEditing.h"
#include "app/TransportSettings.h"
#include <QQmlExpression>
#include <QQmlContext>
#include "call/ScreenCanvas.h"
#include "case/DailyCaseController.h"
#include "cosmetics/CosmeticTypes.h"
#include "cosmetics/CosmeticCatalog.h"
#include "render/CallVideoItem.h"
#include "render/BubbleBackground.h"
#include "cosmetics/BubbleSkins.h"

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

void clearEquippedCosmetics()
{
    QSettings settings;
    for (const char *key : {"Appearance/avatarFrame", "Appearance/presenceBead",
                            "Appearance/nameFlair", "Appearance/profileScene"})
        settings.remove(QLatin1String(key));
}

// Every equipped-cosmetic setting, the bubble skin included.
void clearAllCosmetics()
{
    clearEquippedCosmetics();
    QSettings().remove(QStringLiteral("Appearance/bubbleSkin"));
}

// The AppearanceSettings property that equips an item of `category`.
QByteArray equipProperty(const QString &category)
{
    if (category == QLatin1String("bubble")) return "bubbleSkin";
    if (category == QLatin1String("frame")) return "avatarFrame";
    if (category == QLatin1String("bead")) return "presenceBead";
    if (category == QLatin1String("flair")) return "nameFlair";
    if (category == QLatin1String("scene")) return "profileScene";
    return {};
}

// One row per catalogue item and theme.
void addEveryCosmetic()
{
    QTest::addColumn<QString>("id");
    QTest::addColumn<bool>("dark");
    for (const OpenChat::CosmeticInfo &info : OpenChat::CosmeticCatalog::all()) {
        for (const bool dark : {false, true})
            QTest::newRow(qPrintable(info.id + (dark ? QStringLiteral(" dark") : QStringLiteral(" light"))))
                << info.id << dark;
    }
}

QRect deviceRect(QQuickItem *item, qreal dpr, int margin = 0)
{
    const QRectF scene = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
    return QRect(QPoint(qFloor(scene.left() * dpr), qFloor(scene.top() * dpr)),
                 QPoint(qCeil(scene.right() * dpr) - 1, qCeil(scene.bottom() * dpr) - 1))
        .adjusted(-margin, -margin, margin, margin);
}

bool pixelsDiffer(QRgb a, QRgb b)
{
    constexpr int tolerance = 6;
    return qAbs(qRed(a) - qRed(b)) > tolerance || qAbs(qGreen(a) - qGreen(b)) > tolerance
        || qAbs(qBlue(a) - qBlue(b)) > tolerance || qAbs(qAlpha(a) - qAlpha(b)) > tolerance;
}

// Where two grabs of the same window differ: how much inside `allowed`, how
// much (and where) outside it, skipping pixels in `ignored`.
struct GrabDiff
{
    int inside = 0;
    int outside = 0;
    QRect outsideBox;
    QList<int> perRect; // changed pixels inside each allowed rect
};

GrabDiff diffGrabs(const QImage &before, const QImage &after, const QList<QRect> &allowed,
                   const QRegion &ignored = {})
{
    GrabDiff diff;
    diff.perRect.fill(0, allowed.size());
    const QImage a = before.convertToFormat(QImage::Format_ARGB32);
    const QImage b = after.convertToFormat(QImage::Format_ARGB32);
    if (a.size() != b.size()) {
        diff.outside = -1;
        return diff;
    }
    for (int y = 0; y < a.height(); ++y) {
        const auto *la = reinterpret_cast<const QRgb *>(a.constScanLine(y));
        const auto *lb = reinterpret_cast<const QRgb *>(b.constScanLine(y));
        for (int x = 0; x < a.width(); ++x) {
            if (!pixelsDiffer(la[x], lb[x]) || ignored.contains(QPoint(x, y)))
                continue;
            bool within = false;
            for (int i = 0; i < allowed.size(); ++i) {
                if (allowed[i].contains(x, y)) {
                    within = true;
                    ++diff.perRect[i];
                }
            }
            if (within) {
                ++diff.inside;
            } else {
                ++diff.outside;
                diff.outsideBox |= QRect(x, y, 1, 1);
            }
        }
    }
    return diff;
}

// Pixels that change on their own between two grabs (a caret blinking, say).
QRegion unstablePixels(const QImage &first, const QImage &second)
{
    QRegion region;
    const QImage a = first.convertToFormat(QImage::Format_ARGB32);
    const QImage b = second.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < a.height(); ++y) {
        const auto *la = reinterpret_cast<const QRgb *>(a.constScanLine(y));
        const auto *lb = reinterpret_cast<const QRgb *>(b.constScanLine(y));
        for (int x = 0; x < a.width(); ++x) {
            if (pixelsDiffer(la[x], lb[x]))
                region += QRect(x - 2, y - 2, 5, 5);
        }
    }
    return region;
}

QImage settledGrab(QQuickWindow *window)
{
    QCoreApplication::processEvents();
    QTest::qWait(60);
    return window->grabWindow();
}

// The local user's outgoing message bubbles on screen.
QList<QQuickItem *> outgoingBubbles(QQuickItem *root)
{
    QList<QQuickItem *> bubbles;
    const auto visit = [&bubbles](const auto &self, QQuickItem *item) -> void {
        if (item->objectName() == QLatin1String("messageBubble") && item->isVisible()
            && item->property("outgoing").toBool()) {
            const QRectF scene = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
            if (scene.bottom() > 0 && scene.top() < item->window()->height())
                bubbles.append(item);
        }
        for (QQuickItem *child : item->childItems())
            self(self, child);
    };
    visit(visit, root);
    return bubbles;
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
    void init()
    {
        QSettings settings;
        settings.setValue(QStringLiteral("Appearance/darkMode"), false);
        settings.remove(QStringLiteral("Appearance/bubbleSkin"));
    }

    void dailyCaseInteraction()
    {
        OpenChat::ChatController chat;
        QQmlApplicationEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.setInitialProperties({{"chatController", QVariant::fromValue(&chat)},
            {"dailyCaseAccount", QUuid::createUuid().toString()}});
        engine.loadFromModule("OpenChat", "Main");
        QCOMPARE(engine.rootObjects().size(), 1);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QTest::qWait(100);
        auto *entry = window->findChild<QQuickItem *>("dailyCaseButton");
        auto *controller = window->findChild<OpenChat::DailyCaseController *>("dailyCaseController");
        QVERIFY(entry && controller);
        controller->setProperty("muted", true);
        controller->setProperty("reducedMotion", false);
        const auto click = [window](QQuickItem *item) {
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                item->mapToScene(QPointF(item->width()/2, item->height()/2)).toPoint());
        };
        click(entry);
        QVERIFY(window->property("caseRequested").toBool());
        QQuickItem *reel = nullptr;
        QTRY_VERIFY((reel = findVisualItem(window->contentItem(), "caseReel")));
        auto *open = findVisualItem(window->contentItem(), "caseOpenButton");
        QVERIFY(open);
        const auto capture = [window](const QString &name) {
            const auto dir = qEnvironmentVariable("OPENCHAT_CASE_CAPTURES");
            if (!dir.isEmpty()) window->grabWindow().save(dir + '/' + name + ".png");
        };
        QTest::qWait(100);
        capture("available");
        QSignalSpy reveal(controller, &OpenChat::DailyCaseController::revealed);
        QSignalSpy ticks(controller, &OpenChat::DailyCaseController::crossed);
        click(open);
        QCOMPARE(controller->state(), OpenChat::DailyCaseController::Opening);
        QVERIFY(!open->isEnabled());
        click(open);
        QTest::qWait(450);
        window->resize(1200, 720);
        QTest::qWait(150);
        window->resize(720, 560);
        // Every frame: center stays fixed and both ends are outside the viewport.
        int frames = 0;
        const auto connection = connect(controller, &OpenChat::DailyCaseController::positionChanged,
            window, [&] {
                ++frames;
                const auto tile = reel->property("tileWidth").toDouble();
                const auto stride = reel->property("stride").toDouble();
                const auto left = reel->width()/2 - tile/2 - controller->position()*stride;
                QVERIFY(left <= 0);
                QVERIFY(left + (controller->tileCount()-1)*stride + tile >= reel->width());
                auto *selector = reel->findChild<QQuickItem *>("caseSelector");
                QVERIFY(selector);
                QCOMPARE(selector->x() + selector->width()/2, reel->width()/2);
            });
        QTRY_COMPARE_WITH_TIMEOUT(controller->state(), OpenChat::DailyCaseController::OpenedToday, 8500);
        disconnect(connection);
        QVERIFY(frames > 100);
        QVERIFY(ticks.size() > 35);
        QCOMPARE(reveal.size(), 1);
        QCOMPARE(controller->position(), double(controller->winnerIndex()));
        QCOMPARE(reel->property("winnerCenter").toDouble(), reel->width()/2);
        // The reveal names the drawn item and its tier.
        const auto reward = controller->reward();
        QVERIFY(!reward.value("id").toString().isEmpty());
        auto *status = findVisualItem(window->contentItem(), "caseStatus");
        QVERIFY(status);
        QVERIFY(status->property("text").toString().contains(reward.value("name").toString()));
        QVERIFY(status->property("text").toString().contains(reward.value("rarityName").toString()));
        QTest::qWait(700);
        capture("opened");
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!findVisualItem(window->contentItem(), "caseReel"));
        QTRY_VERIFY(entry->hasActiveFocus());
        const int tickCount = ticks.size();
        QTest::qWait(100);
        QCOMPARE(ticks.size(), tickCount);
        click(entry);
        QTRY_VERIFY((reel = findVisualItem(window->contentItem(), "caseReel")));
        QCOMPARE(reel->property("winnerCenter").toDouble(), reel->width()/2);
        QCOMPARE(reveal.size(), 1);
        // Exercise a narrow popup independently of the app's desktop minimum.
        window->setMinimumWidth(320);
        window->resize(392, 560);
        QTest::qWait(100);
        QCOMPARE(reel->property("winnerCenter").toDouble(), reel->width()/2);
        capture("narrow");
        auto *appearance = engine.singletonInstance<OpenChat::AppearanceSettings *>(
            qmlTypeId("OpenChat.Native", 1, 0, "AppearanceSettings"));
        QVERIFY(appearance);
        appearance->setDarkMode(true);
        QTest::qWait(100);
        capture("narrow-dark");
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!findVisualItem(window->contentItem(), "caseReel"));
        window->resize(860, 680);
        controller->setAccountKey(QUuid::createUuid().toString());
        click(entry);
        QTRY_VERIFY(findVisualItem(window->contentItem(), "caseReel"));
        open = findVisualItem(window->contentItem(), "caseOpenButton");
        QVERIFY(open);
        click(open);
        QCOMPARE(controller->state(), OpenChat::DailyCaseController::Opening);
        QTest::qWait(160);
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!findVisualItem(window->contentItem(), "caseReel"));
        QCOMPARE(controller->state(), OpenChat::DailyCaseController::OpenedToday);
        QSignalSpy stopped(controller, &OpenChat::DailyCaseController::positionChanged);
        QTest::qWait(160);
        QCOMPARE(stopped.size(), 0);
        QCOMPARE(reveal.size(), 1);
        click(entry);
        QTRY_VERIFY((reel = findVisualItem(window->contentItem(), "caseReel")));
        QCOMPARE(reel->property("winnerCenter").toDouble(), reel->width()/2);
        QTest::qWait(100);
        capture("dark");
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!findVisualItem(window->contentItem(), "caseReel"));
        appearance->setDarkMode(false);
        QCOMPARE(warnings.size(), 0);
    }

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

    // With nothing equipped the header is exactly the stock one: no cosmetic
    // item is even created, and the name is inked by its own Text.
    void profileCosmeticsAreAbsentByDefault()
    {
        clearEquippedCosmetics();
        OpenChat::ChatController controller;
        controller.setLocalUserName(QStringLiteral("Developer"));
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{QStringLiteral("chatController"), QVariant::fromValue(&controller)}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");
        QCOMPARE(engine.rootObjects().size(), 1);
        QObject *root = engine.rootObjects().constFirst();
        for (const char *name : {"avatarFrame", "beadArt", "localNameFlair", "profileScene"})
            QVERIFY2(!root->findChild<QObject *>(QLatin1String(name)), name);
        QObject *localUserName = root->findChild<QObject *>(QStringLiteral("localUserName"));
        QVERIFY(localUserName);
        QCOMPARE(localUserName->property("color").value<QColor>(), QColor(QStringLiteral("#2b3b53")));
    }

    // Equipped through the saved settings, the local header wears every item,
    // the name keeps its own geometry (so the bead does not move), and nobody
    // else's picture is framed.
    void equippedProfileCosmeticsDecorateOnlyTheLocalHeader()
    {
        const auto measure = [](QQmlApplicationEngine &engine, OpenChat::ChatController &controller) {
            engine.setInitialProperties(
                {{QStringLiteral("chatController"), QVariant::fromValue(&controller)}});
            engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
            engine.loadFromModule("OpenChat", "Main");
            return engine.rootObjects().isEmpty() ? nullptr : engine.rootObjects().constFirst();
        };
        clearEquippedCosmetics();
        OpenChat::ChatController plainController;
        plainController.setLocalUserName(QStringLiteral("Developer"));
        QQmlApplicationEngine plainEngine;
        QObject *plain = measure(plainEngine, plainController);
        QVERIFY(plain);
        auto *plainName = qobject_cast<QQuickItem *>(plain->findChild<QObject *>(QStringLiteral("localUserName")));
        auto *plainBead = qobject_cast<QQuickItem *>(plain->findChild<QObject *>(QStringLiteral("localPresenceButton")));
        QVERIFY(plainName && plainBead);

        {
            QSettings settings;
            settings.setValue(QStringLiteral("Appearance/avatarFrame"), QStringLiteral("frame.gilded"));
            settings.setValue(QStringLiteral("Appearance/presenceBead"), QStringLiteral("bead.gem"));
            settings.setValue(QStringLiteral("Appearance/nameFlair"), QStringLiteral("flair.holo"));
            settings.setValue(QStringLiteral("Appearance/profileScene"), QStringLiteral("scene.aurora"));
        }
        OpenChat::ChatController controller;
        controller.setLocalUserName(QStringLiteral("Developer"));
        QQmlApplicationEngine engine;
        QObject *root = measure(engine, controller);
        QVERIFY(root);
        QCoreApplication::processEvents();

        const QList<QObject *> frames = root->findChildren<QObject *>(QStringLiteral("avatarFrame"));
        QCOMPARE(frames.size(), 1);
        QCOMPARE(frames.constFirst()->property("frameId").toString(), QStringLiteral("frame.gilded"));
        QObject *bead = root->findChild<QObject *>(QStringLiteral("beadArt"));
        QVERIFY(bead);
        QCOMPARE(bead->property("styleId").toString(), QStringLiteral("bead.gem"));
        QObject *flair = root->findChild<QObject *>(QStringLiteral("localNameFlair"));
        QVERIFY(flair);
        QCOMPARE(flair->property("text").toString(), QStringLiteral("Developer"));
        QObject *scene = root->findChild<QObject *>(QStringLiteral("profileScene"));
        QVERIFY(scene);
        QCOMPARE(scene->property("sceneId").toString(), QStringLiteral("scene.aurora"));

        auto *name = qobject_cast<QQuickItem *>(root->findChild<QObject *>(QStringLiteral("localUserName")));
        auto *beadButton = qobject_cast<QQuickItem *>(root->findChild<QObject *>(QStringLiteral("localPresenceButton")));
        QVERIFY(name && beadButton);
        QCOMPARE(name->property("color").value<QColor>().alpha(), 0);
        QCOMPARE(name->x(), plainName->x());
        QCOMPARE(name->width(), plainName->width());
        QCOMPARE(beadButton->x(), plainBead->x());

        // Changing the choice at runtime follows through; clearing it removes the item.
        QObject *appearance = engine.singletonInstance<QObject *>("OpenChat.Native", "AppearanceSettings");
        QVERIFY(appearance);
        QVERIFY(appearance->setProperty("avatarFrame", QStringLiteral("frame.neon")));
        QTRY_COMPARE(root->findChild<QObject *>(QStringLiteral("avatarFrame"))->property("frameId").toString(),
                     QStringLiteral("frame.neon"));
        QVERIFY(appearance->setProperty("nameFlair", QString()));
        QTRY_VERIFY(!root->findChild<QObject *>(QStringLiteral("localNameFlair")));
        QCOMPARE(name->property("color").value<QColor>().alpha(), 255);
        clearEquippedCosmetics();
    }

    void appearanceSettingsValidateAndPersistCosmetics()
    {
        clearEquippedCosmetics();
        {
            OpenChat::AppearanceSettings appearance;
            QCOMPARE(appearance.avatarFrame(), QString());
            appearance.setAvatarFrame(QStringLiteral("frame.pixel"));
            appearance.setPresenceBead(QStringLiteral("bead.star"));
            QCOMPARE(QSettings().value(QStringLiteral("Appearance/avatarFrame")).toString(),
                     QStringLiteral("frame.pixel"));
            // An id from another category, or one this build does not know, is none.
            appearance.setNameFlair(QStringLiteral("bead.gem"));
            QCOMPARE(appearance.nameFlair(), QString());
            appearance.setPresenceBead(QStringLiteral("bead.unknown"));
            QCOMPARE(appearance.presenceBead(), QString());
            QVERIFY(!QSettings().contains(QStringLiteral("Appearance/presenceBead")));
        }
        QSettings().setValue(QStringLiteral("Appearance/profileScene"), QStringLiteral("scene.retired"));
        OpenChat::AppearanceSettings reloaded;
        QCOMPARE(reloaded.avatarFrame(), QStringLiteral("frame.pixel"));
        QCOMPARE(reloaded.profileScene(), QString());
        clearEquippedCosmetics();
    }

    // Every collectible, equipped live in the real window, in both themes:
    // it lands on the right component, visibly changes only that component's
    // pixels, moves nothing, warns nothing, animates only if it should, is
    // remembered across a restart, and unequipping restores the exact pixels.
    void everyCosmeticWorksInTheApp_data() { addEveryCosmetic(); }
    void everyCosmeticWorksInTheApp()
    {
        QFETCH(QString, id);
        QFETCH(bool, dark);
        const OpenChat::CosmeticInfo *info = OpenChat::CosmeticCatalog::find(id);
        QVERIFY(info);
        const QByteArray property = equipProperty(info->category);
        QVERIFY2(!property.isEmpty(), qPrintable(info->category));

        clearAllCosmetics();
        QSettings().setValue(QStringLiteral("Appearance/darkMode"), dark);
        // The composer takes focus at start; hold its caret still so pixel
        // comparisons are not at the mercy of the blink.
        const int flashTime = QGuiApplication::styleHints()->cursorFlashTime();
        QGuiApplication::styleHints()->setCursorFlashTime(0);
        const auto restoreFlash = qScopeGuard([flashTime] {
            QGuiApplication::styleHints()->setCursorFlashTime(flashTime);
        });
        OpenChat::ChatController controller;
        controller.setLocalUserName(QStringLiteral("Developer"));
        QQmlApplicationEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        engine.setInitialProperties({{QStringLiteral("chatController"), QVariant::fromValue(&controller)}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");
        QCOMPARE(engine.rootObjects().size(), 1);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
        QVERIFY(window);
        window->resize(860, 680);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QObject *appearance = engine.singletonInstance<QObject *>("OpenChat.Native", "AppearanceSettings");
        QVERIFY(appearance);
        QCOMPARE(appearance->property("darkMode").toBool(), dark);
        QQuickItem *root = window->contentItem();
        // The conversation scrolls to its end once laid out; measure after that.
        QTest::qWait(400);

        const auto item = [window](const char *name) {
            return qobject_cast<QQuickItem *>(window->findChild<QObject *>(QLatin1String(name)));
        };
        auto *name = item("localUserName");
        auto *beadButton = item("localPresenceButton");
        auto *avatar = item("localUserAvatar");
        QVERIFY(name && beadButton && avatar);
        QQuickItem *header = avatar->parentItem();
        QQuickItem *search = item("contactSearch");
        QVERIFY(search);
        QQuickItem *searchBox = search->parentItem();
        const QRectF nameBefore(name->x(), name->y(), name->width(), name->height());
        const QPointF beadBefore(beadButton->x(), beadButton->y());
        const QRectF avatarBefore(avatar->x(), avatar->y(), avatar->width(), avatar->height());
        const QList<QQuickItem *> bubbles = outgoingBubbles(root);
        QVERIFY2(bubbles.size() >= 2, "the preview conversation shows my own messages");
        QList<QRectF> bubblesBefore;
        for (QQuickItem *bubble : bubbles)
            bubblesBefore.append(bubble->mapRectToScene(QRectF(0, 0, bubble->width(), bubble->height())));

        const QImage baseline = settledGrab(window);
        QTest::qWait(560);
        const QRegion unstable = unstablePixels(baseline, settledGrab(window));
        const qreal dpr = baseline.devicePixelRatio();

        // Equip it at runtime, as a picker would.
        QVERIFY(appearance->setProperty(property.constData(), id));
        QCOMPARE(appearance->property(property.constData()).toString(), id);
        QCoreApplication::processEvents();

        // It lands on its component, and only there.
        QList<QRect> allowed;
        QList<QQuickItem *> targets;
        if (info->category == QLatin1String("bubble")) {
            for (QQuickItem *bubble : bubbles) {
                QCOMPARE(bubble->property("skin").toString(), id);
                QVERIFY(bubble->property("skinned").toBool());
                allowed.append(deviceRect(bubble, dpr, 1));
                targets.append(bubble);
            }
            const auto everyBubble = window->findChildren<QQuickItem *>(QStringLiteral("messageBubble"));
            for (QQuickItem *bubble : everyBubble) {
                if (!bubble->property("outgoing").toBool())
                    QCOMPARE(bubble->property("skin").toString(), QString());
            }
        } else {
            const char *objectName = info->category == QLatin1String("frame") ? "avatarFrame"
                : info->category == QLatin1String("bead")                     ? "beadArt"
                : info->category == QLatin1String("flair")                    ? "localNameFlair"
                                                                              : "profileScene";
            const char *idProperty = info->category == QLatin1String("frame") ? "frameId"
                : info->category == QLatin1String("bead")                     ? "styleId"
                : info->category == QLatin1String("flair")                    ? "flairId"
                                                                              : "sceneId";
            QQuickItem *target = nullptr;
            QTRY_VERIFY((target = item(objectName)));
            QCOMPARE(window->findChildren<QObject *>(QLatin1String(objectName)).size(), 1);
            QCOMPARE(target->property(idProperty).toString(), id);
            // It belongs to the local header block; a scene alone may fade on
            // into the gap above the search field, behind it.
            const QRectF block = header->mapRectToScene(QRectF(0, 0, header->width(), header->height()))
                                     .adjusted(-1, -1, 1, info->category == QLatin1String("scene") ? 13 : 1);
            const QRectF own = target->mapRectToScene(QRectF(0, 0, target->width(), target->height()));
            QVERIFY2(block.contains(own),
                     qPrintable(QStringLiteral("%1 reaches outside the header: %2,%3 %4x%5")
                                    .arg(id).arg(own.x()).arg(own.y()).arg(own.width()).arg(own.height())));
            // Whatever it is, the search box itself is never painted over
            // (only its rounded corners' cut-outs, which are see-through).
            const QRect box = deviceRect(searchBox, dpr);
            const int corner = qCeil(searchBox->property("radius").toReal() * dpr);
            QRegion region(deviceRect(target, dpr, 1));
            region -= QRegion(box.adjusted(corner, 0, -corner, 0));
            region -= QRegion(box.adjusted(0, corner, 0, -corner));
            for (const QRect &rect : region)
                allowed.append(rect);
            targets.append(target);
        }

        const QImage equipped = settledGrab(window);
        // For review: the equipped surface with some surroundings, and the stock one.
        if (const QString dir = qEnvironmentVariable("OPENCHAT_COSMETIC_CAPTURES"); !dir.isEmpty()) {
            QRect crop;
            for (const QRect &rect : allowed)
                crop |= rect;
            crop = crop.adjusted(-int(24 * dpr), -int(16 * dpr), int(24 * dpr), int(16 * dpr))
                       .intersected(equipped.rect());
            equipped.copy(crop).save(QStringLiteral("%1/%2-%3.png").arg(dir, id, dark ? "dark" : "light"));
            baseline.copy(crop).save(QStringLiteral("%1/%2-%3-stock.png").arg(dir, id, dark ? "dark" : "light"));
        }
        const GrabDiff change = diffGrabs(baseline, equipped, allowed, unstable);
        QVERIFY2(change.outside == 0,
                 qPrintable(QStringLiteral("%1 changed %2 px outside its component, within %3,%4 %5x%6")
                                .arg(id).arg(change.outside).arg(change.outsideBox.x())
                                .arg(change.outsideBox.y()).arg(change.outsideBox.width())
                                .arg(change.outsideBox.height())));
        const int minimum = info->category == QLatin1String("bead") ? int(20 * dpr * dpr) : int(150 * dpr * dpr);
        QVERIFY2(change.inside >= minimum,
                 qPrintable(QStringLiteral("%1 changed only %2 px").arg(id).arg(change.inside)));
        // Every one of my bubbles wears it, not just some.
        if (info->category == QLatin1String("bubble")) {
            for (int i = 0; i < change.perRect.size(); ++i)
                QVERIFY2(change.perRect[i] > int(100 * dpr * dpr),
                         qPrintable(QStringLiteral("%1 left bubble %2 unchanged").arg(id).arg(i)));
        }

        // Nothing moved.
        QCOMPARE(QRectF(name->x(), name->y(), name->width(), name->height()), nameBefore);
        QCOMPARE(QPointF(beadButton->x(), beadButton->y()), beadBefore);
        QCOMPARE(QRectF(avatar->x(), avatar->y(), avatar->width(), avatar->height()), avatarBefore);
        for (int i = 0; i < bubbles.size(); ++i)
            QCOMPARE(bubbles[i]->mapRectToScene(QRectF(0, 0, bubbles[i]->width(), bubbles[i]->height())),
                     bubblesBefore[i]);

        // Only the animated items move on their own.
        if (info->category == QLatin1String("frame")) {
            QQuickItem *frame = targets.constFirst();
            const int phase = frame->property("phase").toInt();
            QTest::qWait(450);
            if (info->animated)
                QVERIFY2(frame->property("phase").toInt() != phase, qPrintable(id + " stood still"));
            else
                QCOMPARE(frame->property("phase").toInt(), phase);
        }

        // Remembered, and worn again after a restart.
        QCOMPARE(QSettings().value(QStringLiteral("Appearance/") + QString::fromLatin1(property)).toString(), id);
        if (!dark) {
            OpenChat::ChatController again;
            QQmlApplicationEngine restarted;
            restarted.setInitialProperties({{QStringLiteral("chatController"), QVariant::fromValue(&again)}});
            restarted.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
            restarted.loadFromModule("OpenChat", "Main");
            QCOMPARE(restarted.rootObjects().size(), 1);
            QObject *reloaded = restarted.singletonInstance<QObject *>("OpenChat.Native", "AppearanceSettings");
            QCOMPARE(reloaded->property(property.constData()).toString(), id);
        }
        // The second window took the focus (and with it the composer's caret).
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));

        // Unequipping leaves exactly the stock pixels.
        QVERIFY(appearance->setProperty(property.constData(), QString()));
        const QImage cleared = settledGrab(window);
        const GrabDiff residue = diffGrabs(baseline, cleared, {}, unstable);
        QVERIFY2(residue.outside == 0,
                 qPrintable(QStringLiteral("%1 left %2 px behind, within %3,%4 %5x%6")
                                .arg(id).arg(residue.outside).arg(residue.outsideBox.x())
                                .arg(residue.outsideBox.y()).arg(residue.outsideBox.width())
                                .arg(residue.outsideBox.height())));
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.isEmpty() ? QString()
            : warnings.constFirst().constFirst().value<QList<QQmlError>>().value(0).toString()));
        clearAllCosmetics();
    }

    // Every frame also wears the local call tile, and only it: the remote
    // tile keeps its stock picture and the caption stays clear of the frame.
    void everyFrameWearsTheLocalCallTile_data()
    {
        QTest::addColumn<QString>("id");
        for (const OpenChat::CosmeticInfo &info : OpenChat::CosmeticCatalog::inCategory(QStringLiteral("frame")))
            QTest::newRow(qPrintable(info.id)) << info.id;
    }
    void everyFrameWearsTheLocalCallTile()
    {
        QFETCH(QString, id);
        clearAllCosmetics();
        const auto grabTiles = [](const QString &frame, QImage *localTile, QImage *remoteTile,
                                  bool *captionClear, QString *equippedId, int *remoteFrames) {
            QSettings().setValue(QStringLiteral("Appearance/avatarFrame"), frame);
            OpenChat::ChatController chats;
            OpenChat::CallController calls;
            calls.enableForPreview(OpenChat::CallState::Active, QStringLiteral("Jessica"),
                                   QStringLiteral("jessica"), false, false);
            QQmlApplicationEngine engine;
            engine.setInitialProperties({{QStringLiteral("chatController"), QVariant::fromValue(&chats)},
                                         {QStringLiteral("callController"), QVariant::fromValue(&calls)}});
            engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
            engine.loadFromModule("OpenChat", "Main");
            auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
            if (!window || !QTest::qWaitForWindowExposed(window))
                return false;
            window->resize(860, 680);
            auto *local = window->findChild<QQuickItem *>(QStringLiteral("localParticipant"));
            auto *remote = window->findChild<QQuickItem *>(QStringLiteral("remoteParticipant"));
            if (!local || !remote)
                return false;
            const QImage shot = settledGrab(window);
            const qreal dpr = shot.devicePixelRatio();
            *localTile = shot.copy(deviceRect(local, dpr));
            *remoteTile = shot.copy(deviceRect(remote, dpr));
            auto *frameItem = local->findChild<QQuickItem *>(QStringLiteral("avatarFrame"));
            *equippedId = frameItem ? frameItem->property("frameId").toString() : QString();
            *remoteFrames = remote->findChildren<QQuickItem *>(QStringLiteral("avatarFrame")).size();
            *captionClear = true;
            if (frameItem) {
                const QRectF frameRect = frameItem->mapRectToScene(
                    QRectF(0, 0, frameItem->width(), frameItem->height()));
                for (QQuickItem *text : local->findChildren<QQuickItem *>()) {
                    if (qstrcmp(text->metaObject()->className(), "QQuickText") == 0 && text->isVisible()
                        && !text->property("text").toString().isEmpty()) {
                        const QRectF textRect = text->mapRectToScene(QRectF(0, 0, text->width(), text->height()));
                        if (textRect.intersects(frameRect.adjusted(2, 2, -2, -2)))
                            *captionClear = false;
                    }
                }
            }
            return true;
        };
        QImage stockLocal, stockRemote, framedLocal, framedRemote;
        bool stockClear = false, framedClear = false;
        QString stockId, framedId;
        int stockRemoteFrames = -1, framedRemoteFrames = -1;
        QVERIFY(grabTiles(QString(), &stockLocal, &stockRemote, &stockClear, &stockId, &stockRemoteFrames));
        QVERIFY(grabTiles(id, &framedLocal, &framedRemote, &framedClear, &framedId, &framedRemoteFrames));
        QCOMPARE(stockId, QString());
        QCOMPARE(framedId, id);
        QCOMPARE(framedRemoteFrames, 0);
        QVERIFY2(framedClear, qPrintable(id + " overlaps the call tile's caption"));
        // The remote tile is untouched (it may sit lower when the header grows).
        QCOMPARE(framedRemote.size(), stockRemote.size());
        QVERIFY2(diffGrabs(stockRemote, framedRemote, {}).outside == 0, qPrintable(id + " changed the remote tile"));
        // The local tile visibly wears it.
        QVERIFY(framedLocal.height() >= stockLocal.height());
        const QImage stockTop = stockLocal.copy(0, 0, stockLocal.width(), stockLocal.height());
        const QImage framedTop = framedLocal.copy(0, 0, stockLocal.width(), stockLocal.height());
        QVERIFY(diffGrabs(stockTop, framedTop, {}).outside > 200);
        clearAllCosmetics();
    }

    // Every collectible draws its preview in a case tile (the reel's and a
    // narrow one), inside the preview slot, with its name and tier.
    void everyCosmeticPreviewsInItsCaseTile_data() { addEveryCosmetic(); }
    void everyCosmeticPreviewsInItsCaseTile()
    {
        QFETCH(QString, id);
        QFETCH(bool, dark);
        QSettings().setValue(QStringLiteral("Appearance/darkMode"), dark);
        QQmlEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        QQmlComponent component(&engine);
        component.setData(QByteArrayLiteral(R"(
            import QtQuick
            import OpenChat
            import OpenChat.Native
            Window {
                id: window
                property string itemId
                readonly property var entry: Cosmetics.item(itemId)
                // The same entry with no category: the tile without its preview.
                readonly property var bare: Object.assign({}, entry, { category: "" })
                width: 320; height: 150; visible: true
                color: Theme.contentBackground
                Row {
                    x: 10; y: 10; spacing: 10
                    CaseTile { objectName: "wide"; width: 104; height: 128; item: window.entry }
                    CaseTile { objectName: "wideBare"; width: 104; height: 128; item: window.bare }
                    CaseTile { objectName: "narrow"; width: 82; height: 128; item: window.entry }
                }
            })"), QUrl());
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.createWithInitialProperties({{"itemId", id}}));
        QVERIFY2(object, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window && QTest::qWaitForWindowExposed(window));
        const QImage shot = settledGrab(window);
        const qreal dpr = shot.devicePixelRatio();
        auto *wide = window->findChild<QQuickItem *>(QStringLiteral("wide"));
        auto *bare = window->findChild<QQuickItem *>(QStringLiteral("wideBare"));
        auto *narrow = window->findChild<QQuickItem *>(QStringLiteral("narrow"));
        QVERIFY(wide && bare && narrow);
        const QImage withPreview = shot.copy(deviceRect(wide, dpr));
        const QImage withoutPreview = shot.copy(deviceRect(bare, dpr));
        // The preview slot: 6 px in, from y 12, 60 px tall.
        const QRect slot = QRect(QPoint(int(6 * dpr), int(12 * dpr)),
                                 QSize(int((104 - 12) * dpr), int(60 * dpr)));
        const GrabDiff preview = diffGrabs(withoutPreview, withPreview, {slot});
        QVERIFY2(preview.outside == 0, qPrintable(id + " draws outside its preview slot"));
        QVERIFY2(preview.inside >= int(40 * dpr * dpr),
                 qPrintable(QStringLiteral("%1 preview drew only %2 px").arg(id).arg(preview.inside)));
        const QImage narrowShot = shot.copy(deviceRect(narrow, dpr));
        int inked = 0;
        const QRgb paper = narrowShot.pixel(narrowShot.width() / 2, int(4 * dpr));
        for (int y = int(12 * dpr); y < int(72 * dpr); ++y)
            for (int x = int(6 * dpr); x < narrowShot.width() - int(6 * dpr); ++x)
                inked += pixelsDiffer(narrowShot.pixel(x, y), paper) ? 1 : 0;
        QVERIFY2(inked >= int(40 * dpr * dpr), qPrintable(id + " vanishes in a narrow tile"));

        const OpenChat::CosmeticInfo *info = OpenChat::CosmeticCatalog::find(id);
        QStringList texts;
        for (QQuickItem *child : wide->childItems()) {
            if (child->isVisible() && qstrcmp(child->metaObject()->className(), "QQuickText") == 0)
                texts << child->property("text").toString();
        }
        QVERIFY2(texts.contains(info->name), qPrintable(texts.join(QLatin1Char('|'))));
        QVERIFY(texts.contains(OpenChat::CosmeticCatalog::rarityName(info->rarity)));
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.isEmpty() ? QString()
            : warnings.constFirst().constFirst().value<QList<QQmlError>>().value(0).toString()));
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

        auto *input = qobject_cast<QQuickItem *>(
            root->findChild<QObject *>(QStringLiteral("messageInput")));
        QVERIFY(input);
        // There is no Send button: Enter sends.
        QVERIFY(!root->findChild<QObject *>(QStringLiteral("sendButton")));
        QVERIFY(!controller.canSend());

        QQuickWindow *window = showActiveWindow(root);
        if (!window)
            QSKIP("No active window on this platform to type into");
        // The open chat's composer already has the keyboard.
        QTRY_VERIFY(input->hasActiveFocus());
        const int before = controller.messages()->rowCount();
        // Enter with nothing to send neither sends nor starts a new line.
        QTest::keyClick(window, Qt::Key_Return);
        QCOMPARE(controller.messages()->rowCount(), before);
        QCOMPARE(input->property("text").toString(), QString());
        QVERIFY(input->setProperty("text", QStringLiteral("Hello")));
        QCoreApplication::processEvents();
        QVERIFY(controller.canSend());
        QTest::keyClick(window, Qt::Key_Return);
        QCoreApplication::processEvents();
        QCOMPARE(controller.messages()->rowCount(), before + 1);
        QCOMPARE(controller.composerText(), QString());
        // The keypad's Enter sends too.
        QVERIFY(input->setProperty("text", QStringLiteral("Again")));
        QTest::keyClick(window, Qt::Key_Enter, Qt::KeypadModifier);
        QCOMPARE(controller.messages()->rowCount(), before + 2);

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

        // Badge labels are backed by the controller's counts, not hardcoded, and
        // a badge shows only while its count is above zero.
        QObject *chatBadge = root->findChild<QObject *>(QStringLiteral("chatBadgeLabel"));
        QObject *callBadge = root->findChild<QObject *>(QStringLiteral("callBadge"));
        QObject *callBadgeLabel = root->findChild<QObject *>(QStringLiteral("callBadgeLabel"));
        QVERIFY(chatBadge);
        QVERIFY(callBadge);
        QVERIFY(callBadgeLabel);
        QCOMPARE(chatBadge->property("text").toString(),
                 QString::number(controller.chatUnreadCount()));
        QCOMPARE(chatBadge->property("text").toString(), QStringLiteral("3"));
        QVERIFY(!callBadge->property("visible").toBool());
        controller.noteMissedCall();
        QVERIFY(callBadge->property("visible").toBool());
        QCOMPARE(callBadgeLabel->property("text").toString(), QStringLiteral("1"));

        // Call: the call placeholder replaces the chat pane exclusively, and the
        // sidebar swaps the contact list for the call history list with its
        // empty state showing.
        controller.setNavSection(OpenChat::ChatController::NavSection::Call);
        QCoreApplication::processEvents();
        QVERIFY(!callBadge->property("visible").toBool()); // seen, so cleared
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

    void theMicrophonePanelDrivesAndRemembersTheSettings()
    {
        OpenChat::ChatController chats;
        chats.setLocalUserName(QStringLiteral("Developer"));
        chats.setNavSection(OpenChat::ChatController::NavSection::Settings);
        chats.setCurrentSettingsCategory(4); // Audio & Video
        OpenChat::ContactController contacts;
        contacts.enableForPreview();
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

        auto *settings = engine.singletonInstance<OpenChat::MicrophoneSettings *>(
            "OpenChat.Native", "MicrophoneSettings");
        QVERIFY(settings);
        // The defaults a fresh install runs with: follow the system device,
        // unity gain, and the gate on at its stock threshold.
        QVERIFY(settings->inputDeviceId().isEmpty());
        QCOMPARE(settings->gain(), 1.0);
        QVERIFY(settings->noiseGateEnabled());
        QVERIFY(settings->processing().gateEnabled);
        QVERIFY(qAbs(settings->processing().gateThreshold - 0.02) < 0.001);

        // The panel replaces the Microphone stub row, and only that row.
        auto *panel = findVisualItem(window->contentItem(), QStringLiteral("microphoneSettingsPanel"));
        QVERIFY(panel && panel->isVisible());
        auto *systemDefault = findVisualItem(window->contentItem(), QStringLiteral("microphoneDevice_0"));
        QVERIFY(systemDefault && systemDefault->isVisible());
        QVERIFY(systemDefault->property("selected").toBool());

        // The gate switch writes the setting, persists it, and hides the
        // threshold slider that no longer means anything.
        auto *gateSwitch = findVisualItem(window->contentItem(), QStringLiteral("noiseGateSwitch"));
        auto *threshold = findVisualItem(window->contentItem(), QStringLiteral("noiseGateThresholdSlider"));
        QVERIFY(gateSwitch && gateSwitch->isVisible());
        QVERIFY(threshold && threshold->isVisible());
        QSignalSpy processing(settings, &OpenChat::MicrophoneSettings::processingChanged);
        const QPoint switchCentre = gateSwitch->mapToScene(
            QPointF(gateSwitch->width() / 2, gateSwitch->height() / 2)).toPoint();
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, switchCentre);
        QTRY_VERIFY(!settings->noiseGateEnabled());
        QCOMPARE(processing.count(), 1);
        QVERIFY(!settings->processing().gateEnabled);
        QCOMPARE(QSettings().value(QStringLiteral("Audio/noiseGate")).toBool(), false);
        QTRY_VERIFY(!threshold->isVisible());
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, switchCentre);
        QTRY_VERIFY(settings->noiseGateEnabled());
        QTRY_VERIFY(threshold->isVisible());

        // The volume slider: its right end is 200%, its middle unity, and the
        // label follows.
        auto *gainSlider = findVisualItem(window->contentItem(), QStringLiteral("microphoneGainSlider"));
        auto *gainLabel = findVisualItem(window->contentItem(), QStringLiteral("microphoneGainLabel"));
        QVERIFY(gainSlider && gainSlider->isVisible() && gainLabel);
        QCOMPARE(gainLabel->property("text").toString(), QStringLiteral("100%"));
        const QPoint rightEnd = gainSlider->mapToScene(
            QPointF(gainSlider->width() - 1, gainSlider->height() / 2)).toPoint();
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, rightEnd);
        QTRY_COMPARE(settings->gain(), 2.0);
        QCOMPARE(gainLabel->property("text").toString(), QStringLiteral("200%"));
        QCOMPARE(settings->processing().gain, 2.0);
        QCOMPARE(QSettings().value(QStringLiteral("Audio/inputGain")).toDouble(), 2.0);

        // The threshold slider works in decibels across the documented range.
        // Its row was just re-shown; let the column lay out before aiming.
        QTest::qWait(50);
        const QPoint thresholdLeft = threshold->mapToScene(
            QPointF(1, threshold->height() / 2)).toPoint();
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, thresholdLeft);
        QTRY_COMPARE(settings->noiseGateThresholdDb(), settings->minThresholdDb());
        QVERIFY(settings->processing().gateThreshold < 0.002);

        // A second engine, as the next window would use, sees what was saved.
        settings->setGain(0.5);
        {
            QQmlEngine nextEngine;
            nextEngine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
            QQmlComponent component(&nextEngine);
            component.setData("import QtQuick; import OpenChat.Native; "
                              "QtObject { property real gain: MicrophoneSettings.gain; "
                              "property bool gate: MicrophoneSettings.noiseGateEnabled; "
                              "property real db: MicrophoneSettings.noiseGateThresholdDb }",
                              QUrl());
            std::unique_ptr<QObject> restored(component.create());
            QVERIFY2(restored, qPrintable(component.errorString()));
            QCOMPARE(restored->property("gain").toDouble(), 0.5);
            QVERIFY(restored->property("gate").toBool());
            QCOMPARE(restored->property("db").toDouble(), settings->minThresholdDb());
        }
        settings->resetToDefaults();
        QCOMPARE(settings->gain(), 1.0);
        QVERIFY(qAbs(settings->processing().gateThreshold - 0.02) < 0.001);
    }

    void theConnectionPanelDrivesAndRemembersTheSettings()
    {
        OpenChat::ChatController chats;
        chats.setLocalUserName(QStringLiteral("Developer"));
        chats.setNavSection(OpenChat::ChatController::NavSection::Settings);
        chats.setCurrentSettingsCategory(4); // Audio & Video
        // Settings are pages now: open the Connection page, as a click would.
        const int page = chats.currentSettingsElements().indexOf(QStringLiteral("Connection"));
        QVERIFY(page >= 0);
        chats.setCurrentSettingsSubcategory(page);
        OpenChat::ContactController contacts;
        contacts.enableForPreview();
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

        auto *settings = engine.singletonInstance<OpenChat::TransportSettings *>(
            "OpenChat.Native", "TransportSettings");
        QVERIFY(settings);
        QCOMPARE(settings->mode(), QStringLiteral("auto"));

        auto *panel = findVisualItem(window->contentItem(), QStringLiteral("connectionSettingsPanel"));
        QVERIFY(panel && panel->isVisible());
        auto *autoRow = findVisualItem(window->contentItem(), QStringLiteral("connectionMode_auto"));
        QVERIFY(autoRow && autoRow->isVisible());
        QVERIFY(autoRow->property("selected").toBool());

        auto *udpRow = findVisualItem(window->contentItem(), QStringLiteral("connectionMode_udp"));
        QVERIFY(udpRow && udpRow->isVisible());
        QVERIFY(!udpRow->property("selected").toBool());

        // Select UDP
        QMetaObject::invokeMethod(udpRow, "activate");
        QCOMPARE(settings->mode(), QStringLiteral("udp"));
        QVERIFY(udpRow->property("selected").toBool());
        QVERIFY(!autoRow->property("selected").toBool());

        // Verify across engine reboot
        {
            QQmlApplicationEngine nextEngine;
            nextEngine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
            QQmlComponent component(&nextEngine);
            component.setData("import QtQuick; import OpenChat.Native; "
                              "QtObject { property string mode: TransportSettings.mode }",
                              QUrl());
            std::unique_ptr<QObject> restored(component.create());
            QVERIFY2(restored, qPrintable(component.errorString()));
            QCOMPARE(restored->property("mode").toString(), QStringLiteral("udp"));
        }

        // Reset back to auto
        settings->setMode(QStringLiteral("auto"));
        QCOMPARE(settings->mode(), QStringLiteral("auto"));
    }

    void voiceDebugWindowLoadsAndBindsController()
    {
        OpenChat::VoiceDebugController controller;
        controller.enableForPreview();

        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{QStringLiteral("debugController"), QVariant::fromValue(&controller)}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "VoiceDebugWindow");
        QCOMPARE(engine.rootObjects().size(), 1);
        QObject *root = engine.rootObjects().constFirst();
        QVERIFY(root);
        QCOMPARE(root->objectName(), QStringLiteral("voiceDebugWindow"));
        QCOMPARE(root->property("title").toString(),
                 QStringLiteral("[VOICE DEBUG OVERLAY] OpenChat Latency, Jitter & Transport Diagnostics"));

        // Simulate a lag spike and verify properties update
        controller.simulateSpike(210.5);
        QVERIFY(controller.lagSpikeCount() > 0);
        QVERIFY(controller.isSpikeActive());
        QCOMPARE(controller.currentRtt(), 210.5);
    }

    void lowMemoryModeIsASwitchUnderGeneralThatAsksForARestart()
    {
        OpenChat::ChatController chats;
        chats.setNavSection(OpenChat::ChatController::NavSection::Settings);
        chats.setCurrentSettingsCategory(0);
        const int page = chats.currentSettingsElements().indexOf(QStringLiteral("Low memory mode"));
        QVERIFY(page >= 0);
        chats.setCurrentSettingsSubcategory(page);
        OpenChat::CallController calls;
        QQmlApplicationEngine engine;
        engine.setInitialProperties({{QStringLiteral("chatController"), QVariant::fromValue(&chats)},
                                     {QStringLiteral("callController"), QVariant::fromValue(&calls)}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");
        QCOMPARE(engine.rootObjects().size(), 1);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));

        auto *memory = engine.singletonInstance<OpenChat::MemorySettings *>("OpenChat.Native",
                                                                           "MemorySettings");
        QVERIFY(memory);
        QVERIFY(!memory->lowMemoryMode());
        auto *toggle = findVisualItem(window->contentItem(), QStringLiteral("lowMemorySwitch"));
        auto *restart = findVisualItem(window->contentItem(), QStringLiteral("lowMemoryRestart"));
        auto *restartButton =
            findVisualItem(window->contentItem(), QStringLiteral("lowMemoryRestartButton"));
        QVERIFY(toggle && toggle->isVisible());
        QVERIFY(restart && restartButton);
        // Nothing to finish while the running process has the saved choice.
        QVERIFY(!restart->isVisible());

        const QPoint switchCentre = toggle->mapToScene(QPointF(toggle->width() / 2,
                                                               toggle->height() / 2)).toPoint();
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, switchCentre);
        QTRY_VERIFY(memory->lowMemoryMode());
        QVERIFY(toggle->property("checked").toBool());
        QVERIFY(QSettings().value(QStringLiteral("Performance/lowMemoryMode")).toBool());
        // Drawing without the graphics card waits for a restart, which the page
        // offers, but not in the middle of a call.
        QVERIFY(memory->restartPending());
        QVERIFY(restart->isVisible());
        QVERIFY(restartButton->isEnabled());
        calls.enableForPreview(OpenChat::CallState::Active, QStringLiteral("Jessica"),
                               QStringLiteral("jessica"), false, false);
        QCoreApplication::processEvents();
        QVERIFY(!restartButton->isEnabled());
        calls.enableForPreview(OpenChat::CallState::Idle, QString(), QString(), false, false);
        QCoreApplication::processEvents();
        QVERIFY(restartButton->isEnabled());

        // Switching back before restarting leaves nothing to finish.
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, switchCentre);
        QTRY_VERIFY(!memory->lowMemoryMode());
        QVERIFY(!memory->restartPending());
        QVERIFY(!restart->isVisible());
        QVERIFY(!QSettings().value(QStringLiteral("Performance/lowMemoryMode")).toBool());
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
        QCOMPARE(singleLineHeight, 40.0);
        QCOMPARE(attachment->property("height").toReal(), singleLineHeight);
        QCOMPARE(attachment->property("y").toReal(), 0.0);

        // Holding one line, the composer lines up with the sidebar's navigation
        // bar beside it, and the field spans it with even margins either side.
        auto *composerItem = qobject_cast<QQuickItem *>(composer);
        auto *bottomNav = qobject_cast<QQuickItem *>(
            root->findChild<QObject *>(QStringLiteral("bottomNav")));
        auto *frameItem = qobject_cast<QQuickItem *>(frame);
        QVERIFY(composerItem && bottomNav && frameItem);
        QCOMPARE(composerItem->height(), bottomNav->height());
        QCOMPARE(composerItem->mapToScene(QPointF()).y(), bottomNav->mapToScene(QPointF()).y());
        QCOMPARE(composerItem->width() - (frameItem->x() + frameItem->width()), frameItem->x());

        QVERIFY(input->setProperty("text", QStringLiteral("First line\nSecond line\nThird line")));
        QCoreApplication::processEvents();
        const qreal multilineHeight = frame->property("height").toReal();
        QVERIFY(multilineHeight > singleLineHeight);
        QCOMPARE(attachment->property("height").toReal(), multilineHeight);
        QCOMPARE(composer->property("height").toReal(), multilineHeight + 24.0);

        // Far more lines than fit: the field stops at its cap and the text
        // scrolls inside it, never reaching past the frame.
        QStringList lines;
        for (int i = 0; i < 80; ++i)
            lines << QStringLiteral("Line %1").arg(i);
        QVERIFY(input->setProperty("text", lines.join(QLatin1Char('\n'))));
        QCoreApplication::processEvents();
        QCOMPARE(frame->property("height").toReal(), composer->property("maxInputHeight").toReal());
        QObject *scroll = root->findChild<QObject *>(QStringLiteral("messageInputScroll"));
        QObject *scrollBar = root->findChild<QObject *>(QStringLiteral("messageInputScrollBar"));
        QVERIFY(scroll && scrollBar);
        const qreal viewY = scroll->property("y").toReal();
        const qreal viewHeight = scroll->property("height").toReal();
        const qreal contentHeight = scroll->property("contentHeight").toReal();
        QVERIFY(scroll->property("clip").toBool());
        QVERIFY(contentHeight > viewHeight);
        QVERIFY(viewY >= 0.0);
        QVERIFY(viewY + viewHeight <= frameItem->height());
        QVERIFY(scrollBar->property("size").toReal() < 1.0);
        // It follows the cursor to the end and back to the start.
        QVERIFY(input->setProperty("cursorPosition", input->property("length")));
        QTRY_COMPARE(scroll->property("contentY").toReal(), contentHeight - viewHeight);
        QVERIFY(input->setProperty("cursorPosition", 0));
        QTRY_COMPARE(scroll->property("contentY").toReal(), 0.0);

        QVERIFY(input->setProperty("text", QString()));
        QCoreApplication::processEvents();
        QCOMPARE(frame->property("height").toReal(), singleLineHeight);
    }

    void composerScrollsUnderTheWheelAndSelectsByDragging()
    {
        OpenChat::ChatController controller;
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{QStringLiteral("chatController"), QVariant::fromValue(&controller)}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");
        QCOMPARE(engine.rootObjects().size(), 1);
        QQuickWindow *window = showActiveWindow(engine.rootObjects().constFirst());
        if (!window)
            QSKIP("No active window on this platform to scroll in");
        auto *input = qobject_cast<QQuickItem *>(
            window->findChild<QObject *>(QStringLiteral("messageInput")));
        QObject *scroll = window->findChild<QObject *>(QStringLiteral("messageInputScroll"));
        QVERIFY(input && scroll);
        QStringList lines;
        for (int i = 0; i < 80; ++i)
            lines << QStringLiteral("Line %1").arg(i);
        QVERIFY(input->setProperty("text", lines.join(QLatin1Char('\n'))));
        QVERIFY(input->setProperty("cursorPosition", 0));
        QTRY_COMPARE(scroll->property("contentY").toReal(), 0.0);

        // A notch of the wheel over the text moves it down, and back up.
        const QPointF over = input->mapToScene(QPointF(input->width() / 2, 30));
        // One event per notch, as a wheel sends them, each later than the last:
        // Flickable ignores a wheel event that is not newer than the one before.
        ulong timestamp = 1'000'000;
        const auto wheel = [&](int notches) {
            for (int i = 0; i < std::abs(notches); ++i) {
                QWheelEvent event(over, window->mapToGlobal(over), QPoint(),
                                  QPoint(0, notches < 0 ? -120 : 120), Qt::NoButton,
                                  Qt::NoModifier, Qt::NoScrollPhase, false);
                timestamp += 50;
                event.setTimestamp(timestamp);
                QGuiApplication::sendEvent(window, &event);
                QTest::qWait(20);
            }
        };
        wheel(-1);
        QTRY_VERIFY(scroll->property("contentY").toReal() > 0.0);
        wheel(10);
        QTRY_COMPARE(scroll->property("contentY").toReal(), 0.0);

        // Dragging across the text selects it rather than scrolling it.
        const QPoint from = input->mapToScene(QPointF(4, 10)).toPoint();
        const QPoint to = input->mapToScene(QPointF(24, 140)).toPoint();
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, from);
        for (int step = 1; step <= 10; ++step)
            QTest::mouseMove(window, from + (to - from) * step / 10);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, to);
        QVERIFY(!input->property("selectedText").toString().isEmpty());
        QCOMPARE(scroll->property("contentY").toReal(), 0.0);
    }

    void composerEditsLikeACodeEditor()
    {
        OpenChat::ChatController controller;
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{QStringLiteral("chatController"), QVariant::fromValue(&controller)}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");
        QCOMPARE(engine.rootObjects().size(), 1);
        QQuickWindow *window = showActiveWindow(engine.rootObjects().constFirst());
        if (!window)
            QSKIP("No active window on this platform to type into");
        auto *input = qobject_cast<QQuickItem *>(
            window->findChild<QObject *>(QStringLiteral("messageInput")));
        QVERIFY(input);
        QTRY_VERIFY(input->hasActiveFocus());
        const auto text = [&] { return input->property("text").toString(); };
        const auto cursor = [&] { return input->property("cursorPosition").toInt(); };
        const auto start = [&](const QString &value, int position) {
            input->setProperty("text", value);
            input->setProperty("cursorPosition", position);
        };
        const auto press = [&](Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
            QTest::keyClick(window, key, modifiers);
        };
        const Qt::KeyboardModifiers ctrl = Qt::ControlModifier;
        const Qt::KeyboardModifiers shift = Qt::ShiftModifier;
        const Qt::KeyboardModifiers alt = Qt::AltModifier;
        const int sent = controller.messages()->rowCount();

        // Shift+Enter breaks the line and keeps its indentation; nothing is sent.
        start(QStringLiteral("    first"), 9);
        press(Qt::Key_Return, shift);
        QCOMPARE(text(), QStringLiteral("    first\n    "));
        QCOMPARE(cursor(), 14);
        QCOMPARE(controller.messages()->rowCount(), sent);

        // Ctrl+Enter opens an indented line below wherever the cursor is in
        // its line; Ctrl+Shift+Enter one above.
        start(QStringLiteral("  one\ntwo"), 3);
        press(Qt::Key_Return, ctrl);
        QCOMPARE(text(), QStringLiteral("  one\n  \ntwo"));
        QCOMPARE(cursor(), 8);
        start(QStringLiteral("  one\ntwo"), 7);
        press(Qt::Key_Return, ctrl | shift);
        QCOMPARE(text(), QStringLiteral("  one\n\ntwo"));
        QCOMPARE(cursor(), 6);

        // Tab fills to the next tab stop; across lines it indents them all.
        // Shift+Tab takes a level back out.
        start(QStringLiteral("ab"), 2);
        press(Qt::Key_Tab);
        QCOMPARE(text(), QStringLiteral("ab  "));
        start(QStringLiteral("one\ntwo"), 0);
        input->setProperty("cursorPosition", 0);
        QVERIFY(QMetaObject::invokeMethod(input, "select", Q_ARG(int, 0), Q_ARG(int, 7)));
        press(Qt::Key_Tab);
        QCOMPARE(text(), QStringLiteral("    one\n    two"));
        press(Qt::Key_Backtab, shift);
        QCOMPARE(text(), QStringLiteral("one\ntwo"));

        // Ctrl+] and Ctrl+[ indent and outdent the line the cursor is on.
        start(QStringLiteral("x"), 1);
        press(Qt::Key_BracketRight, ctrl);
        QCOMPARE(text(), QStringLiteral("    x"));
        QCOMPARE(cursor(), 5);
        press(Qt::Key_BracketLeft, ctrl);
        QCOMPARE(text(), QStringLiteral("x"));
        QCOMPARE(cursor(), 1);

        // Alt+Down and Alt+Up move the line, the cursor with it; a move is a
        // single step to undo, and Ctrl+Y or Ctrl+Shift+Z redo it.
        start(QStringLiteral("one\ntwo\nthree"), 1);
        press(Qt::Key_Down, alt);
        QCOMPARE(text(), QStringLiteral("two\none\nthree"));
        QCOMPARE(cursor(), 5);
        press(Qt::Key_Down, alt);
        QCOMPARE(text(), QStringLiteral("two\nthree\none"));
        press(Qt::Key_Up, alt);
        QCOMPARE(text(), QStringLiteral("two\none\nthree"));
        QCOMPARE(cursor(), 5);
        press(Qt::Key_Z, ctrl);
        QCOMPARE(text(), QStringLiteral("two\nthree\none"));
        press(Qt::Key_Y, ctrl);
        QCOMPARE(text(), QStringLiteral("two\none\nthree"));
        press(Qt::Key_Z, ctrl);
        press(Qt::Key_Z, ctrl | shift);
        QCOMPARE(text(), QStringLiteral("two\none\nthree"));

        // Shift+Alt+Down copies the line below and follows the copy; Ctrl+Shift+K
        // deletes the line.
        start(QStringLiteral("one\ntwo"), 1);
        press(Qt::Key_Down, shift | alt);
        QCOMPARE(text(), QStringLiteral("one\none\ntwo"));
        QCOMPARE(cursor(), 5);
        press(Qt::Key_K, ctrl | shift);
        QCOMPARE(text(), QStringLiteral("one\ntwo"));
        QCOMPARE(cursor(), 4);

        // Ctrl+L selects the line, then the next one too.
        start(QStringLiteral("one\ntwo\nthree"), 1);
        press(Qt::Key_L, ctrl);
        QCOMPARE(input->property("selectedText").toString(), QStringLiteral("one\n"));
        press(Qt::Key_L, ctrl);
        QCOMPARE(input->property("selectionEnd").toInt(), 8);

        // With nothing selected, Ctrl+C copies the whole line and Ctrl+X cuts it.
        start(QStringLiteral("one\ntwo"), 1);
        press(Qt::Key_C, ctrl);
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("one\n"));
        QCOMPARE(text(), QStringLiteral("one\ntwo"));
        input->setProperty("cursorPosition", 5);
        press(Qt::Key_X, ctrl);
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("two\n"));
        QCOMPARE(text(), QStringLiteral("one"));

        // None of it sent anything; switching chats hands the new chat's
        // composer the keyboard.
        QCOMPARE(controller.messages()->rowCount(), sent);
        auto *search = qobject_cast<QQuickItem *>(
            window->findChild<QObject *>(QStringLiteral("contactSearch")));
        QVERIFY(search);
        search->forceActiveFocus();
        QVERIFY(!input->hasActiveFocus());
        QVERIFY(controller.selectContact(QStringLiteral("alex")));
        QTRY_VERIFY(input->hasActiveFocus());
    }

    void composerStopsAtTheLongestMessage()
    {
        OpenChat::ChatController controller;
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{QStringLiteral("chatController"), QVariant::fromValue(&controller)}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");
        QCOMPARE(engine.rootObjects().size(), 1);
        QQuickWindow *window = showActiveWindow(engine.rootObjects().constFirst());
        if (!window)
            QSKIP("No active window on this platform to type into");
        auto *input = qobject_cast<QQuickItem *>(
            window->findChild<QObject *>(QStringLiteral("messageInput")));
        auto *counter = qobject_cast<QQuickItem *>(
            window->findChild<QObject *>(QStringLiteral("composerLengthCounter")));
        QVERIFY(input && counter);
        const int max = controller.composerMaxLength();
        QVERIFY(!counter->isVisible());

        // More than fits arriving at once, as a paste would: the excess is cut.
        QVERIFY(input->setProperty("text", QString(max + 50, QLatin1Char('a'))));
        QCOMPARE(input->property("length").toInt(), max);
        QCOMPARE(controller.composerText().size(), max);
        QVERIFY(counter->isVisible());
        QCOMPARE(counter->property("text").toString(), QStringLiteral("0 left"));
        const QString capture = qEnvironmentVariable("OPENCHAT_COMPOSER_CAPTURE");
        if (!capture.isEmpty())
            QVERIFY(window->grabWindow().save(capture));

        // Typing into a full message changes nothing already there.
        input->forceActiveFocus();
        QVERIFY(input->setProperty("cursorPosition", 10));
        typeText(window, QStringLiteral("XYZ"));
        QCOMPARE(input->property("length").toInt(), max);
        QCOMPARE(controller.composerText(), QString(max, QLatin1Char('a')));
        QCOMPARE(input->property("cursorPosition").toInt(), 10);

        // With room again, typing lands where the cursor is.
        QVERIFY(input->setProperty("text", QString(max - 2, QLatin1Char('a'))));
        QVERIFY(input->setProperty("cursorPosition", 10));
        typeText(window, QStringLiteral("XYZ"));
        QCOMPARE(controller.composerText().size(), max);
        QCOMPARE(controller.composerText().mid(10, 3), QStringLiteral("XYa"));

        QVERIFY(input->setProperty("text", QString()));
        QVERIFY(!counter->isVisible());
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

    void outgoingBubblesWearTheEquippedSkin()
    {
        QQmlEngine engine;
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        auto *appearance = engine.singletonInstance<OpenChat::AppearanceSettings *>(
            "OpenChat.Native", "AppearanceSettings");
        QVERIFY(appearance);
        QCOMPARE(appearance->bubbleSkin(), QString());
        QQmlComponent component(&engine);
        component.loadFromModule("OpenChat", "MessageDelegate");
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        const auto message = [&component](int direction) {
            return component.createWithInitialProperties({
                {"direction", direction}, {"deliveryState", 3}, {"body", "hello"},
                {"timestamp", "10:15 AM"}, {"kind", 0}, {"dateLabel", ""},
                {"showDateDivider", false}, {"senderName", ""}, {"width", 540}});
        };
        QScopedPointer<QObject> mine(message(1));
        QScopedPointer<QObject> theirs(message(0));
        QVERIFY(mine && theirs);
        auto *myBubble = mine->findChild<QObject *>(QStringLiteral("messageBubble"));
        auto *myBody = mine->findChild<QObject *>(QStringLiteral("messageBody"));
        auto *myTime = mine->findChild<QObject *>(QStringLiteral("messageTimestamp"));
        auto *theirBubble = theirs->findChild<QObject *>(QStringLiteral("messageBubble"));
        auto *theirBody = theirs->findChild<QObject *>(QStringLiteral("messageBody"));
        QVERIFY(myBubble && myBody && myTime && theirBubble && theirBody);

        // Nothing equipped: the classic bubble and the theme's text colours.
        QCOMPARE(myBubble->property("skin").toString(), QString());
        QVERIFY(!myBubble->property("skinned").toBool());
        const QColor classicText = myBody->property("color").value<QColor>();
        const QColor classicTime = myTime->property("color").value<QColor>();
        QCOMPARE(classicText, QColor("#2b3b53"));
        QCOMPARE(myBody->property("style").toInt(), 0); // Text.Normal

        const QString nebula = QStringLiteral("bubble.nebula");
        appearance->setBubbleSkin(nebula);
        QCOMPARE(myBubble->property("skin").toString(), nebula);
        QVERIFY(myBubble->property("skinned").toBool());
        QCOMPARE(myBody->property("color").value<QColor>(), OpenChat::BubbleSkins::textColor(nebula));
        QCOMPARE(myTime->property("color").value<QColor>(),
                 OpenChat::BubbleSkins::secondaryTextColor(nebula));
        QCOMPARE(myBody->property("style").toInt(), 2); // Text.Raised
        QCOMPARE(myBody->property("styleColor").value<QColor>(),
                 OpenChat::BubbleSkins::textShadowColor(nebula));
        // Skins are local only: other people's bubbles stay classic.
        QCOMPARE(theirBubble->property("skin").toString(), QString());
        QCOMPARE(theirBody->property("color").value<QColor>(), classicText);

        // The choice is remembered.
        QCOMPARE(QSettings().value(QStringLiteral("Appearance/bubbleSkin")).toString(), nebula);
        OpenChat::AppearanceSettings reloaded;
        QCOMPARE(reloaded.bubbleSkin(), nebula);

        // A skin this build does not know (retired, or from a newer build)
        // falls back to the classic bubble rather than an unreadable one.
        appearance->setBubbleSkin(QStringLiteral("bubble.retired"));
        QVERIFY(!myBubble->property("skinned").toBool());
        QCOMPARE(myBody->property("color").value<QColor>(), classicText);
        QCOMPARE(myTime->property("color").value<QColor>(), classicTime);
        QCOMPARE(myBody->property("style").toInt(), 0);
        appearance->setBubbleSkin(QString());
    }

    void chatRowsShowUnreadCountBadges()
    {
        QQmlEngine engine;
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        QQmlComponent component(&engine);
        component.loadFromModule("OpenChat", "ContactRow");
        QScopedPointer<QObject> row(component.createWithInitialProperties({
            {"contactId", "bob"}, {"name", "Bob"}, {"statusText", "Offline"},
            {"presence", 2}, {"favorite", false}, {"selected", false},
            {"avatarKey", "userpfp_none"}, {"isGroup", false}, {"unreadCount", 12},
            {"callInProgress", false}, {"width", 250}, {"height", 60}}));
        QVERIFY2(row, qPrintable(component.errorString()));
        auto *badge = row->findChild<QObject *>("contactUnreadBadge");
        auto *label = row->findChild<QObject *>("contactUnreadLabel");
        QVERIFY(badge && label);
        QVERIFY(badge->property("visible").toBool());
        QCOMPARE(label->property("text").toString(), QString("12"));
        QCOMPARE(badge->property("color").value<QColor>(), QColor("#cb3843"));
        row->setProperty("unreadCount", 120);
        QCOMPARE(label->property("text").toString(), QString("99+"));
        row->setProperty("isGroup", true);
        QVERIFY(badge->property("visible").toBool());
        row->setProperty("unreadCount", 0);
        QVERIFY(!badge->property("visible").toBool());
    }

    void conversationEventsAreCompactAndAligned_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::addColumn<int>("kind");
        QTest::addColumn<int>("direction");
        QTest::newRow("join-light") << false << 2 << 0;
        QTest::newRow("join-dark") << true << 2 << 0;
        QTest::newRow("outgoing-light") << false << 3 << 1;
        QTest::newRow("outgoing-dark") << true << 3 << 1;
        QTest::newRow("incoming-light") << false << 3 << 0;
        QTest::newRow("incoming-dark") << true << 3 << 0;
    }

    void conversationEventsAreCompactAndAligned()
    {
        QFETCH(bool, dark);
        QFETCH(int, kind);
        QFETCH(int, direction);
        QSettings().setValue(QStringLiteral("Appearance/darkMode"), dark);
        QQmlEngine engine;
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        QQmlComponent component(&engine);
        component.loadFromModule("OpenChat", "MessageDelegate");
        QScopedPointer<QObject> item(component.createWithInitialProperties({
            {"direction", direction}, {"deliveryState", 0},
            {"body", kind == 2 ? "Ayman Left the group chat" : "JohnM Started a group call"},
            {"timestamp", "10:15 AM"}, {"kind", kind}, {"dateLabel", "September 6, 2026"},
            {"showDateDivider", false}, {"senderName", "JohnM"}, {"width", 540}}));
        QVERIFY2(item, qPrintable(component.errorString()));
        auto *row = item->findChild<QObject *>("conversationEvent");
        auto *label = item->findChild<QObject *>("conversationEventText");
        QVERIFY(row && label);
        QVERIFY(row->property("visible").toBool());
        QVERIFY(item->property("implicitHeight").toReal() < 50);
        QVERIFY(row->property("width").toReal() < 400);
        QVERIFY(!item->findChild<QObject *>("messageTimestamp")->property("visible").toBool());
        QVERIFY(!item->findChild<QObject *>("messageSender")->property("visible").toBool());
        const qreal x = row->property("x").toReal();
        const qreal width = row->property("width").toReal();
        if (kind == 2) {
            QCOMPARE(x, (540 - width) / 2);
        } else {
            QCOMPARE(x, direction == 1 ? 540 - width - 17 : 16);
            QCOMPARE(label->property("color").value<QColor>(),
                     QColor(dark ? "#95d6ac" : "#2e7d4f"));
        }
        const qreal compactHeight = item->property("implicitHeight").toReal();
        item->setProperty("showDateDivider", true);
        QCOMPARE(item->property("implicitHeight").toReal(), compactHeight + 64);
        // Long names wrap and retain their own space even at narrow widths.
        item->setProperty("width", 220);
        item->setProperty("body", QString(150, QLatin1Char('W')));
        QVERIFY(item->property("implicitHeight").toReal() > compactHeight + 64);
        QVERIFY(row->property("width").toReal() <= 184);
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
        QVERIFY(banner);
        QVERIFY(messageList);
        QVERIFY(notice);
        QVERIFY(input);

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
        QVERIFY(!controller.canSend());

        // Returning to Ready restores the interface and the composer draft is intact.
        controller.setSessionState(OpenChat::ChatController::SessionState::Ready);
        QCoreApplication::processEvents();
        QVERIFY(!banner->property("visible").toBool());
        QCOMPARE(banner->property("height").toReal(), 0.0);
        QVERIFY(messageList->property("visible").toBool());
        QVERIFY(!notice->property("visible").toBool());
        QCOMPARE(controller.composerText(), QStringLiteral("blocked while locked"));
        QVERIFY(controller.canSend());
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
        // surfaces exactly what the flow produced, independent of any real
        // ProfileSession. The controller pointer is bound after construction; the
        // Starter is only invoked later, when the submit button is clicked.
        OpenChat::OnboardingController *controllerPtr = nullptr;
        QString submittedHandle;
        QString submittedPassword;
        OpenChat::OnboardingController controller(
            [&](OpenChat::OnboardingController::Mode, const QString &handle,
                const QString &password) {
                submittedHandle = handle;
                submittedPassword = QString(password.constData(), password.size());
                controllerPtr->onSubmitSucceeded(QStringLiteral("TEST-CODE-1234-5678"));
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

        const auto child = [&root](const char *name) {
            return root->findChild<QObject *>(QString::fromLatin1(name));
        };
        QObject *credentialsView = child("onboardingCredentialsView");
        QObject *recoveryView = child("onboardingRecoveryView");
        QObject *title = child("onboardingTitle");
        QObject *notice = child("onboardingNotice");
        QObject *handleField = child("handleField");
        QObject *passwordField = child("passwordField");
        QObject *passwordConfirmField = child("passwordConfirmField");
        QObject *passwordReveal = child("passwordFieldReveal");
        QObject *passwordHint = child("passwordHint");
        QObject *strengthMeter = child("passwordStrengthMeter");
        QObject *submitButton = child("submitButton");
        QObject *modeSwitch = child("modeSwitch");
        QObject *savedButton = child("savedButton");
        QObject *recoveryCodeText = child("recoveryCodeText");
        for (QObject *object :
             {credentialsView, recoveryView, title, notice, handleField, passwordField,
              passwordConfirmField, passwordReveal, passwordHint, strengthMeter, submitButton,
              modeSwitch, savedButton, recoveryCodeText})
            QVERIFY(object);

        // The sign-up form is shown first, with nothing to submit yet.
        QVERIFY(credentialsView->property("visible").toBool());
        QVERIFY(!recoveryView->property("visible").toBool());
        QCOMPARE(title->property("text").toString(), QStringLiteral("Create your account"));
        QCOMPARE(submitButton->property("enabled").toBool(), false);
        QVERIFY(!notice->property("visible").toBool());

        // A startup notice appears above the form when there is one.
        controller.setNotice(QStringLiteral("Old account data was erased."));
        QCoreApplication::processEvents();
        QVERIFY(notice->property("visible").toBool());

        // Passwords are masked, and only ever shown on request. (TextInput.Normal
        // and TextInput.Password, per the documented EchoMode values.)
        constexpr int echoNormal = 0;
        constexpr int echoPassword = 2;
        QCOMPARE(passwordField->property("echoMode").toInt(), echoPassword);
        QCOMPARE(passwordConfirmField->property("echoMode").toInt(),
                 echoPassword);

        // Switching to log-in hides the confirmation and the new-password guidance.
        QVERIFY(QMetaObject::invokeMethod(modeSwitch, "clicked"));
        QCoreApplication::processEvents();
        QCOMPARE(controller.mode(), OpenChat::OnboardingController::Mode::LogIn);
        QCOMPARE(title->property("text").toString(), QStringLiteral("Welcome back"));
        QVERIFY(!passwordConfirmField->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(modeSwitch, "clicked"));
        QCoreApplication::processEvents();
        QCOMPARE(controller.mode(), OpenChat::OnboardingController::Mode::SignUp);
        QVERIFY(passwordConfirmField->property("visible").toBool());

        // Typing flows into the controller, and its guidance back onto the screen.
        QVERIFY(handleField->setProperty("text", QStringLiteral("Ada")));
        QVERIFY(passwordField->setProperty("text", QStringLiteral("short")));
        QCoreApplication::processEvents();
        QCOMPARE(controller.handle(), QStringLiteral("Ada"));
        QVERIFY(passwordHint->property("visible").toBool());
        QVERIFY(strengthMeter->property("visible").toBool());
        QCOMPARE(submitButton->property("enabled").toBool(), false);

        const QString password = QStringLiteral("correct horse battery");
        QVERIFY(passwordField->setProperty("text", password));
        QCoreApplication::processEvents();
        QVERIFY(!passwordHint->property("visible").toBool());
        QCOMPARE(submitButton->property("enabled").toBool(), false); // unconfirmed
        QVERIFY(passwordConfirmField->setProperty("text", password));
        QCoreApplication::processEvents();
        QCOMPARE(submitButton->property("enabled").toBool(), true);

        // The Show toggle reveals the password on request...
        QVERIFY(passwordReveal->property("visible").toBool());
        QObject *passwordFrame = qvariant_cast<QObject *>(passwordField->property("parent"));
        QVERIFY(passwordFrame);
        QVERIFY(passwordFrame->setProperty("revealed", true));
        QCoreApplication::processEvents();
        QCOMPARE(passwordField->property("echoMode").toInt(), echoNormal);

        QSignalSpy completedSpy(&controller, &OpenChat::OnboardingController::completed);

        // Submitting hands over the canonical username and the password, empties
        // both password fields on screen, and re-masks the revealed one.
        QVERIFY(QMetaObject::invokeMethod(submitButton, "clicked"));
        QCoreApplication::processEvents();
        QCOMPARE(submittedHandle, QStringLiteral("ada"));
        QCOMPARE(submittedPassword, password);
        QCOMPARE(passwordField->property("text").toString(), QString());
        QCOMPARE(passwordConfirmField->property("text").toString(), QString());
        QCOMPARE(passwordField->property("echoMode").toInt(), echoPassword);

        // A new account then reveals the returned recovery code.
        QCOMPARE(controller.step(), OpenChat::OnboardingController::Step::Recovery);
        QVERIFY(!credentialsView->property("visible").toBool());
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

        // The dialog is not even built until the controller opens it.
        QVERIFY(!root->findChild<QObject *>(QStringLiteral("addContactDialog")));

        contactController.openDialog();
        QCoreApplication::processEvents();
        QObject *dialog = root->findChild<QObject *>(QStringLiteral("addContactDialog"));
        QVERIFY(dialog);
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

    void categoriesFoldToTheirHeaderOnClick()
    {
        OpenChat::ChatController chatController;
        OpenChat::ContactController contactController;
        contactController.enableForPreview();
        contactController.addMockRequest(QStringLiteral("@dave"), QStringLiteral("wants to chat"));

        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{QStringLiteral("chatController"), QVariant::fromValue(&chatController)},
             {QStringLiteral("contactController"), QVariant::fromValue(&contactController)}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");
        QCOMPARE(engine.rootObjects().size(), 1);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        const auto centre = [](QQuickItem *item) {
            return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
        };

        auto *chats = window->findChild<QQuickItem *>(QStringLiteral("contactsCategory"));
        auto *requestsPanel = window->findChild<QQuickItem *>(QStringLiteral("requestsPanel"));
        QVERIFY(chats && requestsPanel);
        QTRY_VERIFY(requestsPanel->height() > 40.0);
        auto *chatsHeader = findVisualItem(chats, QStringLiteral("categoryHeaderArea"));
        auto *alex = findVisualItem(chats, QStringLiteral("contactRow_alex"));
        auto *requestsHeader = findVisualItem(requestsPanel, QStringLiteral("requestsHeaderArea"));
        auto *badge = findVisualItem(requestsPanel, QStringLiteral("requestsBadge"));
        OpenChat::RequestListModel *model = contactController.requests();
        const QString requestId =
            model->data(model->index(0), OpenChat::RequestListModel::IdRole).toString();
        auto *requestRow = findVisualItem(requestsPanel, QStringLiteral("requestRow_") + requestId);
        QVERIFY(chatsHeader && alex && requestsHeader && badge && requestRow);
        const qreal chatsOpenHeight = chats->height();
        const qreal requestsOpenHeight = requestsPanel->height();
        const qreal chatsOpenY = chats->y();
        QVERIFY(chatsOpenHeight > 40.0);
        QVERIFY(alex->isVisible() && requestRow->isVisible());

        // Friend requests folds to its header, keeping its count badge, and
        // the category below moves up into the freed space.
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centre(requestsHeader));
        QVERIFY(requestsPanel->property("collapsed").toBool());
        QCOMPARE(requestsPanel->height(), 39.0); // its bottom rule meets the next header's
        QVERIFY(!requestRow->isVisible());
        QVERIFY(badge->isVisible());
        QTRY_COMPARE(chats->y(), chatsOpenY - (requestsOpenHeight - 39.0));

        // Chats folds the same way and its rows stop being drawn.
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centre(chatsHeader));
        QVERIFY(chats->property("collapsed").toBool());
        QCOMPARE(chats->height(), 39.0);
        QVERIFY(!alex->isVisible());
        const QString capture = qEnvironmentVariable("OPENCHAT_CATEGORY_FOLD_CAPTURE");
        if (!capture.isEmpty())
            QVERIFY(window->grabWindow().save(capture));

        // A second click on each brings the rows back where they were.
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centre(chatsHeader));
        QVERIFY(!chats->property("collapsed").toBool());
        QCOMPARE(chats->height(), chatsOpenHeight);
        QVERIFY(alex->isVisible());
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centre(requestsHeader));
        QVERIFY(!requestsPanel->property("collapsed").toBool());
        QCOMPARE(requestsPanel->height(), requestsOpenHeight);
        QVERIFY(requestRow->isVisible());
        QTRY_COMPARE(chats->y(), chatsOpenY);
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

        // The dialog is not even built until the controller opens the surface.
        QVERIFY(!root->findChild<QObject *>(QStringLiteral("safetyNumberDialog")));

        contactController.openSafetyNumberPreview();
        QCoreApplication::processEvents();
        QObject *dialog = root->findChild<QObject *>(QStringLiteral("safetyNumberDialog"));
        QVERIFY(dialog);
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
        // and the call surface is not built at all.
        QVERIFY(conversationHeader->isVisible());
        QVERIFY(!root->findChild<QQuickItem *>(QStringLiteral("callHeader")));

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

    void theCallScreenWaitsForThePeerAndOffersToRejoin()
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

        // Jessica hung up on a call we are still in: her picture is captioned
        // and faded, the controls stay (we are in a call), and the status
        // line counts the grace period down instead of the call's length.
        callController.enableForPreview(OpenChat::CallState::Active, QStringLiteral("Jessica"),
                                        QStringLiteral("jessica"), false, false);
        callController.setPreviewWaiting(true, 4 * 60'000 + 59'000, QStringLiteral("Left"));
        QCoreApplication::processEvents();
        auto *remote = root->findChild<QQuickItem *>(QStringLiteral("remoteParticipant"));
        auto *status = root->findChild<QQuickItem *>(QStringLiteral("callStatusText"));
        auto *end = root->findChild<QQuickItem *>(QStringLiteral("endCallButton"));
        auto *rejoin = root->findChild<QQuickItem *>(QStringLiteral("rejoinCallButton"));
        auto *dismiss = root->findChild<QQuickItem *>(QStringLiteral("dismissCallButton"));
        QVERIFY(remote && status && end && rejoin && dismiss);
        QCOMPARE(remote->property("caption").toString(), QStringLiteral("Left"));
        QVERIFY(remote->property("dimmed").toBool());
        QCOMPARE(status->property("text").toString(),
                 QStringLiteral("Waiting for Jessica to come back · 4:59"));
        QVERIFY(end->isVisible());
        QVERIFY(!rejoin->isVisible());

        // She is back: the caption goes and the duration returns.
        callController.setPreviewWaiting(false, 0, QString());
        QCoreApplication::processEvents();
        QVERIFY(remote->property("caption").toString().isEmpty());
        QVERIFY(!remote->property("dimmed").toBool());
        QCOMPARE(status->property("text").toString(), callController.durationText());

        // We hung up while she stayed: the ended surface offers Rejoin beside
        // Back to chat, and only while there is a call to go back to.
        callController.enableForPreview(OpenChat::CallState::Ended, QStringLiteral("Jessica"),
                                        QStringLiteral("jessica"), false, false);
        callController.setPreviewCanRejoin(true);
        QCoreApplication::processEvents();
        QVERIFY(rejoin->isVisible());
        QVERIFY(dismiss->isVisible());
        QVERIFY(!end->isVisible());
        QCOMPARE(rejoin->property("label").toString(), QStringLiteral("Rejoin"));
        QCOMPARE(rejoin->property("accent").toString(), QStringLiteral("accept"));
        QTRY_VERIFY(rejoin->x() < dismiss->x());
        callController.setPreviewCanRejoin(false);
        QCoreApplication::processEvents();
        QVERIFY(!rejoin->isVisible());
        QVERIFY(dismiss->isVisible());
    }

    void theCallSurfaceStaysInItsOwnConversationAndIsAStripElsewhere()
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

        auto *conversationHeader =
            root->findChild<QQuickItem *>(QStringLiteral("conversationHeader"));
        auto *stripSlot = root->findChild<QQuickItem *>(QStringLiteral("callStripSlot"));
        auto *history = root->findChild<QQuickItem *>(QStringLiteral("messageHistory"));
        QVERIFY(conversationHeader && stripSlot);
        // No call: no call surface, no strip, and no room taken by it.
        QVERIFY(!root->findChild<QQuickItem *>(QStringLiteral("callHeader")));
        QVERIFY(!stripSlot->isVisible());
        QCOMPARE(stripSlot->height(), 0.0);

        // A call on the open conversation: the surface, as ever, and no strip.
        callController.enableForPreview(OpenChat::CallState::Active, QStringLiteral("Jessica"),
                                        QStringLiteral("jessica"), false, false);
        QCoreApplication::processEvents();
        auto *callHeader = root->findChild<QQuickItem *>(QStringLiteral("callHeader"));
        QVERIFY(callHeader);
        QVERIFY(callHeader->isVisible());
        QVERIFY(!conversationHeader->isVisible());
        QVERIFY(!stripSlot->isVisible());

        // The user opens another conversation: that conversation's own header
        // comes back, the call surface is gone from it, and a strip under the
        // header keeps the call in reach — named, timed, with Return and End.
        callController.setPreviewCallInCurrentChat(false);
        QCoreApplication::processEvents();
        QVERIFY(!callHeader->isVisible());
        QVERIFY(conversationHeader->isVisible());
        QVERIFY(stripSlot->isVisible());
        QVERIFY(stripSlot->height() > 0);
        auto *strip = root->findChild<QQuickItem *>(QStringLiteral("callStrip"));
        auto *text = root->findChild<QQuickItem *>(QStringLiteral("callStripText"));
        auto *back = root->findChild<QQuickItem *>(QStringLiteral("callStripReturnButton"));
        auto *end = root->findChild<QQuickItem *>(QStringLiteral("callStripEndButton"));
        auto *answer = root->findChild<QQuickItem *>(QStringLiteral("callStripAnswerButton"));
        auto *decline = root->findChild<QQuickItem *>(QStringLiteral("callStripDeclineButton"));
        QVERIFY(strip && text && back && end && answer && decline);
        QCOMPARE(text->property("text").toString(),
                 QStringLiteral("In call with Jessica · ") + callController.durationText());
        QVERIFY(back->isVisible());
        QCOMPARE(back->property("label").toString(), QStringLiteral("Return"));
        QVERIFY(end->isVisible());
        QVERIFY(!answer->isVisible());
        QVERIFY(!decline->isVisible());
        // Directly under the header, and the conversation below it moved down.
        auto *slot = root->findChild<QQuickItem *>(QStringLiteral("conversationHeaderSlot"));
        QVERIFY(slot);
        QCOMPARE(stripSlot->mapToScene(QPointF()).y(), slot->mapToScene(QPointF()).y() + slot->height());
        if (history)
            QVERIFY(history->mapToScene(QPointF()).y() >= stripSlot->mapToScene(QPointF()).y() + stripSlot->height());

        // A call ringing from another conversation can be answered or refused
        // from the strip without leaving.
        callController.enableForPreview(OpenChat::CallState::Ringing, QStringLiteral("Jessica"),
                                        QStringLiteral("jessica"), false, false);
        callController.setPreviewCallInCurrentChat(false);
        QCoreApplication::processEvents();
        QVERIFY(stripSlot->isVisible());
        QCOMPARE(text->property("text").toString(), QStringLiteral("Incoming call from Jessica"));
        QVERIFY(answer->isVisible());
        QVERIFY(decline->isVisible());
        QVERIFY(!end->isVisible());
        QCOMPARE(back->property("label").toString(), QStringLiteral("Open"));

        // Back to the call's own conversation: the surface again, strip gone.
        callController.setPreviewCallInCurrentChat(true);
        QCoreApplication::processEvents();
        QVERIFY(callHeader->isVisible());
        QVERIFY(!stripSlot->isVisible());
        QCOMPARE(stripSlot->height(), 0.0);
    }

    void theConversationHeaderSaysWhoIsInACallAndOffersToJoin()
    {
        OpenChat::ChatController chatController;
        OpenChat::CallController callController;
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{QStringLiteral("chatController"), QVariant::fromValue(&chatController)},
             {QStringLiteral("callController"), QVariant::fromValue(&callController)}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");
        QCOMPARE(engine.rootObjects().size(), 1);
        QObject *root = engine.rootObjects().constFirst();
        QVERIFY(chatController.hasCurrentContact());

        auto *banner = root->findChild<QQuickItem *>(QStringLiteral("ongoingCallBanner"));
        auto *text = root->findChild<QQuickItem *>(QStringLiteral("ongoingCallText"));
        auto *join = root->findChild<QQuickItem *>(QStringLiteral("joinCallButton"));
        auto *header = root->findChild<QQuickItem *>(QStringLiteral("conversationHeader"));
        QVERIFY(banner && text && join && header);
        // Nothing to say while no call is going.
        QVERIFY(!banner->isVisible());

        callController.setPreviewOngoingCall(QStringLiteral("Jessica and Michael are in a call"));
        QCoreApplication::processEvents();
        QVERIFY(banner->isVisible());
        QVERIFY(join->isVisible());
        QCOMPARE(text->property("text").toString(),
                 QStringLiteral("Jessica and Michael are in a call"));
        QCOMPARE(join->property("label").toString(), QStringLiteral("Join"));
        // Under the subtitle, inside the header, clear of the call buttons.
        auto *phone = root->findChild<QQuickItem *>(QStringLiteral("phoneCallButton"));
        QVERIFY(phone);
        QVERIFY(banner->mapToScene(QPointF()).y() > 60);
        QVERIFY(banner->mapToScene(QPointF(banner->width(), 0)).x()
                <= phone->mapToScene(QPointF()).x());
        QVERIFY(banner->y() + banner->height() <= header->height());

        callController.setPreviewOngoingCall(QString());
        QCoreApplication::processEvents();
        QVERIFY(!banner->isVisible());
    }

    void chatRowsMarkACallInProgress()
    {
        QQmlEngine engine;
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        QQmlComponent component(&engine);
        component.loadFromModule("OpenChat", "ContactRow");
        QScopedPointer<QObject> row(component.createWithInitialProperties({
            {"contactId", "bob"}, {"name", "Bob"}, {"statusText", "Offline"},
            {"presence", 2}, {"favorite", false}, {"selected", false},
            {"avatarKey", "userpfp_none"}, {"isGroup", false}, {"unreadCount", 3},
            {"callInProgress", true}, {"width", 250}, {"height", 60}}));
        QVERIFY2(row, qPrintable(component.errorString()));
        auto *pill = row->findChild<QQuickItem *>("contactCallPill");
        auto *badge = row->findChild<QQuickItem *>("contactUnreadBadge");
        QVERIFY(pill && badge);
        QVERIFY(pill->isVisible());
        QVERIFY(badge->isVisible());
        // The mark sits left of the unread badge and never runs under it.
        QVERIFY(pill->x() + pill->width() <= badge->x());
        row->setProperty("callInProgress", false);
        QVERIFY(!pill->isVisible());
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

    void theScreenShareButtonSitsBesideTheCameraAndTracksItsState()
    {
        OpenChat::ChatController chatController;
        OpenChat::CallController callController;
        callController.enableForPreview(OpenChat::CallState::Active, QStringLiteral("Jessica"),
                                        QStringLiteral("jessica"), false, false);

        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{QStringLiteral("chatController"), QVariant::fromValue(&chatController)},
             {QStringLiteral("callController"), QVariant::fromValue(&callController)}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");
        QCOMPARE(engine.rootObjects().size(), 1);
        QObject *root = engine.rootObjects().constFirst();

        auto *camera = root->findChild<QQuickItem *>(QStringLiteral("cameraCallButton"));
        auto *share = root->findChild<QQuickItem *>(QStringLiteral("screenShareButton"));
        auto *end = root->findChild<QQuickItem *>(QStringLiteral("endCallButton"));
        QVERIFY2(share, "the call screen has no screen-share button");
        QVERIFY(camera && end);

        // Directly beside the camera, in the same row, at the same size — one
        // pair of media controls rather than two unrelated ones.
        QVERIFY(share->isVisible());
        QCOMPARE(share->parentItem(), camera->parentItem());
        QCOMPARE(share->y(), camera->y());
        QCOMPARE(share->height(), camera->height());
        QVERIFY2(share->x() > camera->x(), "the share button is not beside the camera");
        QVERIFY2(share->x() < end->x(), "the share button is past the end-call button");

        // Available but idle: it offers to share and is not checked.
        QCOMPARE(share->property("label").toString(), QStringLiteral("Share screen"));
        QCOMPARE(share->property("checked").toBool(), false);
        QCOMPARE(share->property("screenIcon").toBool(), true);
        // No live call engine in a preview, so the control explains itself
        // rather than silently doing nothing.
        QCOMPARE(callController.screenShareAvailable(), false);
        QCOMPARE(share->property("disabled").toBool(), true);
        QVERIFY(!share->property("tooltip").toString().isEmpty());

        // The stage only exists while there is something on it.
        auto *stage = root->findChild<QQuickItem *>(QStringLiteral("screenShareStage"));
        QVERIFY(stage);
        QVERIFY2(!stage->isVisible(), "the share stage was shown with nothing being shared");

        // Ringing and ended are not moments to start a share, exactly as for
        // the camera beside it.
        callController.enableForPreview(OpenChat::CallState::Ringing, QStringLiteral("Jessica"),
                                        QStringLiteral("jessica"), false, false);
        QCoreApplication::processEvents();
        QVERIFY(!share->isVisible());
        QCOMPARE(share->isVisible(), camera->isVisible());
        callController.enableForPreview(OpenChat::CallState::Ended, QStringLiteral("Jessica"),
                                        QStringLiteral("jessica"), false, false);
        QCoreApplication::processEvents();
        QVERIFY(!share->isVisible());
        QCOMPARE(share->isVisible(), camera->isVisible());
    }

    void anIncomingShareAppearsOnTheStageByItself()
    {
        // The whole seam between the C++ side and the view: a share is handed
        // across as a live surface rather than as a picture, and the stage has
        // to appear on its own when one arrives and go away when it stops —
        // exactly as an incoming camera tile does.
        OpenChat::ChatController chatController;
        OpenChat::CallController callController;
        callController.enableForPreview(OpenChat::CallState::Active, QStringLiteral("Jessica"),
                                        QStringLiteral("jessica"), false, false);

        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{QStringLiteral("chatController"), QVariant::fromValue(&chatController)},
             {QStringLiteral("callController"), QVariant::fromValue(&callController)}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");
        QCOMPARE(engine.rootObjects().size(), 1);
        QObject *root = engine.rootObjects().constFirst();

        auto *stage = root->findChild<QQuickItem *>(QStringLiteral("screenShareStage"));
        auto *video = root->findChild<QQuickItem *>(QStringLiteral("remoteScreenVideo"));
        QVERIFY(stage && video);
        QVERIFY(!stage->isVisible());
        QCOMPARE(stage->property("implicitHeight").toDouble(), 0.0);

        auto canvas = std::make_shared<OpenChat::ScreenCanvas>(QSize(1600, 900));
        QVERIFY(!canvas->isEmpty());
        callController.setPreviewScreenShare(canvas, QStringLiteral("Jessica"));
        QCoreApplication::processEvents();

        QVERIFY2(stage->isVisible(), "an incoming share did not raise the stage");
        QVERIFY(stage->property("implicitHeight").toDouble() > 0.0);
        // The surface itself crossed into the view, not a copy of its pixels.
        // Scoped, so this check is not itself the thing keeping it alive below.
        {
            const QVariant held = video->property("canvas");
            QCOMPARE(held.value<OpenChat::ScreenCanvasPtr>(), canvas);
        }
        QCOMPARE(video->property("sourceAspect").toDouble(), 1600.0 / 900.0);
        auto *caption = root->findChild<QQuickItem *>(QStringLiteral("remoteScreenCaption"));
        QVERIFY(caption);
        QCOMPARE(caption->property("text").toString(), QStringLiteral("Jessica's screen"));

        // Stopping takes the stage down with it and lets go of the pixels.
        std::weak_ptr<OpenChat::ScreenCanvas> observer = canvas;
        callController.setPreviewScreenShare({}, QString());
        QCoreApplication::processEvents();
        QVERIFY2(!stage->isVisible(), "the stage stayed up after the share stopped");
        QVERIFY(!video->property("canvas").value<OpenChat::ScreenCanvasPtr>());
        canvas.reset();
        QVERIFY2(observer.expired(), "the view kept the share's pixels alive after it ended");
    }

    void thePickerCanActOnTheRowThatWasClicked()
    {
        // The picker is instantiated from Main.qml, whose own root carries the
        // same id. Every row's click handler has to reach the PICKER's root to
        // start a share, and this is where that resolution is checked — a bare
        // load never builds a row, so nothing else here would catch it.
        OpenChat::ChatController chatController;
        OpenChat::CallController callController;
        callController.enableForPreview(OpenChat::CallState::Active, QStringLiteral("Jessica"),
                                        QStringLiteral("jessica"), false, false);

        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{QStringLiteral("chatController"), QVariant::fromValue(&chatController)},
             {QStringLiteral("callController"), QVariant::fromValue(&callController)}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");
        QCOMPARE(engine.rootObjects().size(), 1);
        QObject *root = engine.rootObjects().constFirst();

        // A ListView builds no rows until something lays it out, and nothing
        // lays out a window that was never shown. This is the one test here
        // that needs real delegates, so it shows the window and waits for it.
        auto *window = qobject_cast<QQuickWindow *>(root);
        QVERIFY(window);
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));

        auto *picker = root->findChild<QQuickItem *>(QStringLiteral("screenSharePicker"));
        QVERIFY2(picker, "the screen-source picker was never loaded");
        QVERIFY(!picker->isVisible());

        QVERIFY(QMetaObject::invokeMethod(picker, "show"));
        QCoreApplication::processEvents();
        QVERIFY2(picker->isVisible(), "the picker did not open");

        auto *list = root->findChild<QQuickItem *>(QStringLiteral("screenSourceList"));
        QVERIFY(list);
        // Offscreen still reports a screen, so there is always at least one row.
        QVERIFY2(picker->property("sources").toList().size() > 0,
                 "no capturable source was enumerated");
        // Nothing renders in a headless load, so the view is asked to lay out
        // rather than waiting for a frame that never comes.
        QVERIFY(QMetaObject::invokeMethod(list, "forceLayout"));
        QQuickItem *row = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT(
            QMetaObject::invokeMethod(list, "itemAtIndex", Q_RETURN_ARG(QQuickItem *, row),
                                      Q_ARG(int, 0))
                && row != nullptr,
            5000);

        // The row's own handler runs in the delegate's scope. Evaluating the
        // names it uses there is exactly what a click does.
        // The id this resolves to is the PICKER'''s, not the Main.qml root that
        // shares the name — an ambiguity that left every row's click handler
        // unable to see either of them.
        QQmlExpression reachesPicker(qmlContext(row), row,
                                     QStringLiteral("picker.objectName"));
        bool failed = false;
        const QVariant reached = reachesPicker.evaluate(&failed);
        QVERIFY2(!failed && !reachesPicker.hasError(),
                 qPrintable(reachesPicker.error().toString()));
        QCOMPARE(reached.toString(), QStringLiteral("screenSharePicker"));

        // And choosing a row closes the picker and asks the controller to start.
        QVERIFY(QMetaObject::invokeMethod(row, "activate"));
        QCoreApplication::processEvents();
        QVERIFY2(!picker->isVisible(), "choosing a source left the picker open");
        // No live engine in a preview, so nothing starts — but the attempt is
        // what had to reach the controller, and it did so without throwing.
        QVERIFY(!callController.screenShareEnabled());
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

    // The window an in-call test shows for real: mouse clicks and shortcuts
    // only reach a window that is exposed and active.
    static QQuickWindow *showActiveWindow(QObject *root)
    {
        auto *window = qobject_cast<QQuickWindow *>(root);
        if (!window)
            return nullptr;
        window->show();
        window->requestActivate();
        if (!QTest::qWaitForWindowExposed(window) || !QTest::qWaitForWindowActive(window))
            return nullptr;
        return window;
    }

    static void clickItem(QQuickWindow *window, QQuickItem *item)
    {
        const QPointF centre = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centre.toPoint());
    }

    void aPictureEnlargesOverTheWindowAndAClickOutsideShrinksIt()
    {
        // The zoom chip on a camera tile grows a copy of that camera to fill
        // the window at its own aspect, behind a scrim that takes every click;
        // a click anywhere off the picture shrinks it back onto the tile.
        OpenChat::ChatController chats;
        OpenChat::CallController calls;
        calls.enableForPreview(OpenChat::CallState::Active, QStringLiteral("Jessica"),
                               QStringLiteral("jessica"), true, false);
        QImage wide(640, 360, QImage::Format_RGB32);
        QImage portrait(360, 640, QImage::Format_RGB32);
        wide.fill(Qt::blue);
        portrait.fill(Qt::green);
        calls.setPreviewVideo(wide, portrait);

        QQmlApplicationEngine engine;
        engine.setInitialProperties({{QStringLiteral("chatController"), QVariant::fromValue(&chats)},
                                     {QStringLiteral("callController"), QVariant::fromValue(&calls)}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");
        QCOMPARE(engine.rootObjects().size(), 1);
        QObject *root = engine.rootObjects().constFirst();
        QQuickWindow *window = showActiveWindow(root);
        QVERIFY(window);

        auto *remote = root->findChild<QQuickItem *>(QStringLiteral("remoteParticipant"));
        QVERIFY(remote);
        auto *remoteVideo = remote->findChild<QQuickItem *>(QStringLiteral("participantVideo"));
        auto *zoomChip = remote->findChild<QQuickItem *>(QStringLiteral("zoomButton"));
        auto *overlay = root->findChild<QQuickItem *>(QStringLiteral("mediaZoomOverlay"));
        auto *picture = root->findChild<QQuickItem *>(QStringLiteral("mediaZoomPicture"));
        QVERIFY(remoteVideo && zoomChip && overlay && picture);
        QVERIFY(zoomChip->isVisible());
        // The chip sits in the tile's bottom-right corner, on the picture.
        const QPointF chipCorner = zoomChip->mapToItem(
            remoteVideo, QPointF(zoomChip->width(), zoomChip->height()));
        QVERIFY(qAbs(chipCorner.x() - (remoteVideo->width() - 6)) < 1.0);
        QVERIFY(qAbs(chipCorner.y() - (remoteVideo->height() - 6)) < 1.0);
        QVERIFY(!overlay->isVisible());

        clickItem(window, zoomChip);
        QTRY_VERIFY2(overlay->isVisible(), "the zoom chip did not open the enlarged picture");
        QVERIFY(overlay->property("expanded").toBool());
        QCOMPARE(overlay->property("sourceItem").value<QQuickItem *>(), remoteVideo);
        // The tile under the scrim stops repainting while its copy is up.
        QVERIFY(remoteVideo->property("paused").toBool());

        // It grows to the largest rectangle of the remote camera's aspect that
        // fits the window with air around it, centred — never stretched.
        const double aspect = 360.0 / 640.0;
        const double margin = overlay->property("margin").toDouble();
        const double fitWidth = std::min(window->width() - 2 * margin,
                                         (window->height() - 2 * margin) * aspect);
        QTRY_VERIFY(qAbs(picture->width() - fitWidth) < 1.0);
        QVERIFY(qAbs(picture->width() / picture->height() - aspect) < 0.005);
        QVERIFY(qAbs(picture->x() + picture->width() / 2 - window->width() / 2.0) < 1.5);
        QVERIFY(qAbs(picture->y() + picture->height() / 2 - window->height() / 2.0) < 1.5);
        // And it shows the same frames the tile does.
        auto *copy = root->findChild<QQuickItem *>(QStringLiteral("mediaZoomVideo"));
        QVERIFY(copy);
        QCOMPARE(copy->property("frame").value<QImage>().size(), portrait.size());
        QCOMPARE(copy->property("sourceAspect").toDouble(), aspect);

        // A click on the picture is nothing; a click anywhere else closes it.
        clickItem(window, picture);
        QCoreApplication::processEvents();
        QVERIFY(overlay->property("expanded").toBool());
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                          QPoint(12, window->height() / 2));
        QTRY_VERIFY(!overlay->property("expanded").toBool());
        QVERIFY(!remoteVideo->property("paused").toBool());
        // It shrinks back onto the tile it came from before it goes.
        const QPointF tileOrigin = remoteVideo->mapToScene(QPointF());
        QTRY_VERIFY(qAbs(picture->x() - tileOrigin.x()) < 1.0);
        QVERIFY(qAbs(picture->width() - remoteVideo->width()) < 1.0);
        QTRY_VERIFY2(!overlay->isVisible(), "the enlarged picture stayed after the shrink");
        QVERIFY(!overlay->property("sourceItem").value<QQuickItem *>());
    }

    void anEnlargedShareIsEncodedForTheWindowAndClosesWithTheShare()
    {
        // Enlarging the far end's screen changes what the sender is told about
        // our view — the size it is drawn at is now most of the window, so the
        // share is encoded for that and not for the strip. Escape closes it,
        // and a share that stops takes its enlarged copy with it at once.
        OpenChat::ChatController chats;
        OpenChat::CallController calls;
        calls.enableForPreview(OpenChat::CallState::Active, QStringLiteral("Jessica"),
                               QStringLiteral("jessica"), false, false);
        auto canvas = std::make_shared<OpenChat::ScreenCanvas>(QSize(1600, 900));
        calls.setPreviewScreenShare(canvas, QStringLiteral("Jessica"));

        QQmlApplicationEngine engine;
        engine.setInitialProperties({{QStringLiteral("chatController"), QVariant::fromValue(&chats)},
                                     {QStringLiteral("callController"), QVariant::fromValue(&calls)}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");
        QCOMPARE(engine.rootObjects().size(), 1);
        QObject *root = engine.rootObjects().constFirst();
        QQuickWindow *window = showActiveWindow(root);
        QVERIFY(window);

        auto *video = root->findChild<QQuickItem *>(QStringLiteral("remoteScreenVideo"));
        auto *chip = root->findChild<QQuickItem *>(QStringLiteral("remoteScreenZoomButton"));
        auto *overlay = root->findChild<QQuickItem *>(QStringLiteral("mediaZoomOverlay"));
        QVERIFY(video && chip && overlay);
        QVERIFY(video->isVisible() && chip->isVisible());
        const QSize stripSize = calls.remoteScreenViewSize();
        QVERIFY(stripSize.width() > 0);
        QVERIFY(stripSize.width() < window->width() / 2);

        clickItem(window, chip);
        QTRY_VERIFY(overlay->property("expanded").toBool());
        QCOMPARE(overlay->property("sourceItem").value<QQuickItem *>(), video);
        auto *copy = root->findChild<QQuickItem *>(QStringLiteral("mediaZoomVideo"));
        QVERIFY(copy);
        QCOMPARE(copy->property("canvas").value<OpenChat::ScreenCanvasPtr>(), canvas);
        const double margin = overlay->property("margin").toDouble();
        const double fitWidth = std::min(window->width() - 2 * margin,
                                         (window->height() - 2 * margin) * 1600.0 / 900.0);
        QCOMPARE(calls.remoteScreenViewSize(),
                 QSize(qRound(fitWidth), qRound(fitWidth * 900.0 / 1600.0)));

        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY2(!overlay->property("expanded").toBool(), "Escape did not close it");
        QCOMPARE(calls.remoteScreenViewSize(), stripSize);
        QTRY_VERIFY(!overlay->isVisible());

        clickItem(window, chip);
        QTRY_VERIFY(overlay->property("expanded").toBool());
        calls.setPreviewScreenShare({}, QString());
        QCoreApplication::processEvents();
        QVERIFY2(!overlay->isVisible(), "the enlarged copy outlived the share");
        QVERIFY(!overlay->property("sourceItem").value<QQuickItem *>());
        // The copy follows its source in C++, so nothing is left on the
        // JavaScript heap waiting for a collection: the pixels go at once.
        std::weak_ptr<OpenChat::ScreenCanvas> observer = canvas;
        canvas.reset();
        QVERIFY2(observer.expired(), "the enlarged copy kept the share's pixels alive");
    }

    void anEnlargedPictureGoesWithTheCallAndLeavesNothingBehind()
    {
        // The call surface is only built while there is a call, so a call that
        // ends with a camera enlarged takes the tile away under the copy. The
        // copy must close with it, and leave nothing holding Escape: the next
        // call's full-window mode still hands the window back on Escape.
        OpenChat::ChatController chats;
        OpenChat::CallController calls;
        calls.enableForPreview(OpenChat::CallState::Active, QStringLiteral("Jessica"),
                               QStringLiteral("jessica"), true, false);
        QImage wide(640, 360, QImage::Format_RGB32);
        wide.fill(Qt::blue);
        calls.setPreviewVideo(wide, wide);

        QQmlApplicationEngine engine;
        engine.setInitialProperties({{QStringLiteral("chatController"), QVariant::fromValue(&chats)},
                                     {QStringLiteral("callController"), QVariant::fromValue(&calls)}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");
        QCOMPARE(engine.rootObjects().size(), 1);
        QObject *root = engine.rootObjects().constFirst();
        QQuickWindow *window = showActiveWindow(root);
        QVERIFY(window);

        auto *remote = root->findChild<QQuickItem *>(QStringLiteral("remoteParticipant"));
        QVERIFY(remote);
        auto *zoomChip = remote->findChild<QQuickItem *>(QStringLiteral("zoomButton"));
        auto *overlay = root->findChild<QQuickItem *>(QStringLiteral("mediaZoomOverlay"));
        QVERIFY(zoomChip && overlay);
        clickItem(window, zoomChip);
        QTRY_VERIFY(overlay->property("expanded").toBool());

        calls.enableForPreview(OpenChat::CallState::Idle, QString(), QString(), false, false);
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(!root->findChild<QQuickItem *>(QStringLiteral("callHeader")));
        QVERIFY2(!overlay->isVisible(), "the enlarged copy outlived the call");
        QVERIFY(!overlay->property("expanded").toBool());

        calls.enableForPreview(OpenChat::CallState::Active, QStringLiteral("Jessica"),
                               QStringLiteral("jessica"), true, false);
        QCoreApplication::processEvents();
        QVERIFY(root->setProperty("callFullscreen", true));
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY2(!root->property("callFullscreen").toBool(),
                     "Escape no longer leaves the full-window call");
    }

    void theCallCanFillTheWholeWindowAndGivesItBackWhenItEnds()
    {
        // The corner chip collapses the sidebar and the conversation and hands
        // the call surface the whole window. Escape or the chip give them
        // back, and so does the end of the call, without being asked.
        OpenChat::ChatController chats;
        OpenChat::CallController calls;
        calls.enableForPreview(OpenChat::CallState::Active, QStringLiteral("Jessica"),
                               QStringLiteral("jessica"), true, false);
        QImage wide(640, 360, QImage::Format_RGB32);
        wide.fill(Qt::blue);
        calls.setPreviewVideo(wide, QImage());

        QQmlApplicationEngine engine;
        engine.setInitialProperties({{QStringLiteral("chatController"), QVariant::fromValue(&chats)},
                                     {QStringLiteral("callController"), QVariant::fromValue(&calls)}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");
        QCOMPARE(engine.rootObjects().size(), 1);
        QObject *root = engine.rootObjects().constFirst();
        QQuickWindow *window = showActiveWindow(root);
        QVERIFY(window);

        auto *chip = root->findChild<QQuickItem *>(QStringLiteral("callFullscreenButton"));
        auto *callHeader = root->findChild<QQuickItem *>(QStringLiteral("callHeader"));
        auto *sidebar = root->findChild<QQuickItem *>(QStringLiteral("contactSidebar"));
        auto *pane = root->findChild<QQuickItem *>(QStringLiteral("conversationPane"));
        auto *slot = root->findChild<QQuickItem *>(QStringLiteral("conversationHeaderSlot"));
        auto *history = root->findChild<QQuickItem *>(QStringLiteral("messageHistory"));
        auto *actions = root->findChild<QQuickItem *>(QStringLiteral("callActions"));
        QVERIFY(chip && callHeader && sidebar && pane && slot && history && actions);
        QVERIFY(chip->isVisible());
        QCOMPARE(chip->property("icon").toString(), QStringLiteral("expand"));
        // In the surface's bottom-right corner, clear of the action row.
        const QPointF corner = chip->mapToItem(callHeader, QPointF(chip->width(), chip->height()));
        QVERIFY(qAbs(corner.x() - (callHeader->width() - 8)) < 1.0);
        QVERIFY(qAbs(corner.y() - (callHeader->height() - 8)) < 1.0);
        QVERIFY(actions->mapToItem(callHeader, QPointF(actions->width(), 0)).x()
                <= chip->mapToItem(callHeader, QPointF()).x());
        const int sidebarWidth = root->property("sidebarWidth").toInt();
        QVERIFY(sidebar->isVisible());
        QCOMPARE(pane->x(), double(sidebarWidth));
        QVERIFY(history->isVisible());
        QVERIFY(slot->height() < window->height() * 0.7);

        clickItem(window, chip);
        QTRY_VERIFY2(root->property("callFullscreen").toBool(),
                     "the corner chip did not fill the window with the call");
        QCOMPARE(chip->property("icon").toString(), QStringLiteral("collapse"));
        QVERIFY(!sidebar->isVisible());
        QVERIFY(!history->isVisible());
        QCOMPARE(pane->x(), 0.0);
        QCOMPARE(pane->width(), double(window->width()));
        QCOMPARE(slot->height(), double(window->height()));
        QCOMPARE(callHeader->height(), double(window->height()));
        // The call's content is centred in the room it now has, and the
        // pictures grow into it: a lone camera row takes most of the height.
        auto *localVideo = root->findChild<QQuickItem *>(QStringLiteral("localParticipant"))
                               ->findChild<QQuickItem *>(QStringLiteral("participantVideo"));
        QVERIFY(localVideo);
        QVERIFY(localVideo->height() > 230);
        QVERIFY(callHeader->property("contentTop").toDouble() >= 18);
        QVERIFY(actions->mapToItem(callHeader, QPointF(0, actions->height())).y()
                < callHeader->height() - 8);

        // Escape gives the window back...
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!root->property("callFullscreen").toBool());
        QVERIFY(sidebar->isVisible());
        QVERIFY(history->isVisible());
        QCOMPARE(pane->x(), double(sidebarWidth));
        QVERIFY(slot->height() < window->height() * 0.7);

        // ...and so does the end of the call, on its own.
        clickItem(window, chip);
        QTRY_VERIFY(root->property("callFullscreen").toBool());
        calls.enableForPreview(OpenChat::CallState::Idle, QString(), QString(), false, false);
        QCoreApplication::processEvents();
        QVERIFY2(!root->property("callFullscreen").toBool(),
                 "the call ended but kept the whole window");
        QVERIFY(sidebar->isVisible());
    }

    // The window a crash relaunches into: it must say in plain words what
    // happened and what OpenChat was doing, before anybody opens the details.
    void aCrashReportWindowSaysWhatHappenedAndWhatOpenChatWasDoing()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("crash-20260923-120513-4242.txt"));
        {
            QFile report(path);
            QVERIFY(report.open(QIODevice::WriteOnly));
            report.write("OpenChat crash report\n"
                         "=====================\n"
                         "\n"
                         "What happened:  The program tried to read memory at 0x10, which is not "
                         "valid: a null pointer.\n"
                         "Where:          nvwgf2umx.dll+0x1a2b3c\n"
                         "Thread:         main (id 4312)\n"
                         "Doing:          screen share: copying the desktop image on the GPU\n"
                         "Hint:           The crash happened inside the NVIDIA graphics driver.\n"
                         "\n"
                         "Application:    OpenChat 0.1.0\n"
                         "Stack of the crashed thread\n");
        }
        OpenChat::CrashReportController report(path, true);
        QCOMPARE(report.headline(), QStringLiteral("OpenChat crashed"));
        QCOMPARE(report.doing(), QStringLiteral("screen share: copying the desktop image on the GPU"));
        QCOMPARE(report.where(), QStringLiteral("nvwgf2umx.dll+0x1a2b3c"));
        QVERIFY(report.hint().contains(QStringLiteral("NVIDIA")));
        // Nothing after the summary block leaks into it.
        QVERIFY(!report.whatHappened().contains(QStringLiteral("Application")));

        QQmlEngine engine;
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        QQmlComponent component(&engine, QUrl::fromLocalFile(
            QStringLiteral(OPENCHAT_SOURCE_DIR "/qml/OpenChat/CrashNotice.qml")));
        std::unique_ptr<QObject> root(component.createWithInitialProperties(
            {{QStringLiteral("report"), QVariant::fromValue(&report)}}));
        auto *window = qobject_cast<QQuickWindow *>(root.get());
        QVERIFY2(window, qPrintable(component.errorString()));
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QCOMPARE(window->title(), QStringLiteral("OpenChat crashed"));
        auto textOf = [&](const char *name) {
            QObject *item = window->findChild<QObject *>(QString::fromLatin1(name));
            return item ? item->property("text").toString() : QString();
        };
        QVERIFY(textOf("crashWhatHappened").contains(QStringLiteral("null pointer")));
        QCOMPARE(textOf("crashDoing"), report.doing());
        QVERIFY(textOf("crashReportPath").contains(path));
        auto *restart = window->findChild<QQuickItem *>(QStringLiteral("crashRestartButton"));
        QVERIFY(restart && restart->isVisible());
        auto *details = window->findChild<QQuickItem *>(QStringLiteral("crashDetails"));
        QVERIFY(details && !details->isVisible());
        auto *toggle = window->findChild<QQuickItem *>(QStringLiteral("crashDetailsButton"));
        QVERIFY(toggle);
        clickItem(window, toggle);
        QTRY_VERIFY(details->isVisible());
        if (qEnvironmentVariableIsSet("OPENCHAT_CAPTURE_CRASH_NOTICE"))
            window->grabWindow().save(qEnvironmentVariable("OPENCHAT_CAPTURE_CRASH_NOTICE"));

        // Closing marks the report seen, so the next launch does not show it
        // again, and closes the window exactly once.
        QSignalSpy finished(&report, &OpenChat::CrashReportController::finished);
        auto *close = window->findChild<QQuickItem *>(QStringLiteral("crashCloseButton"));
        QVERIFY(close);
        clickItem(window, close);
        QTRY_COMPARE(finished.count(), 1);
        QVERIFY(QFileInfo::exists(path + QStringLiteral(".seen")));
        report.dismiss();
        QCOMPARE(finished.count(), 1);
    }

    // A start that fails says why in a window, instead of quitting with the
    // reason written to a console that closes with the process.
    void startupProblemSaysWhyAndCloses()
    {
        QQmlEngine engine;
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        QQmlComponent component(&engine, QUrl::fromLocalFile(
            QStringLiteral(OPENCHAT_SOURCE_DIR "/qml/OpenChat/StartupProblem.qml")));
        const QString explanation = QStringLiteral(
            "OpenChat couldn't open your account's data on this computer. If OpenChat was "
            "just closed, wait a moment and start it again.");
        std::unique_ptr<QObject> root(component.createWithInitialProperties(
            {{QStringLiteral("headline"), QStringLiteral("OpenChat couldn't start")},
             {QStringLiteral("explanation"), explanation}}));
        auto *window = qobject_cast<QQuickWindow *>(root.get());
        QVERIFY2(window, qPrintable(component.errorString()));
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto textOf = [&](const char *name) {
            QObject *item = window->findChild<QObject *>(QString::fromLatin1(name));
            return item ? item->property("text").toString() : QString();
        };
        QCOMPARE(textOf("startupProblemHeadline"), QStringLiteral("OpenChat couldn't start"));
        QCOMPARE(textOf("startupProblemExplanation"), explanation);
        // The whole explanation fits: nothing runs off the bottom.
        auto *explained = window->findChild<QQuickItem *>(QStringLiteral("startupProblemExplanation"));
        QVERIFY(explained);
        QVERIFY(explained->mapToScene(QPointF(0, explained->height())).y() <= window->height());
        if (qEnvironmentVariableIsSet("OPENCHAT_CAPTURE_STARTUP_PROBLEM"))
            window->grabWindow().save(qEnvironmentVariable("OPENCHAT_CAPTURE_STARTUP_PROBLEM"));

        auto *close = window->findChild<QQuickItem *>(QStringLiteral("startupProblemCloseButton"));
        QVERIFY(close);
        clickItem(window, close);
        QTRY_VERIFY(!window->isVisible());
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
    qputenv("XDG_DATA_HOME", settingsDirectory.path().toUtf8());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
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
    qmlRegisterSingletonType<OpenChat::MemorySettings>(
        "OpenChat.Native", 1, 0, "MemorySettings",
        [](QQmlEngine *, QJSEngine *) -> QObject * { return new OpenChat::MemorySettings; });
    qmlRegisterSingletonType<OpenChat::TransportSettings>(
        "OpenChat.Native", 1, 0, "TransportSettings",
        [](QQmlEngine *, QJSEngine *) -> QObject * { return new OpenChat::TransportSettings; });
    qmlRegisterType<OpenChat::BubbleBackground>(
        "OpenChat.Native", 1, 0, "BubbleBackground");
    qmlRegisterType<OpenChat::CallVideoItem>("OpenChat.Native", 1, 0, "CallVideoItem");
    qmlRegisterType<OpenChat::AvatarArtwork>(
        "OpenChat.Native", 1, 0, "AvatarArtwork");
    qmlRegisterType<OpenChat::ComposerEditing>("OpenChat.Native", 1, 0, "ComposerEditing");
    OpenChat::registerCosmeticQmlTypes();
    qmlRegisterUncreatableType<OpenChat::ChatController>(
        "OpenChat.Native", 1, 0, "ChatController",
        QStringLiteral("ChatController is provided by the application"));
    qmlRegisterUncreatableType<OpenChat::OnboardingController>(
        "OpenChat.Native", 1, 0, "OnboardingController",
        QStringLiteral("OnboardingController is provided by the application"));
    qmlRegisterUncreatableType<OpenChat::ContactController>(
        "OpenChat.Native", 1, 0, "ContactController",
        QStringLiteral("ContactController is provided by the application"));
    qmlRegisterUncreatableType<OpenChat::VoiceDebugController>(
        "OpenChat.Native", 1, 0, "VoiceDebugController",
        QStringLiteral("VoiceDebugController is provided by the application"));
    QmlLoadTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_qmlload.moc"
