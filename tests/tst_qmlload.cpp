#include <QColor>
#include <QClipboard>
#include <QDir>
#include <QGuiApplication>
#include <QPointer>
#include <QRegularExpression>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QFile>
#include <QFileInfo>
#include <QBuffer>
#include <QImage>
#include <QMimeData>
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

#include <qpa/qplatformmenu.h>
#include <qpa/qplatformsystemtrayicon.h>

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
#include "render/TrayOrb.h"
#include "app/TrayIcon.h"
#include "app/AppearanceSettings.h"
#include "app/CloseToTray.h"
#include "app/MemorySettings.h"
#include "app/MicrophoneSettings.h"
#include "app/VoiceEffectHost.h"
#include "app/ComposerEditing.h"
#include "app/TextLineSpacing.h"
#include "app/TransportSettings.h"
#include <QQmlExpression>
#include <QQmlContext>
#include "call/ScreenCanvas.h"
#include "case/DailyCaseController.h"
#include "cosmetics/CosmeticTypes.h"
#include "cosmetics/CosmeticCatalog.h"
#include "cosmetics/PeerCosmetics.h"
#include "render/CallVideoItem.h"
#include "render/BubbleBackground.h"
#include "cosmetics/BubbleSkins.h"
#include "controllers/ProfileController.h"
#include "controllers/ProfileReferencePages.h"
#include "profile/ProfilePanelMedia.h"
#include "profile/SongPlayer.h"
#include "ProfileQmlHarness.h"

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

// Every catalogue id, for an account that may wear anything.
QStringList everyCosmeticId()
{
    QStringList ids;
    for (const OpenChat::CosmeticInfo &info : OpenChat::CosmeticCatalog::all())
        ids.append(info.id);
    return ids;
}

// A fresh case account that has unboxed exactly `ids`, to hand the
// window as its dailyCaseAccount: only what it owns can be worn.
QString accountOwning(const QStringList &ids)
{
    const QString account = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!ids.isEmpty() && !OpenChat::LocalCosmeticInventory().grant(account, ids))
        qFatal("Could not grant the test account its cosmetics");
    return account;
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

// A notification area for TrayIcon to drive, recording what it was shown: the
// offscreen platform has none of its own.
class FakeTrayMenuItem final : public QPlatformMenuItem
{
public:
    void setText(const QString &text) override { this->text = text; }
    void setIcon(const QIcon &) override {}
    void setMenu(QPlatformMenu *) override {}
    void setVisible(bool) override {}
    void setIsSeparator(bool) override {}
    void setFont(const QFont &) override {}
    void setRole(MenuRole) override {}
    void setCheckable(bool) override {}
    void setChecked(bool) override {}
    void setShortcut(const QKeySequence &) override {}
    void setEnabled(bool) override {}
    void setIconSize(int) override {}

    QString text;
};

class FakeTrayMenu final : public QPlatformMenu
{
public:
    void insertMenuItem(QPlatformMenuItem *item, QPlatformMenuItem *before) override
    {
        items.insert(before ? items.indexOf(before) : items.size(), item);
    }
    void removeMenuItem(QPlatformMenuItem *item) override { items.removeOne(item); }
    void syncMenuItem(QPlatformMenuItem *) override {}
    void syncSeparatorsCollapsible(bool) override {}
    void setText(const QString &) override {}
    void setIcon(const QIcon &) override {}
    void setEnabled(bool) override {}
    void setVisible(bool) override {}
    QPlatformMenuItem *menuItemAt(int position) const override { return items.value(position); }
    QPlatformMenuItem *menuItemForTag(quintptr tag) const override
    {
        for (QPlatformMenuItem *item : items)
            if (item->tag() == tag)
                return item;
        return nullptr;
    }
    QPlatformMenuItem *createMenuItem() const override { return new FakeTrayMenuItem; }

    QList<QPlatformMenuItem *> items;
};

class FakeTray final : public QPlatformSystemTrayIcon
{
public:
    // Outlives the fake, which the TrayIcon under test owns.
    struct Record
    {
        bool shown = false;
        QIcon icon;
        QString toolTip;
        FakeTrayMenu *menu = nullptr;
    };
    explicit FakeTray(Record *record) : record(record) {}

    void init() override { record->shown = true; }
    void cleanup() override { record->shown = false; }
    void updateIcon(const QIcon &icon) override { record->icon = icon; }
    void updateToolTip(const QString &toolTip) override { record->toolTip = toolTip; }
    void updateMenu(QPlatformMenu *menu) override { record->menu = static_cast<FakeTrayMenu *>(menu); }
    QRect geometry() const override { return {}; }
    void showMessage(const QString &, const QString &, const QIcon &, MessageIcon, int) override {}
    bool isSystemTrayAvailable() const override { return true; }
    bool supportsMessages() const override { return false; }
    QPlatformMenu *createMenu() const override { return new FakeTrayMenu; }

    Record *record;
};

// The colour in the lower half of the icon's 16 px picture, below the gloss.
QColor trayColour(const QIcon &icon)
{
    return icon.pixmap(QSize(16, 16), 1.0).toImage().pixelColor(8, 11);
}

} // namespace

namespace {

// The profile tests' window: Main, with whatever controllers the test hands
// in, shown and exposed; null if it failed to load.
QQuickWindow *showMain(QQmlApplicationEngine &engine, const QVariantMap &properties)
{
    engine.setInitialProperties(properties);
    engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
    engine.loadFromModule("OpenChat", "Main");
    if (engine.rootObjects().size() != 1)
        return nullptr;
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
    if (!window)
        return nullptr;
    window->show();
    window->requestActivate();
    return QTest::qWaitForWindowExposed(window) ? window : nullptr;
}

QPoint centreOf(QQuickItem *item)
{
    return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
}

// The profile page Main shows, once it has faded in; null while none is.
QQuickItem *settledProfilePage(QQuickWindow *window)
{
    auto *page = findVisualItem(window->contentItem(), QStringLiteral("profilePage"));
    return page && page->property("settled").toBool() && page->isVisible() ? page : nullptr;
}

bool profileLoaderActive(QQuickWindow *window)
{
    auto *loader = window->findChild<QQuickItem *>(QStringLiteral("profileLoader"));
    return loader && loader->property("active").toBool();
}

// Saves the window under OPENCHAT_PROFILE_CAPTURES, when set, as <name>.png.
void captureProfileShot(QQuickWindow *window, const QString &name)
{
    const QString directory = qEnvironmentVariable("OPENCHAT_PROFILE_CAPTURES");
    if (directory.isEmpty())
        return;
    QDir().mkpath(directory);
    QVERIFY(window->grabWindow().save(QDir(directory).filePath(name + QStringLiteral(".png"))));
}

// Every warning that points at a line of QML fails the profile tests: a
// broken binding on an avatar or the page is a bug, not noise.
void failOnQmlWarnings()
{
    QTest::failOnWarning(QRegularExpression(QStringLiteral("\\.qml:\\d+")));
}

// Saves the window under OPENCHAT_ATTACHMENT_CAPTURES, when set, as
// <name>.png: the attachment surfaces as the tests leave them.
void captureAttachmentShot(QQuickWindow *window, const QString &name)
{
    const QString directory = qEnvironmentVariable("OPENCHAT_ATTACHMENT_CAPTURES");
    if (directory.isEmpty())
        return;
    QDir().mkpath(directory);
    QVERIFY(window->grabWindow().save(QDir(directory).filePath(name + QStringLiteral(".png"))));
}

// A small real picture on disk, for the attach tests to pick.
QString writeTestPicture(const QTemporaryDir &dir, const QString &name, const QColor &colour)
{
    QImage image(160, 120, QImage::Format_RGB32);
    image.fill(colour);
    const QString path = dir.filePath(name);
    return image.save(path) ? path : QString();
}

// Lays `items` out one under another (or side by side) in a window and
// saves it under OPENCHAT_ATTACHMENT_CAPTURES as <name>.png, when that is set.
void captureItems(const QList<QQuickItem *> &items, const QString &name, bool across, const QColor &background)
{
    const QString directory = qEnvironmentVariable("OPENCHAT_ATTACHMENT_CAPTURES");
    if (directory.isEmpty())
        return;
    QQuickWindow window;
    window.setColor(background);
    qreal x = 12;
    qreal y = 12;
    qreal width = 0;
    qreal height = 0;
    for (QQuickItem *item : items) {
        item->setParentItem(window.contentItem());
        item->setPosition(QPointF(x, y));
        const qreal itemHeight = item->height() > 0 ? item->height() : item->implicitHeight();
        if (across) {
            x += item->width() + 10;
            width = x;
            height = std::max(height, y + itemHeight + 12);
        } else {
            y += itemHeight + 8;
            width = std::max(width, x + item->width() + 12);
            height = y + 4;
        }
    }
    window.resize(int(std::ceil(width)), int(std::ceil(height)));
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QTest::qWait(300);
    QDir().mkpath(directory);
    QVERIFY(window.grabWindow().save(QDir(directory).filePath(name + QStringLiteral(".png"))));
    for (QQuickItem *item : items)
        item->setParentItem(nullptr);
}

// A sound device that plays nothing: the chat's audio player pulls from it
// only when a test reads the stream it was given.
class SilentSongOutput final : public OpenChat::SongOutput
{
public:
    explicit SilentSongOutput(int channels, QIODevice **stream) : m_stream(stream)
    {
        m_format.setSampleRate(48000);
        m_format.setChannelCount(channels);
        m_format.setSampleFormat(QAudioFormat::Int16);
    }
    ~SilentSongOutput() override { *m_stream = nullptr; }
    [[nodiscard]] QAudioFormat format() const override { return m_format; }
    [[nodiscard]] int bufferMs() const override { return 0; }
    bool start(QIODevice *stream) override
    {
        *m_stream = stream;
        return true;
    }
    void stop() override { *m_stream = nullptr; }

private:
    QAudioFormat m_format;
    QIODevice **m_stream;
};

// A JPEG of `size` in PanelMediaLibrary under `key`, for a picture to show.
void putTestPicture(const QString &key, QSize size, const QColor &top, const QColor &bottom)
{
    QImage image(size, QImage::Format_RGB32);
    for (int row = 0; row < size.height(); ++row) {
        const qreal t = qreal(row) / std::max(1, size.height() - 1);
        const QColor colour = QColor::fromRgbF(top.redF() + (bottom.redF() - top.redF()) * t,
                                               top.greenF() + (bottom.greenF() - top.greenF()) * t,
                                               top.blueF() + (bottom.blueF() - top.blueF()) * t);
        for (int column = 0; column < size.width(); ++column)
            image.setPixelColor(column, row, colour);
    }
    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPEG", 85);
    OpenChat::PanelMediaLibrary::instance().put(key, jpeg);
}

// Every Text and TextEdit under `root` showing `needle`: what peer strings
// reach the screen through.
QList<QQuickItem *> textItemsShowing(QQuickItem *root, const QString &needle)
{
    QList<QQuickItem *> found;
    const auto visit = [&](const auto &self, QQuickItem *item) -> void {
        const QMetaObject *meta = item->metaObject();
        if (meta->indexOfProperty("textFormat") >= 0
            && item->property("text").toString().contains(needle))
            found.append(item);
        for (QQuickItem *child : item->childItems())
            self(self, child);
    };
    visit(visit, root);
    return found;
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
        settings.remove(QStringLiteral("Appearance/plainProfiles"));
        OpenChat::ProfileQmlHarness::resetProfileSingletons();
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
        auto *offer = findVisualItem(window->contentItem(), "caseStatus");
        QVERIFY(offer);
        QVERIFY(offer->property("text").toString().contains(QStringLiteral("every 30 minutes while OpenChat is open")));
        QCOMPARE(open->property("text").toString(), QStringLiteral("Open case"));
        // A new account's first case waits, and the popup counts it.
        auto *waiting = findVisualItem(window->contentItem(), "caseDrops");
        QVERIFY(waiting);
        QCOMPARE(controller->drops(), 1);
        QVERIFY(waiting->property("text").toString().startsWith(QStringLiteral("1 case waiting")));
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
        QTRY_COMPARE_WITH_TIMEOUT(controller->state(), OpenChat::DailyCaseController::Opened, 8500);
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
        // None waits now; the next drops half an hour of running from the
        // start, and the button and the count say when.
        QCOMPARE(controller->drops(), 0);
        const QDateTime next = controller->nextDropAt();
        QVERIFY(qAbs(QDateTime::currentDateTimeUtc().msecsTo(next) - OpenChat::caseDropIntervalMs) < 60 * 1000);
        QVERIFY(!open->isEnabled());
        QVERIFY(open->property("text").toString().startsWith(QStringLiteral("Next at ")));
        QVERIFY(waiting->property("text").toString().startsWith(QStringLiteral("Next case drops at ")));
        // And it is the account's to keep: now in its collection, and wearable.
        QVERIFY(controller->owned().contains(reward.value("id").toString()));
        auto *wardrobe = engine.singletonInstance<OpenChat::AppearanceSettings *>(
            qmlTypeId("OpenChat.Native", 1, 0, "AppearanceSettings"));
        QVERIFY(wardrobe);
        QVERIFY(wardrobe->owns(reward.value("id").toString()));
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
        QCOMPARE(controller->state(), OpenChat::DailyCaseController::Opened);
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

        // Drops stack: both waiting cases are counted, and after a reveal the
        // next opens from the same popup, from a belt of its own.
        const QString stacked = QUuid::createUuid().toString();
        QCOMPARE(OpenChat::LocalDailyCaseService().accrue(stacked, OpenChat::caseDropIntervalMs).drops, 2);
        controller->setProperty("reducedMotion", true);
        controller->setAccountKey(stacked);
        QCOMPARE(controller->drops(), 2);
        click(entry);
        QTRY_VERIFY((reel = findVisualItem(window->contentItem(), "caseReel")));
        auto *count = findVisualItem(window->contentItem(), "caseDrops");
        open = findVisualItem(window->contentItem(), "caseOpenButton");
        QVERIFY(count && open);
        QVERIFY(count->property("text").toString().startsWith(QStringLiteral("2 cases waiting")));
        click(open);
        QTRY_COMPARE(controller->state(), OpenChat::DailyCaseController::Opened);
        QCOMPARE(controller->drops(), 1);
        QVERIFY(count->property("text").toString().startsWith(QStringLiteral("1 case waiting")));
        QTRY_VERIFY(open->isEnabled());
        QCOMPARE(open->property("text").toString(), QStringLiteral("Open next case"));
        const QVariantList firstBelt = controller->fillers();
        click(open);
        QTRY_COMPARE(reveal.size(), 3);
        QCOMPARE(controller->state(), OpenChat::DailyCaseController::Opened);
        QCOMPARE(controller->drops(), 0);
        QVERIFY(controller->fillers() != firstBelt);
        QVERIFY(!open->isEnabled());
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!findVisualItem(window->contentItem(), "caseReel"));
        controller->setProperty("reducedMotion", false);
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
        const QString account = accountOwning({QStringLiteral("frame.gilded"), QStringLiteral("frame.neon"),
                                               QStringLiteral("bead.gem"), QStringLiteral("flair.holo"),
                                               QStringLiteral("scene.aurora")});
        const auto measure = [&account](QQmlApplicationEngine &engine, OpenChat::ChatController &controller) {
            engine.setInitialProperties(
                {{QStringLiteral("chatController"), QVariant::fromValue(&controller)},
                 {QStringLiteral("dailyCaseAccount"), account}});
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
        const QStringList owned{QStringLiteral("frame.pixel"), QStringLiteral("bead.star"),
                                QStringLiteral("bead.gem")};
        {
            OpenChat::AppearanceSettings appearance;
            appearance.setOwnedCosmetics(owned);
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
        reloaded.setOwnedCosmetics(owned);
        QCOMPARE(reloaded.avatarFrame(), QStringLiteral("frame.pixel"));
        QCOMPARE(reloaded.profileScene(), QString());
        clearEquippedCosmetics();
    }

    // Only what the account has unboxed is worn or can be equipped: nothing
    // until ownership is known, then the saved choice if it is owned; an
    // unowned choice is taken off and forgotten, and equipping one is refused.
    void appearanceSettingsWearOnlyWhatIsOwned()
    {
        clearAllCosmetics();
        QSettings().setValue(QStringLiteral("Appearance/avatarFrame"), QStringLiteral("frame.neon"));
        QSettings().setValue(QStringLiteral("Appearance/bubbleSkin"), QStringLiteral("bubble.magma"));
        OpenChat::AppearanceSettings appearance;
        QSignalSpy frameChanged(&appearance, &OpenChat::AppearanceSettings::avatarFrameChanged);
        QSignalSpy bubbleChanged(&appearance, &OpenChat::AppearanceSettings::bubbleSkinChanged);

        // Unknown ownership: nothing worn, nothing equippable, nothing forgotten.
        QCOMPARE(appearance.avatarFrame(), QString());
        QCOMPARE(appearance.bubbleSkin(), QString());
        appearance.setPresenceBead(QStringLiteral("bead.gem"));
        QCOMPARE(appearance.presenceBead(), QString());
        QVERIFY(!QSettings().contains(QStringLiteral("Appearance/presenceBead")));
        QCOMPARE(QSettings().value(QStringLiteral("Appearance/avatarFrame")).toString(),
                 QStringLiteral("frame.neon"));

        // Known: the owned frame is worn at once; the unowned skin is dropped
        // for good. Ids this build does not know own nothing.
        appearance.setOwnedCosmetics({QStringLiteral("frame.neon"), QStringLiteral("bead.star"),
                                      QStringLiteral("frame.retired")});
        QCOMPARE(appearance.ownedCosmetics(),
                 (QStringList{QStringLiteral("frame.neon"), QStringLiteral("bead.star")}));
        QCOMPARE(appearance.avatarFrame(), QStringLiteral("frame.neon"));
        QCOMPARE(frameChanged.count(), 1);
        QCOMPARE(appearance.bubbleSkin(), QString());
        QCOMPARE(bubbleChanged.count(), 0);
        QVERIFY(!QSettings().contains(QStringLiteral("Appearance/bubbleSkin")));

        // Equipping something not unboxed leaves the choice alone.
        appearance.setAvatarFrame(QStringLiteral("frame.inferno"));
        QCOMPARE(appearance.avatarFrame(), QStringLiteral("frame.neon"));
        QCOMPARE(QSettings().value(QStringLiteral("Appearance/avatarFrame")).toString(),
                 QStringLiteral("frame.neon"));
        appearance.setPresenceBead(QStringLiteral("bead.star"));
        QCOMPARE(appearance.presenceBead(), QStringLiteral("bead.star"));

        // Losing an item takes it off.
        appearance.setOwnedCosmetics({QStringLiteral("bead.star")});
        QCOMPARE(appearance.avatarFrame(), QString());
        QCOMPARE(frameChanged.count(), 2);
        QVERIFY(!QSettings().contains(QStringLiteral("Appearance/avatarFrame")));
        QCOMPARE(appearance.presenceBead(), QStringLiteral("bead.star"));
        clearAllCosmetics();
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
        const QString account = accountOwning({id});
        QQmlApplicationEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        engine.setInitialProperties({{QStringLiteral("chatController"), QVariant::fromValue(&controller)},
                                     {QStringLiteral("dailyCaseAccount"), account}});
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
            restarted.setInitialProperties({{QStringLiteral("chatController"), QVariant::fromValue(&again)},
                                            {QStringLiteral("dailyCaseAccount"), account}});
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
        const QString account = accountOwning({id});
        const auto grabTiles = [&account](const QString &frame, QImage *localTile, QImage *remoteTile,
                                          bool *captionClear, QString *equippedId, int *remoteFrames) {
            QSettings().setValue(QStringLiteral("Appearance/avatarFrame"), frame);
            OpenChat::ChatController chats;
            OpenChat::CallController calls;
            calls.enableForPreview(OpenChat::CallState::Active, QStringLiteral("Jessica"),
                                   QStringLiteral("jessica"), false, false);
            QQmlApplicationEngine engine;
            engine.setInitialProperties({{QStringLiteral("chatController"), QVariant::fromValue(&chats)},
                                         {QStringLiteral("callController"), QVariant::fromValue(&calls)},
                                         {QStringLiteral("dailyCaseAccount"), account}});
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

        controller.setCurrentSettingsCategory(1);
        QCoreApplication::processEvents();
        QCOMPARE(settingsDetailTitle->property("text").toString(),
                 QStringLiteral("Audio & Video"));

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

    void settingsListCategoriesBesideTheOpenCategorysControls()
    {
        OpenChat::ChatController chats;
        chats.setLocalUserName(QStringLiteral("Developer"));
        OpenChat::ContactController contacts;
        contacts.enableForPreview();
        OpenChat::CallController calls;
        // Owns what the capture below wears; the rest of the collection is locked.
        const QString account = accountOwning({QStringLiteral("frame.neon"), QStringLiteral("flair.holo"),
                                               QStringLiteral("bead.gem"), QStringLiteral("scene.aurora"),
                                               QStringLiteral("bubble.nebula")});
        QQmlApplicationEngine engine;
        engine.setInitialProperties({{QStringLiteral("chatController"), QVariant::fromValue(&chats)},
                                     {QStringLiteral("contactController"), QVariant::fromValue(&contacts)},
                                     {QStringLiteral("callController"), QVariant::fromValue(&calls)},
                                     {QStringLiteral("dailyCaseAccount"), account}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");
        QCOMPARE(engine.rootObjects().size(), 1);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        const auto flushDeletes = [] {
            QCoreApplication::processEvents();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        };

        // Out of Settings, no section's control is built: Input has a device
        // behind it.
        QVERIFY(!findVisualItem(window->contentItem(), QStringLiteral("lowMemoryPanel")));
        chats.setNavSection(OpenChat::ChatController::NavSection::Settings);
        QCoreApplication::processEvents();

        // Only categories with working controls, each holding only those.
        const QStringList categories = chats.settingsCategories();
        QCOMPARE(categories, (QStringList{QStringLiteral("General"),
                                          QStringLiteral("Audio & Video"),
                                          QStringLiteral("Appearance"),
                                          QStringLiteral("Cosmetics")}));
        const QHash<QString, QString> controlOf = {
            {QStringLiteral("Memory"), QStringLiteral("lowMemoryPanel")},
            {QStringLiteral("Input"), QStringLiteral("microphoneSettingsPanel")},
            {QStringLiteral("Custom Vocal FX"), QStringLiteral("customVocalFxPanel")},
            {QStringLiteral("Connection"), QStringLiteral("connectionSettingsPanel")},
            {QStringLiteral("Theme"), QStringLiteral("darkModeSwitch")},
            {QStringLiteral("Profiles"), QStringLiteral("plainProfilesSwitch")},
            {QStringLiteral("Avatar frame"), QStringLiteral("cosmeticPicker_frame")},
            {QStringLiteral("Name flair"), QStringLiteral("cosmeticPicker_flair")},
            {QStringLiteral("Presence bead"), QStringLiteral("cosmeticPicker_bead")},
            {QStringLiteral("Profile scene"), QStringLiteral("cosmeticPicker_scene")},
            {QStringLiteral("Chat bubble"), QStringLiteral("cosmeticPicker_bubble")},
        };

        // The sidebar lists exactly the categories, with no Back row, and
        // stays that way whichever is open.
        QVERIFY(!findVisualItem(window->contentItem(),
                                QStringLiteral("settingsCategoryRow_%1").arg(categories.size())));
        auto *title = findVisualItem(window->contentItem(), QStringLiteral("settingsDetailTitle"));
        QVERIFY(title && title->isVisible());

        for (int i = 0; i < categories.size(); ++i) {
            auto *row = findVisualItem(window->contentItem(),
                                       QStringLiteral("settingsCategoryRow_%1").arg(i));
            QVERIFY(row && row->isVisible());
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                              row->mapToScene(QPointF(row->width() / 2, row->height() / 2)).toPoint());
            QTRY_COMPARE(chats.currentSettingsCategory(), i);
            flushDeletes();
            QCOMPARE(title->property("text").toString(), categories.at(i));
            for (int j = 0; j < categories.size(); ++j) {
                auto *other = findVisualItem(window->contentItem(),
                                             QStringLiteral("settingsCategoryRow_%1").arg(j));
                QVERIFY(other && other->isVisible());
                QCOMPARE(other->property("selected").toBool(), i == j);
            }

            // The page holds every section of the open category at once, each
            // a heading over its control, and no other category's controls.
            const QStringList sections = chats.currentSettingsElements();
            QVERIFY(!sections.isEmpty());
            for (const QString &name : sections) {
                QVERIFY2(controlOf.contains(name), qPrintable(name + QStringLiteral(" has no working control")));
                auto *section = findVisualItem(window->contentItem(), QStringLiteral("settingsSection_") + name);
                auto *heading = findVisualItem(window->contentItem(), QStringLiteral("settingsSectionTitle_") + name);
                auto *control = findVisualItem(window->contentItem(), controlOf.value(name));
                QVERIFY2(section && section->isVisible(), qPrintable(name));
                QVERIFY2(heading && heading->isVisible(), qPrintable(name));
                QCOMPARE(heading->property("text").toString(), name);
                QVERIFY2(control && control->isVisible(), qPrintable(name));
                QVERIFY(section->isAncestorOf(control));
                QVERIFY(control->height() > 0);
            }
            for (auto it = controlOf.cbegin(); it != controlOf.cend(); ++it) {
                if (!sections.contains(it.key()))
                    QVERIFY2(!findVisualItem(window->contentItem(), it.value()),
                             qPrintable(it.value()));
            }
        }

        // Set OPENCHAT_SETTINGS_CAPTURE_DIR to keep every category, light and
        // dark, every screenful of any page that scrolls, and every category
        // at the narrowest window, for eyeballing.
        const QString captureDir = qEnvironmentVariable("OPENCHAT_SETTINGS_CAPTURE_DIR");
        if (!captureDir.isEmpty()) {
            auto *appearance = engine.singletonInstance<OpenChat::AppearanceSettings *>(
                "OpenChat.Native", "AppearanceSettings");
            auto *scroll = findVisualItem(window->contentItem(), QStringLiteral("settingsScroll"));
            QVERIFY(appearance && scroll);
            const auto shoot = [&](const QString &name) {
                QTest::qWait(120);
                return window->grabWindow().save(captureDir + QLatin1Char('/') + name
                                                 + QStringLiteral(".png"));
            };
            // The open category's top, then each further screenful.
            const auto shootPage = [&](const QString &name) {
                scroll->setProperty("contentY", 0);
                bool saved = shoot(name);
                const qreal overflow = scroll->property("contentHeight").toReal() - scroll->height();
                for (int screen = 1; overflow > 0 && (screen - 1) * scroll->height() < overflow; ++screen) {
                    scroll->setProperty("contentY", qMin(overflow, screen * scroll->height()));
                    saved = shoot(name + QStringLiteral("-%1").arg(screen)) && saved;
                }
                return saved;
            };
            for (const bool dark : {false, true}) {
                appearance->setDarkMode(dark);
                const QString theme = dark ? QStringLiteral("dark") : QStringLiteral("light");
                for (int i = 0; i < categories.size(); ++i) {
                    chats.setCurrentSettingsCategory(i);
                    flushDeletes();
                    QVERIFY(shootPage(QStringLiteral("settings-%1-%2").arg(i).arg(theme)));
                }
                // Cosmetics again, wearing one of each kind.
                appearance->setAvatarFrame(QStringLiteral("frame.neon"));
                appearance->setNameFlair(QStringLiteral("flair.holo"));
                appearance->setPresenceBead(QStringLiteral("bead.gem"));
                appearance->setProfileScene(QStringLiteral("scene.aurora"));
                appearance->setBubbleSkin(QStringLiteral("bubble.nebula"));
                chats.setCurrentSettingsCategory(int(categories.indexOf(QStringLiteral("Cosmetics"))));
                flushDeletes();
                QVERIFY(shootPage(QStringLiteral("settings-cosmetics-equipped-%1").arg(theme)));
                for (const char *property : {"avatarFrame", "nameFlair", "presenceBead",
                                             "profileScene", "bubbleSkin"})
                    appearance->setProperty(property, QString());
            }
            appearance->setDarkMode(false);
            const QSize size = window->size();
            window->resize(window->minimumWidth(), size.height());
            for (int i = 0; i < categories.size(); ++i) {
                chats.setCurrentSettingsCategory(i);
                flushDeletes();
                QVERIFY(shoot(QStringLiteral("settings-%1-narrow").arg(i)));
            }
            window->resize(size);
        }

        // Leaving Settings tears the controls down again; coming back finds
        // the category that was open.
        chats.setNavSection(OpenChat::ChatController::NavSection::Chat);
        flushDeletes();
        QVERIFY(!findVisualItem(window->contentItem(), QStringLiteral("cosmeticPicker_frame")));
        chats.setNavSection(OpenChat::ChatController::NavSection::Settings);
        QCoreApplication::processEvents();
        QCOMPARE(title->property("text").toString(), QStringLiteral("Cosmetics"));
        auto *picker = findVisualItem(window->contentItem(), QStringLiteral("cosmeticPicker_frame"));
        QVERIFY(picker && picker->isVisible());
    }

    // Settings → Cosmetics: each kind lists None and then every item of it,
    // lowest tier first; a click or a key equips an unboxed tile at once (the
    // header wears it and it is remembered), the check follows the equipped
    // tile, and None puts the stock look back. What the account has not
    // unboxed stays locked until the case hands it over.
    void theCosmeticsPageEquipsWhatYouPick()
    {
        clearAllCosmetics();
        const QStringList kinds{QStringLiteral("frame"), QStringLiteral("flair"), QStringLiteral("bead"),
                                QStringLiteral("scene"), QStringLiteral("bubble")};
        // Each kind in picker order; the account owns all but its first item.
        QHash<QString, QStringList> orderOf;
        QStringList owned;
        for (const QString &category : kinds) {
            QList<OpenChat::CosmeticInfo> items = OpenChat::CosmeticCatalog::inCategory(category);
            std::stable_sort(items.begin(), items.end(), [](const auto &a, const auto &b) {
                return a.rarity < b.rarity;
            });
            QStringList order{category + QStringLiteral(".none")};
            for (const OpenChat::CosmeticInfo &info : items)
                order.append(info.id);
            orderOf.insert(category, order);
            owned += order.mid(2);
        }
        const QString account = accountOwning(owned);
        OpenChat::ChatController chats;
        chats.setLocalUserName(QStringLiteral("Developer"));
        chats.setNavSection(OpenChat::ChatController::NavSection::Settings);
        chats.setCurrentSettingsCategory(3); // Cosmetics
        QQmlApplicationEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        engine.setInitialProperties({{QStringLiteral("chatController"), QVariant::fromValue(&chats)},
                                     {QStringLiteral("dailyCaseAccount"), account}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");
        QCOMPARE(engine.rootObjects().size(), 1);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
        QVERIFY(window);
        // The narrowest window leaves the tiles least room.
        window->resize(window->minimumWidth(), 680);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
        QObject *appearance = engine.singletonInstance<QObject *>("OpenChat.Native", "AppearanceSettings");
        QVERIFY(appearance);
        QQuickItem *root = window->contentItem();

        // Said once for the page: where cosmetics come from, and that the
        // people you chat with see them.
        auto *note = findVisualItem(root, QStringLiteral("settingsCategoryNote"));
        QVERIFY(note && note->isVisible());
        QVERIFY(note->property("text").toString().contains(QStringLiteral("cases that drop")));
        QVERIFY(note->property("text").toString().contains(QStringLiteral("see what you wear")));
        QTRY_COMPARE(appearance->property("ownedCosmetics").toStringList(), owned);

        auto *scroll = findVisualItem(root, QStringLiteral("settingsScroll"));
        QVERIFY(scroll);
        auto *content = scroll->property("contentItem").value<QQuickItem *>();
        QVERIFY(content);
        const auto bringIntoView = [&](QQuickItem *item) {
            const qreal top = item->mapToItem(content, QPointF(0, 0)).y();
            const qreal most = qMax<qreal>(0, scroll->property("contentHeight").toReal() - scroll->height());
            scroll->setProperty("contentY", qBound<qreal>(0, top - 24, most));
            QCoreApplication::processEvents();
        };
        const auto inView = [&](QQuickItem *item) {
            const QRectF box = item->mapRectToItem(scroll, QRectF(0, 0, item->width(), item->height()));
            return box.top() >= 0 && box.bottom() <= scroll->height();
        };
        const auto click = [&](QQuickItem *item) {
            bringIntoView(item);
            QVERIFY(inView(item));
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                              item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint());
        };
        auto *avatar = qobject_cast<QQuickItem *>(window->findChild<QObject *>(QStringLiteral("localUserAvatar")));
        QVERIFY(avatar);
        QQuickItem *header = avatar->parentItem();
        // What the local header wears for a kind, or empty when it wears nothing.
        const auto worn = [&](const QString &category) -> QString {
            QObject *item = nullptr;
            const char *idProperty = "";
            if (category == QLatin1String("frame")) {
                item = findVisualItem(header, QStringLiteral("avatarFrame"));
                idProperty = "frameId";
            } else if (category == QLatin1String("bead")) {
                item = findVisualItem(header, QStringLiteral("beadArt"));
                idProperty = "styleId";
            } else if (category == QLatin1String("flair")) {
                item = window->findChild<QObject *>(QStringLiteral("localNameFlair"));
                idProperty = "flairId";
            } else if (category == QLatin1String("scene")) {
                item = window->findChild<QObject *>(QStringLiteral("profileScene"));
                idProperty = "sceneId";
            }
            return item ? item->property(idProperty).toString() : QString();
        };

        for (const QString &category : kinds) {
            auto *picker = findVisualItem(root, QStringLiteral("cosmeticPicker_") + category);
            QVERIFY2(picker && picker->isVisible(), qPrintable(category));
            const QByteArray property = equipProperty(category);

            // None, then the kind's items by tier, reading left to right.
            const QStringList order = orderOf.value(category);
            QList<QQuickItem *> tiles;
            for (const QString &id : order) {
                auto *tile = findVisualItem(picker, QStringLiteral("cosmeticChoice_") + id);
                QVERIFY2(tile && tile->isVisible(), qPrintable(id));
                tiles.append(tile);
                // Name and tier read in full, a play mark flags exactly the
                // items that move, and a padlock exactly those not unboxed.
                auto *name = findVisualItem(tile, QStringLiteral("cosmeticChoiceName"));
                auto *rarity = findVisualItem(tile, QStringLiteral("cosmeticChoiceRarity"));
                auto *animated = findVisualItem(tile, QStringLiteral("cosmeticChoiceAnimated"));
                auto *lock = findVisualItem(tile, QStringLiteral("cosmeticChoiceLock"));
                auto *art = findVisualItem(tile, QStringLiteral("cosmeticChoiceArt"));
                QVERIFY(name && rarity && animated && lock && art);
                QVERIFY2(!name->property("truncated").toBool(), qPrintable(id));
                QVERIFY2(!rarity->property("truncated").toBool(), qPrintable(id));
                const OpenChat::CosmeticInfo *info = OpenChat::CosmeticCatalog::find(id);
                QCOMPARE(animated->isVisible(), info && info->animated);
                const bool locked = id == order[1];
                QCOMPARE(lock->isVisible(), locked);
                QCOMPARE(tile->property("locked").toBool(), locked);
                QCOMPARE(art->opacity() < 1, locked);
            }
            auto *count = findVisualItem(picker, QStringLiteral("cosmeticPickerCount"));
            QVERIFY(count);
            QCOMPARE(count->property("text").toString(),
                     QStringLiteral("%1 of %2 unboxed").arg(order.size() - 2).arg(order.size() - 1));
            const int columns = picker->property("columns").toInt();
            QVERIFY(columns >= 3);
            for (int i = 1; i < tiles.size(); ++i) {
                const QPointF previous = tiles[i - 1]->mapToItem(picker, QPointF(0, 0));
                const QPointF here = tiles[i]->mapToItem(picker, QPointF(0, 0));
                QVERIFY2(here.y() > previous.y() || (here.y() == previous.y() && here.x() > previous.x()),
                         qPrintable(order[i] + QStringLiteral(" is out of order")));
                QCOMPARE(here.y() > previous.y(), i % columns == 0);
                // Tiles fill the width, and never spill past it.
                QVERIFY(here.x() + tiles[i]->width() <= picker->width() + 0.5);
            }

            // An animated frame's preview holds still until pointed at.
            if (category == QLatin1String("frame")) {
                const int orbit = int(order.indexOf(QStringLiteral("frame.orbit")));
                const int inferno = int(order.indexOf(QStringLiteral("frame.inferno")));
                QVERIFY(orbit > 0 && inferno > 0);
                auto *orbitFrame = findVisualItem(tiles[orbit], QStringLiteral("avatarFrame"));
                auto *infernoFrame = findVisualItem(tiles[inferno], QStringLiteral("avatarFrame"));
                QVERIFY(orbitFrame && infernoFrame);
                bringIntoView(tiles[orbit]);
                const int orbitPhase = orbitFrame->property("phase").toInt();
                const int infernoPhase = infernoFrame->property("phase").toInt();
                QTest::mouseMove(window, tiles[orbit]->mapToScene(
                    QPointF(tiles[orbit]->width() / 2, tiles[orbit]->height() / 2)).toPoint());
                QTRY_VERIFY(orbitFrame->property("phase").toInt() != orbitPhase);
                QCOMPARE(infernoFrame->property("phase").toInt(), infernoPhase);
                QTest::mouseMove(window, QPoint(2, window->height() - 2));
            }

            // Nothing equipped: None carries the check.
            const auto checked = [&](int index) {
                for (int i = 0; i < tiles.size(); ++i) {
                    auto *check = findVisualItem(tiles[i], QStringLiteral("cosmeticChoiceCheck"));
                    if (tiles[i]->property("selected").toBool() != (i == index)
                        || !check || check->isVisible() != (i == index))
                        return false;
                }
                return true;
            };
            QVERIFY(checked(0));
            QCOMPARE(worn(category), QString());

            // A locked item cannot be put on: not by a click, not by a key,
            // and not by asking AppearanceSettings directly.
            click(tiles[1]);
            tiles[1]->forceActiveFocus(Qt::TabFocusReason);
            QTest::keyClick(window, Qt::Key_Space);
            QVERIFY(!appearance->setProperty(property.constData(), order[1])
                    || appearance->property(property.constData()).toString().isEmpty());
            QCoreApplication::processEvents();
            QCOMPARE(appearance->property(property.constData()).toString(), QString());
            QVERIFY(!QSettings().contains(QStringLiteral("Appearance/") + QString::fromLatin1(property)));
            QVERIFY(checked(0));
            QCOMPARE(worn(category), QString());

            // A click equips the rarest item, remembered and worn at once.
            const int rarest = int(tiles.size()) - 1;
            click(tiles[rarest]);
            QTRY_COMPARE(appearance->property(property.constData()).toString(), order[rarest]);
            QCOMPARE(QSettings().value(QStringLiteral("Appearance/") + QString::fromLatin1(property)).toString(),
                     order[rarest]);
            QVERIFY(checked(rarest));
            if (category != QLatin1String("bubble"))
                QTRY_COMPARE(worn(category), order[rarest]);

            // The keyboard: Left walks back a tile, Space equips it; Up and
            // Down move by a row.
            tiles[rarest]->forceActiveFocus(Qt::TabFocusReason);
            QTest::keyClick(window, Qt::Key_Left);
            QCOMPARE(window->activeFocusItem(), tiles[rarest - 1]);
            QTest::keyClick(window, Qt::Key_Space);
            QTRY_COMPARE(appearance->property(property.constData()).toString(), order[rarest - 1]);
            QVERIFY(checked(rarest - 1));
            tiles[0]->forceActiveFocus(Qt::TabFocusReason);
            QTest::keyClick(window, Qt::Key_Down);
            QCOMPARE(window->activeFocusItem(), tiles[columns]);
            QTest::keyClick(window, Qt::Key_Up);
            QCOMPARE(window->activeFocusItem(), tiles[0]);
            QTest::keyClick(window, Qt::Key_Up); // Already on the top row.
            QCOMPARE(window->activeFocusItem(), tiles[0]);

            // None clears it and the stock look returns.
            click(tiles[0]);
            QTRY_COMPARE(appearance->property(property.constData()).toString(), QString());
            QVERIFY(!QSettings().contains(QStringLiteral("Appearance/") + QString::fromLatin1(property)));
            QVERIFY(checked(0));
            QTRY_COMPARE(worn(category), QString());
        }

        // Tab stops once per kind, on its equipped tile, and the page scrolls
        // to show it.
        appearance->setProperty("nameFlair", QStringLiteral("flair.holo"));
        auto *frameNone = findVisualItem(root, QStringLiteral("cosmeticChoice_frame.none"));
        QVERIFY(frameNone);
        bringIntoView(frameNone);
        frameNone->forceActiveFocus(Qt::TabFocusReason);
        QTest::keyClick(window, Qt::Key_Tab);
        QTRY_COMPARE(window->activeFocusItem()->objectName(), QStringLiteral("cosmeticChoice_flair.holo"));
        QTest::keyClick(window, Qt::Key_Tab);
        QTRY_COMPARE(window->activeFocusItem()->objectName(), QStringLiteral("cosmeticChoice_bead.none"));
        QTest::keyClick(window, Qt::Key_Tab);
        QTest::keyClick(window, Qt::Key_Tab);
        QTRY_COMPARE(window->activeFocusItem()->objectName(), QStringLiteral("cosmeticChoice_bubble.none"));
        QTRY_VERIFY(inView(window->activeFocusItem()));

        // Once the case's authority hands a locked item over, it unlocks on
        // the open page and can be worn.
        const QString lockedFrame = orderOf.value(QStringLiteral("frame")).at(1);
        auto *lockedTile = findVisualItem(root, QStringLiteral("cosmeticChoice_") + lockedFrame);
        auto *frameCount = findVisualItem(findVisualItem(root, QStringLiteral("cosmeticPicker_frame")),
                                          QStringLiteral("cosmeticPickerCount"));
        auto *dailyCase = window->findChild<OpenChat::DailyCaseController *>(QStringLiteral("dailyCaseController"));
        QVERIFY(lockedTile && frameCount && dailyCase);
        QVERIFY(lockedTile->property("locked").toBool());
        QVERIFY(OpenChat::LocalCosmeticInventory().grant(account, {lockedFrame}));
        dailyCase->refresh();
        QTRY_VERIFY(!lockedTile->property("locked").toBool());
        QVERIFY(!findVisualItem(lockedTile, QStringLiteral("cosmeticChoiceLock"))->isVisible());
        const int frames = int(orderOf.value(QStringLiteral("frame")).size()) - 1;
        QCOMPARE(frameCount->property("text").toString(),
                 QStringLiteral("%1 of %1 unboxed").arg(frames));
        click(lockedTile);
        QTRY_COMPARE(appearance->property("avatarFrame").toString(), lockedFrame);
        QTRY_COMPARE(worn(QStringLiteral("frame")), lockedFrame);

        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.isEmpty() ? QString()
            : warnings.constFirst().constFirst().value<QList<QQmlError>>().value(0).toString()));
        clearAllCosmetics();
    }

    void theMicrophonePanelDrivesAndRemembersTheSettings()
    {
        OpenChat::ChatController chats;
        chats.setLocalUserName(QStringLiteral("Developer"));
        chats.setNavSection(OpenChat::ChatController::NavSection::Settings);
        chats.setCurrentSettingsCategory(1); // Audio & Video
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

        // The Input section, first on the Audio & Video page.
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
        chats.setCurrentSettingsCategory(1); // Audio & Video
        QVERIFY(chats.currentSettingsElements().contains(QStringLiteral("Connection")));
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
        chats.setCurrentSettingsCategory(0); // General
        QVERIFY(chats.currentSettingsElements().contains(QStringLiteral("Memory")));
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
        chats.setCurrentSettingsCategory(2); // Appearance
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
        // Settings built its controls afresh on return; the earlier switch is gone.
        chats.setNavSection(OpenChat::ChatController::NavSection::Settings);
        QCoreApplication::processEvents();
        toggle = findVisualItem(window->contentItem(), QStringLiteral("darkModeSwitch"));
        QVERIFY(toggle && toggle->isVisible());
        QVERIFY(toggle->property("checked").toBool());
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
        QObject *input = root->findChild<QObject *>(QStringLiteral("messageInput"));
        auto *attach = qobject_cast<QQuickItem *>(
            root->findChild<QObject *>(QStringLiteral("attachButton")));
        QVERIFY(composer);
        QVERIFY(frame);
        QVERIFY(attach);
        QVERIFY(input);
        // The old chevron segment inside the field is gone.
        QVERIFY(!root->findChild<QObject *>(QStringLiteral("attachmentButton")));

        QCoreApplication::processEvents();
        const qreal singleLineHeight = frame->property("height").toReal();
        QCOMPARE(singleLineHeight, 40.0);

        // Holding one line, the composer lines up with the sidebar's navigation
        // bar beside it. The round "+" sits at the old field margin, as tall
        // as one line and level with the field; the field starts after it and
        // keeps its right margin (the outgoing bubbles' edge).
        auto *composerItem = qobject_cast<QQuickItem *>(composer);
        auto *bottomNav = qobject_cast<QQuickItem *>(
            root->findChild<QObject *>(QStringLiteral("bottomNav")));
        auto *frameItem = qobject_cast<QQuickItem *>(frame);
        QVERIFY(composerItem && bottomNav && frameItem);
        QCOMPARE(composerItem->height(), bottomNav->height());
        QCOMPARE(composerItem->mapToScene(QPointF()).y(), bottomNav->mapToScene(QPointF()).y());
        QCOMPARE(attach->x(), 17.0);
        QCOMPARE(attach->width(), singleLineHeight);
        QCOMPARE(attach->height(), singleLineHeight);
        QCOMPARE(attach->y() + attach->height(), frameItem->y() + frameItem->height());
        QCOMPARE(frameItem->x(), 65.0);
        QCOMPARE(composerItem->width() - (frameItem->x() + frameItem->width()), 17.0);

        QVERIFY(input->setProperty("text", QStringLiteral("First line\nSecond line\nThird line")));
        QCoreApplication::processEvents();
        const qreal multilineHeight = frame->property("height").toReal();
        QVERIFY(multilineHeight > singleLineHeight);
        // The "+" stays one line tall, level with the field's last line.
        QCOMPARE(attach->height(), singleLineHeight);
        QCOMPARE(attach->y() + attach->height(), frameItem->y() + frameItem->height());
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

    // The "+" left of the field opens the attach menu above itself; a click
    // on the cross it has become closes it again, and so do Esc and a click
    // anywhere else. Ctrl+O in the field opens it too, its first row picked
    // out for the arrows and Enter. The keyboard goes back to the field after
    // Esc, and stays wherever an outside click put it.
    void theAttachMenuOpensAboveThePlusAndTogglesWithIt()
    {
        failOnQmlWarnings();
        OpenChat::ChatController controller;
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{QStringLiteral("chatController"), QVariant::fromValue(&controller)}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");
        QCOMPARE(engine.rootObjects().size(), 1);
        QQuickWindow *window = showActiveWindow(engine.rootObjects().constFirst());
        if (!window)
            QSKIP("No active window on this platform to click into");

        auto *button = findVisualItem(window->contentItem(), QStringLiteral("attachButton"));
        auto *glyph = findVisualItem(window->contentItem(), QStringLiteral("attachButtonGlyph"));
        auto *menu = window->findChild<QObject *>(QStringLiteral("attachmentMenu"));
        auto *input = qobject_cast<QQuickItem *>(window->findChild<QObject *>(QStringLiteral("messageInput")));
        QVERIFY(button && glyph && menu && input);
        QTRY_VERIFY(input->hasActiveFocus());
        QVERIFY(button->isEnabled());
        QVERIFY(!menu->property("visible").toBool());
        QCOMPARE(button->property("activeFocusOnTab").toBool(), false);
        captureAttachmentShot(window, QStringLiteral("composer-idle"));

        // A click opens it above the "+", whose plus turns into a cross.
        clickItem(window, button);
        QTRY_VERIFY(menu->property("opened").toBool());
        QTRY_COMPARE(glyph->rotation(), 45.0);
        QVERIFY(menu->property("y").toReal() + menu->property("height").toReal() <= 0.0);
        const QStringList rows{QStringLiteral("attachPhoto"), QStringLiteral("attachVideo"),
                               QStringLiteral("attachAudio"), QStringLiteral("attachFile")};
        for (const QString &name : rows) {
            auto *row = menu->findChild<QQuickItem *>(name);
            QVERIFY2(row && row->isVisible(), qPrintable(name));
            QVERIFY(row->mapToScene(QPointF(0, row->height())).y() < button->mapToScene(QPointF()).y());
        }
        QCOMPARE(menu->findChild<QQuickItem *>(QStringLiteral("attachVideo"))->isEnabled(),
                 controller.videoAttachmentsSupported());
        QTest::qWait(400);
        captureAttachmentShot(window, QStringLiteral("menu-open"));

        // The cross closes it: the press on it does not count as outside.
        clickItem(window, button);
        QTRY_VERIFY(!menu->property("visible").toBool());
        QTRY_COMPARE(glyph->rotation(), 0.0);
        QTRY_VERIFY(input->hasActiveFocus());

        // From the keyboard: Ctrl+O, the arrows, and Esc back to the field.
        QTest::keyClick(window, Qt::Key_O, Qt::ControlModifier);
        QTRY_VERIFY(menu->property("opened").toBool());
        QCOMPARE(menu->property("currentIndex").toInt(), 0);
        QTest::keyClick(window, Qt::Key_Down);
        QCOMPARE(menu->property("currentIndex").toInt(), 1);
        QTest::qWait(400);
        captureAttachmentShot(window, QStringLiteral("menu-keyboard"));
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!menu->property("visible").toBool());
        QTRY_VERIFY(input->hasActiveFocus());
        QCOMPARE(controller.composerText(), QString());

        // A click elsewhere closes it and the keyboard stays where it went.
        clickItem(window, button);
        QTRY_VERIFY(menu->property("opened").toBool());
        auto *search = findVisualItem(window->contentItem(), QStringLiteral("contactSearch"));
        QVERIFY(search);
        clickItem(window, search);
        QTRY_VERIFY(!menu->property("visible").toBool());
        QTest::qWait(50);
        QVERIFY(search->hasActiveFocus());
        QVERIFY(!input->hasActiveFocus());
        input->forceActiveFocus();

        // Nothing is attached to an edit, or while the messages are hidden:
        // the "+" is off and Ctrl+O does nothing.
        const QString mine = controller.messages()->data(controller.messages()->index(1),
                                                         OpenChat::MessageListModel::StableIdRole).toString();
        QVERIFY(controller.beginEdit(mine));
        QTRY_VERIFY(!button->isEnabled());
        QTest::keyClick(window, Qt::Key_O, Qt::ControlModifier);
        QTest::qWait(50);
        QVERIFY(!menu->property("visible").toBool());
        controller.cancelComposeMode();
        QTRY_VERIFY(button->isEnabled());
        clickItem(window, button);
        QTRY_VERIFY(menu->property("opened").toBool());
        // Hiding the messages takes an open menu away with it.
        controller.setSessionState(OpenChat::ChatController::SessionState::Locked);
        QTRY_VERIFY(!menu->property("visible").toBool());
        QVERIFY(!button->isEnabled());
        controller.setSessionState(OpenChat::ChatController::SessionState::Ready);
        QTRY_VERIFY(button->isEnabled());
    }

    // Each row asks for its kind of file (the dialog also offers any file);
    // what is picked lands in the tray above the field as a card, the
    // composer growing to hold it, and the card's × takes it out again. A
    // pick the controller cannot take says why on a line above the tray.
    void pickingAKindOpensItsDialogAndTheFileLandsInTheTray()
    {
        failOnQmlWarnings();
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString picture = writeTestPicture(dir, QStringLiteral("harbour.png"), QColor("#4a8fc0"));
        QVERIFY(!picture.isEmpty());

        OpenChat::ChatController controller;
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{QStringLiteral("chatController"), QVariant::fromValue(&controller)}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");
        QCOMPARE(engine.rootObjects().size(), 1);
        QQuickWindow *window = showActiveWindow(engine.rootObjects().constFirst());
        if (!window)
            QSKIP("No active window on this platform to click into");

        auto *button = findVisualItem(window->contentItem(), QStringLiteral("attachButton"));
        auto *menu = window->findChild<QObject *>(QStringLiteral("attachmentMenu"));
        auto *composer = findVisualItem(window->contentItem(), QStringLiteral("messageComposer"));
        auto *tray = findVisualItem(window->contentItem(), QStringLiteral("stagedAttachments"));
        auto *input = qobject_cast<QQuickItem *>(window->findChild<QObject *>(QStringLiteral("messageInput")));
        QVERIFY(button && menu && composer && tray && input);
        QVERIFY(!tray->isVisible());
        const qreal restingHeight = composer->height();

        // Every row opens its own dialog.
        const QList<std::pair<QString, QString>> rows{
            {QStringLiteral("attachPhoto"), QStringLiteral("attachPhotoDialog")},
            {QStringLiteral("attachVideo"), QStringLiteral("attachVideoDialog")},
            {QStringLiteral("attachAudio"), QStringLiteral("attachAudioDialog")},
            {QStringLiteral("attachFile"), QStringLiteral("attachFileDialog")}};
        for (const auto &[rowName, dialogName] : rows) {
            QObject *dialog = window->findChild<QObject *>(dialogName);
            QVERIFY2(dialog, qPrintable(dialogName));
            QCOMPARE(dialog->property("fileMode").toInt(), 1); // FileDialog.OpenFiles
            if (!controller.videoAttachmentsSupported() && rowName == QLatin1String("attachVideo"))
                continue;
            clickItem(window, button);
            QTRY_VERIFY(menu->property("opened").toBool());
            auto *row = menu->findChild<QQuickItem *>(rowName);
            QVERIFY(row);
            QTest::qWait(300); // the rows rise into place
            clickItem(window, row);
            QTRY_VERIFY2(dialog->property("visible").toBool(), qPrintable(dialogName));
            QTRY_VERIFY(!menu->property("visible").toBool());
            QMetaObject::invokeMethod(dialog, "close");
            QTRY_VERIFY(!dialog->property("visible").toBool());
            window->requestActivate();
            QVERIFY(QTest::qWaitForWindowActive(window));
        }

        // The photo dialog's pick is staged: a card in the tray, which the
        // composer grows to hold.
        QObject *photos = window->findChild<QObject *>(QStringLiteral("attachPhotoDialog"));
        photos->setProperty("selectedFile", QUrl::fromLocalFile(picture));
        QMetaObject::invokeMethod(photos, "accepted");
        QTRY_VERIFY(controller.hasStagedAttachments());
        QTRY_VERIFY(tray->isVisible());
        QTRY_COMPARE(composer->height(), restingHeight + tray->height() + 10.0);
        QTRY_VERIFY(input->hasActiveFocus());
        // The tray's list makes its card on its next layout, which a busy
        // machine can put after the tray has already grown.
        QQuickItem *card = nullptr;
        QTRY_VERIFY((card = findVisualItem(tray, QStringLiteral("stagedAttachmentCard"))));
        QCOMPARE(card->property("name").toString(), QStringLiteral("harbour.png"));
        QCOMPARE(card->property("kind").toInt(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(card->property("ready").toBool(), 20'000);
        QTRY_VERIFY(findVisualItem(card, QStringLiteral("stagedAttachmentThumbnail"))->property("ready").toBool());
        // Something to send now, with no text.
        QVERIFY(controller.canSend());
        QTest::qWait(200);
        captureAttachmentShot(window, QStringLiteral("tray-photo"));

        // A second pick, of a file, makes a second card beside it.
        const QString notes = dir.filePath(QStringLiteral("Meeting notes.txt"));
        {
            QFile file(notes);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("Bring the tickets.\n");
        }
        QObject *files = window->findChild<QObject *>(QStringLiteral("attachFileDialog"));
        files->setProperty("selectedFile", QUrl::fromLocalFile(notes));
        QMetaObject::invokeMethod(files, "accepted");
        auto *model = qobject_cast<QAbstractItemModel *>(controller.stagedAttachments());
        QVERIFY(model);
        QTRY_COMPARE(model->rowCount(), 2);
        QTRY_VERIFY_WITH_TIMEOUT(model->data(model->index(1, 0),
                                             model->roleNames().key("ready")).toBool(), 20'000);
        QTest::qWait(200);
        captureAttachmentShot(window, QStringLiteral("tray-photo-and-file"));

        // Something that cannot be attached says why, above the tray, until
        // it is dismissed.
        auto *notice = findVisualItem(window->contentItem(), QStringLiteral("attachmentNotice"));
        QVERIFY(notice && !notice->isVisible());
        controller.attachFiles({QUrl(QStringLiteral("https://example.com/cat.png"))});
        QTRY_VERIFY(notice->isVisible());
        QVERIFY(!controller.attachmentNotice().isEmpty());
        QTRY_COMPARE(composer->height(), restingHeight + tray->height() + 10.0 + 26.0);
        captureAttachmentShot(window, QStringLiteral("tray-notice"));
        clickItem(window, findVisualItem(notice, QStringLiteral("attachmentNoticeDismiss")));
        QTRY_VERIFY(!notice->isVisible());
        QVERIFY(controller.attachmentNotice().isEmpty());

        // The ×s take the cards back out, and the composer settles back.
        while (model->rowCount() > 0) {
            const int before = model->rowCount();
            QQuickItem *remove = nullptr;
            QTRY_VERIFY((remove = findVisualItem(tray, QStringLiteral("removeStagedAttachment"))));
            clickItem(window, remove);
            QTRY_COMPARE(model->rowCount(), before - 1);
            QTest::qWait(200); // the card shrinks away
        }
        QTRY_VERIFY(!controller.hasStagedAttachments());
        QTRY_VERIFY(!tray->isVisible());
        QTRY_COMPARE(composer->height(), restingHeight);
    }

    // Files dragged in from the desktop are attached, but only local files
    // and only while the open chat can take them: not while a message is
    // being edited, while the messages are hidden, or under a profile.
    void droppedFilesAreAttachedOnlyWhileTheChatCanTakeThem()
    {
        failOnQmlWarnings();
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString picture = writeTestPicture(dir, QStringLiteral("dropped.png"), QColor("#c07a4a"));
        QVERIFY(!picture.isEmpty());

        OpenChat::ChatController controller;
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{QStringLiteral("chatController"), QVariant::fromValue(&controller)}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");
        QCOMPARE(engine.rootObjects().size(), 1);
        QQuickWindow *window = showActiveWindow(engine.rootObjects().constFirst());
        if (!window)
            QSKIP("No active window on this platform to drop onto");
        auto *drop = findVisualItem(window->contentItem(), QStringLiteral("attachDropOverlay"));
        auto *history = findVisualItem(window->contentItem(), QStringLiteral("messageHistory"));
        QVERIFY(drop && history);
        QVERIFY(drop->property("accepting").toBool());

        // A drag over the chat, as the desktop sends one: the well shows
        // while it hovers, and the drop stages the local file (not the link).
        const QPoint over = centreOf(history);
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(picture), QUrl(QStringLiteral("https://example.com/a.png"))});
        QDragEnterEvent enter(over, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &enter);
        QDragMoveEvent move(over, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &move);
        QTRY_VERIFY(drop->property("hovering").toBool());
        captureAttachmentShot(window, QStringLiteral("drop-hover"));
        QDropEvent dropped(over, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &dropped);
        QTRY_VERIFY(!drop->property("hovering").toBool());
        QTRY_VERIFY(controller.hasStagedAttachments());
        auto *model = qobject_cast<QAbstractItemModel *>(controller.stagedAttachments());
        QVERIFY(model);
        QCOMPARE(model->rowCount(), 1);
        QCOMPARE(model->data(model->index(0, 0), model->roleNames().key("name")).toString(),
                 QStringLiteral("dropped.png"));
        controller.clearStagedAttachments();

        // A drag of links only is refused outright.
        QMimeData links;
        links.setUrls({QUrl(QStringLiteral("https://example.com/a.png"))});
        QDragEnterEvent linkEnter(over, Qt::CopyAction, &links, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &linkEnter);
        QTest::qWait(20);
        QVERIFY(!drop->property("hovering").toBool());
        QDragLeaveEvent leave;
        QCoreApplication::sendEvent(window, &leave);

        // Editing a message, hidden messages and an open profile: no drops.
        const QString mine = controller.messages()->data(controller.messages()->index(1),
                                                         OpenChat::MessageListModel::StableIdRole).toString();
        QVERIFY(controller.beginEdit(mine));
        QTRY_VERIFY(!drop->property("accepting").toBool());
        controller.cancelComposeMode();
        QTRY_VERIFY(drop->property("accepting").toBool());
        controller.setSessionState(OpenChat::ChatController::SessionState::Locked);
        QTRY_VERIFY(!drop->property("accepting").toBool());
        controller.setSessionState(OpenChat::ChatController::SessionState::Ready);
        QTRY_VERIFY(drop->property("accepting").toBool());
        controller.profiles()->openContact(controller.currentContactId());
        QTRY_VERIFY(!drop->property("accepting").toBool());
        QDragEnterEvent underProfile(over, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &underProfile);
        QDropEvent droppedUnderProfile(over, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &droppedUnderProfile);
        QTest::qWait(50);
        QVERIFY(!controller.hasStagedAttachments());
    }

    // An attachment's bubble, for each kind, with and without a caption, as
    // a reply, sent and received: the block loads (by URL, so a bubble on its
    // own needs nothing it does not use), the bubble takes its width from the
    // block, a caption wraps under it, a quote sits above it, and a picture
    // without a caption carries its time on itself. Pictures keep their shape
    // within bounds. Under a skin and in the dark theme the rows take the
    // bubble's inks. Nothing warns.
    void attachmentBubblesDrawEveryKindWithoutWarnings()
    {
        failOnQmlWarnings();
        QQmlEngine engine;
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        auto *appearance = engine.singletonInstance<OpenChat::AppearanceSettings *>(
            "OpenChat.Native", "AppearanceSettings");
        QVERIFY(appearance);
        QQmlComponent component(&engine);
        component.loadFromModule("OpenChat", "MessageDelegate");
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));

        QVariantList peaks;
        for (int i = 0; i < 96; ++i)
            peaks.append(40 + (i * 37) % 200);
        const QStringList names{QString(), QStringLiteral("IMG_2041.jpg"), QStringLiteral("Evening walk.mp4"),
                                QStringLiteral("Harbour tune.m4a"), QStringLiteral("Trip itinerary.pdf")};
        const auto bubble = [&](int attachmentKind, int direction, const QString &caption, bool reply,
                                const QVariantMap &extra = {}) {
            const bool visual = attachmentKind == 1 || attachmentKind == 2;
            QVariantMap properties{
                {"direction", direction}, {"deliveryState", 3}, {"body", caption},
                {"timestamp", "10:15 AM"}, {"kind", 4}, {"dateLabel", ""},
                {"showDateDivider", false}, {"senderName", ""}, {"width", 720},
                {"stableId", "m1"}, {"attachmentKind", attachmentKind},
                {"fileName", names.value(attachmentKind)}, {"sizeText", "148 KB"},
                {"mediaWidth", visual ? 1600 : 0}, {"mediaHeight", visual ? 1200 : 0},
                {"durationMs", attachmentKind == 2 || attachmentKind == 3 ? 42000.0 : 0.0},
                {"peaks", attachmentKind == 3 ? peaks : QVariantList()}, {"transferState", 1},
                {"canSave", attachmentKind == 1 || attachmentKind == 4}};
            if (reply) {
                properties.insert("replyToId", "m0");
                properties.insert("quotedSender", "Michael");
                properties.insert("quotedBody", "Could you send me the plan?");
            }
            for (auto it = extra.cbegin(); it != extra.cend(); ++it)
                properties.insert(it.key(), it.value());
            return std::unique_ptr<QObject>(component.createWithInitialProperties(properties));
        };
        const QStringList blocks{QString(), QStringLiteral("chatImageBlock"), QStringLiteral("chatVideoBlock"),
                                 QStringLiteral("chatAudioRow"), QStringLiteral("chatFileRow")};

        for (int kind = 1; kind <= 4; ++kind) {
            for (const int direction : {0, 1}) {
                for (const bool captioned : {false, true}) {
                    for (const bool reply : {false, true}) {
                        const QString label = QStringLiteral("kind %1, %2, %3, %4")
                            .arg(kind).arg(direction ? "sent" : "received")
                            .arg(captioned ? "captioned" : "bare").arg(reply ? "reply" : "new");
                        const auto message = bubble(kind, direction,
                                                    captioned ? QStringLiteral("The view from the ferry")
                                                              : QString(), reply);
                        QVERIFY2(message, qPrintable(component.errorString()));
                        auto *loader = message->findChild<QQuickItem *>(QStringLiteral("attachmentLoader"));
                        QVERIFY2(loader && loader->property("status").toInt() == 1, qPrintable(label));
                        QVERIFY2(loader->findChild<QQuickItem *>(blocks.at(kind)), qPrintable(label));
                        auto *bubbleItem = message->findChild<QQuickItem *>(QStringLiteral("messageBubble"));
                        auto *body = message->findChild<QQuickItem *>(QStringLiteral("messageBody"));
                        auto *time = message->findChild<QQuickItem *>(QStringLiteral("messageTimestamp"));
                        auto *quote = message->findChild<QQuickItem *>(QStringLiteral("messageQuote"));
                        QVERIFY(bubbleItem && body && time && quote);
                        const bool visual = kind == 1 || kind == 2;
                        const qreal boxWidth = message->property("mediaBoxWidth").toReal();
                        // The block sits in the bubble, which is as wide as it.
                        QCOMPARE(loader->width(), boxWidth);
                        QCOMPARE(bubbleItem->width(), boxWidth + (visual ? 17.0 : 37.0));
                        QVERIFY2(loader->x() >= bubbleItem->x() && loader->y() >= bubbleItem->y()
                                     && loader->x() + loader->width() <= bubbleItem->x() + bubbleItem->width()
                                     && loader->y() + loader->height() <= bubbleItem->y() + bubbleItem->height(),
                                 qPrintable(label));
                        if (visual)
                            QCOMPARE(loader->x() - bubbleItem->x(), direction ? 4.0 : 13.0);
                        // The caption under it, left-aligned and wrapped to it.
                        QCOMPARE(body->isVisible(), captioned);
                        if (captioned) {
                            QVERIFY(body->y() >= loader->y() + loader->height());
                            QCOMPARE(body->x(), bubbleItem->x() + message->property("contentLeftInset").toReal());
                            QVERIFY(body->y() + body->height() <= bubbleItem->y() + bubbleItem->height());
                        }
                        // A picture with no caption carries its own time.
                        QCOMPARE(time->isVisible(), captioned || !visual);
                        // A reply quotes what it answers, above the block.
                        QCOMPARE(quote->isVisible(), reply);
                        if (reply)
                            QVERIFY(quote->y() + quote->height() <= loader->y());
                    }
                }
            }
        }

        // Pictures keep their shape between 2:5 and 5:2 inside 320 x 320,
        // never narrower than 140; an unknown shape is 4:3.
        const QList<std::tuple<int, int, qreal, qreal>> shapes{
            {1600, 1200, 320, 240}, {1200, 1600, 240, 320}, {8192, 100, 320, 128},
            {100, 8192, 140, 320}, {0, 0, 320, 240}, {1080, 1080, 320, 320}};
        for (const auto &[w, h, expectedWidth, expectedHeight] : shapes) {
            const auto message = bubble(1, 0, QString(), false, {{"mediaWidth", w}, {"mediaHeight", h}});
            QVERIFY(message);
            QCOMPARE(message->property("mediaBoxWidth").toReal(), expectedWidth);
            QCOMPARE(message->property("mediaBoxHeight").toReal(), expectedHeight);
        }

        // Under a skin the rows read in the skin's inks, raised.
        const QString nebula = QStringLiteral("bubble.nebula");
        appearance->setOwnedCosmetics({nebula});
        appearance->setBubbleSkin(nebula);
        {
            const auto file = bubble(4, 1, QStringLiteral("Here's the plan"), false);
            auto *name = file->findChild<QQuickItem *>(QStringLiteral("chatFileName"));
            QVERIFY(name);
            QCOMPARE(name->property("color").value<QColor>(), OpenChat::BubbleSkins::textColor(nebula));
            QCOMPARE(name->property("style").toInt(), 2); // Text.Raised
            const auto sound = bubble(3, 1, QString(), false);
            auto *clock = sound->findChild<QQuickItem *>(QStringLiteral("chatAudioTime"));
            QVERIFY(clock);
            QCOMPARE(clock->property("color").value<QColor>(), OpenChat::BubbleSkins::secondaryTextColor(nebula));
            for (int kind = 1; kind <= 4; ++kind)
                QVERIFY(bubble(kind, 1, QStringLiteral("Skinned"), true));
        }
        appearance->setBubbleSkin(QString());

        // And in the dark.
        appearance->setDarkMode(true);
        for (int kind = 1; kind <= 4; ++kind) {
            for (const int direction : {0, 1})
                QVERIFY(bubble(kind, direction, QStringLiteral("In the dark"), kind == 2));
        }
        const auto darkFile = bubble(4, 0, QString(), false);
        QCOMPARE(darkFile->findChild<QQuickItem *>(QStringLiteral("chatFileName"))->property("color").value<QColor>(),
                 QColor("#e0eaf3"));
        appearance->setDarkMode(false);

        // Every transfer state renders too: on its way (one's own, with the
        // cross that stops it), failed, stopped by the sender, unavailable.
        for (int kind = 0; kind <= 4; ++kind) {
            for (const int state : {0, 2, 3, 4}) {
                const auto message = bubble(kind, state == 0 ? 1 : 0, QString(), false,
                                            {{"transferState", state}, {"transferProgress", 0.4},
                                             {"transferText", state == 0 ? "Sending… 40%" : "Couldn't receive this file"},
                                             {"canCancel", state == 0}, {"canSave", false}});
                QVERIFY(message);
                auto *cancel = message->findChild<QQuickItem *>(QStringLiteral("cancelAttachment"));
                QVERIFY2(cancel && cancel->isVisible() == (state == 0), qPrintable(QString::number(kind)));
            }
        }

        // A picture of the transfer states, sent and received, in both
        // themes (only when OPENCHAT_ATTACHMENT_CAPTURES asks for one).
        if (qEnvironmentVariableIsEmpty("OPENCHAT_ATTACHMENT_CAPTURES"))
            return;
        for (const bool dark : {false, true}) {
            appearance->setDarkMode(dark);
            std::vector<std::unique_ptr<QObject>> made;
            QList<QQuickItem *> items;
            const auto add = [&](int kind, int direction, int state, const QString &text, bool retry = false) {
                made.push_back(bubble(kind, direction, QString(), false,
                                      {{"transferState", state}, {"transferProgress", 0.4}, {"transferText", text},
                                       {"canCancel", state == 0 && direction == 1}, {"canRetry", retry},
                                       {"canSave", false}, {"width", 640}}));
                items.append(qobject_cast<QQuickItem *>(made.back().get()));
            };
            add(1, 1, 0, QStringLiteral("Sending… 40%"));
            add(4, 0, 0, QStringLiteral("Receiving… 3 of 7"));
            add(3, 1, 0, QStringLiteral("Waiting for the call to end"));
            add(2, 0, 2, QStringLiteral("Couldn't receive this video"));
            add(4, 1, 3, QStringLiteral("You stopped sending this"), true);
            add(0, 0, 4, QStringLiteral("Couldn't show this attachment"));
            captureItems(items, dark ? QStringLiteral("bubble-states-dark") : QStringLiteral("bubble-states-light"),
                         false, dark ? QColor("#18232e") : QColor("#f8fbfd"));
        }
        appearance->setDarkMode(false);
    }

    // A tray card for every state: a photo or video that is ready (or on
    // its way) is just its thumbnail, anything with words to show is a wider
    // card; a card being prepared fills a bar, and one that failed says why in
    // the error colour. In both themes, and nothing warns.
    void stagedCardsDrawEveryState()
    {
        failOnQmlWarnings();
        QQmlEngine engine;
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        auto *appearance = engine.singletonInstance<OpenChat::AppearanceSettings *>(
            "OpenChat.Native", "AppearanceSettings");
        QVERIFY(appearance);
        QQmlComponent component(&engine);
        component.loadFromModule("OpenChat", "StagedAttachmentCard");
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        putTestPicture(QStringLiteral("test:harbour"), QSize(320, 240), QColor("#f2b48a"), QColor("#4a5d8c"));
        putTestPicture(QStringLiteral("test:clip"), QSize(320, 180), QColor("#2c3550"), QColor("#7d5a86"));
        const auto guard = qScopeGuard([] {
            OpenChat::PanelMediaLibrary::instance().release(QStringLiteral("test:harbour"));
            OpenChat::PanelMediaLibrary::instance().release(QStringLiteral("test:clip"));
        });

        struct Card { int kind; QString name; qreal progress; bool ready; bool failed; QString error;
                      QString notice; QString previewKey; QString durationText; qreal width; };
        const QList<Card> cards{
            {1, "Harbour.jpg", 1, true, false, {}, {}, "test:harbour", {}, 60},
            {2, "Evening walk.mp4", 1, true, false, {}, {}, "test:clip", "0:42", 60},
            {1, "IMG_2057.jpg", 0.4, false, false, {}, {}, {}, {}, 60},
            {2, "Holiday.mov", 1, true, false, {}, "Only the first minute will be sent.", "test:clip", "1:00", 256},
            {3, "Harbour tune.m4a", 1, true, false, {}, {}, {}, "2:05", 200},
            {4, "Quarterly report.pdf", 0.7, false, false, {}, {}, {}, {}, 200},
            {4, "Backup.zip", 0, false, true, "Files up to 16 MB can be sent.", {}, {}, {}, 256}};
        for (const bool dark : {false, true}) {
            appearance->setDarkMode(dark);
            std::vector<std::unique_ptr<QQuickItem>> made;
            QList<QQuickItem *> items;
            for (int i = 0; i < cards.size(); ++i) {
                const Card &card = cards.at(i);
                made.emplace_back(qobject_cast<QQuickItem *>(component.createWithInitialProperties({
                    {"index", i}, {"stagedId", QString::number(i)}, {"kind", card.kind},
                    {"name", card.name}, {"sizeText", "2.4 MB"}, {"progress", card.progress},
                    {"ready", card.ready}, {"failed", card.failed}, {"error", card.error},
                    {"notice", card.notice}, {"previewKey", card.previewKey},
                    {"durationText", card.durationText}})));
                QQuickItem *item = made.back().get();
                QVERIFY2(item, qPrintable(component.errorString()));
                QCOMPARE(item->width(), card.width);
                QCOMPARE(item->height(), 60.0);
                auto *progress = item->findChild<QQuickItem *>(QStringLiteral("stagedAttachmentProgress"));
                QVERIFY(progress);
                QCOMPARE(progress->property("visible").toBool(), !card.ready && !card.failed);
                if (card.width > 60) {
                    auto *status = item->findChild<QQuickItem *>(QStringLiteral("stagedAttachmentStatus"));
                    QVERIFY(status);
                    if (card.failed) {
                        QCOMPARE(status->property("text").toString(), card.error);
                        QCOMPARE(status->property("color").value<QColor>(),
                                 dark ? QColor("#ffa99e") : QColor("#c0392b"));
                    } else if (!card.notice.isEmpty()) {
                        QCOMPARE(status->property("text").toString(), card.notice);
                    }
                }
                items.append(item);
            }
            captureItems(items, dark ? QStringLiteral("tray-cards-dark") : QStringLiteral("tray-cards-light"),
                         true, dark ? QColor("#1e2e3b") : QColor("#eef4f8"));
        }
        appearance->setDarkMode(false);
    }

    // What came from the other side (a file's name, a caption, a sender's
    // name in the transfer line, a quote) is only ever shown as plain text,
    // wherever an attachment shows it.
    void attachmentPeerStringsArePlainText()
    {
        failOnQmlWarnings();
        QQmlEngine engine;
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        QQmlComponent component(&engine);
        component.loadFromModule("OpenChat", "MessageDelegate");
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));

        const QString name = QStringLiteral("<font color=\"#ff0000\">invoice</font>.pdf");
        const QString caption = QStringLiteral("<img src=\"http://example.com/beacon.png\">caption");
        const QString sender = QStringLiteral("<b>Mallory</b>");
        for (int kind = 0; kind <= 4; ++kind) {
            for (const int state : {0, 1, 2}) {
                std::unique_ptr<QObject> message(component.createWithInitialProperties({
                    {"direction", 0}, {"deliveryState", 3}, {"body", caption},
                    {"timestamp", "10:15 AM"}, {"kind", 4}, {"dateLabel", ""},
                    {"showDateDivider", false}, {"senderName", sender}, {"width", 720},
                    {"attachmentKind", kind}, {"fileName", name}, {"mimeType", "text/<i>x</i>"},
                    {"sizeText", "1 MB"}, {"durationMs", 1000.0}, {"transferState", state},
                    {"transferText", state == 1 ? QString() : QStringLiteral("Waiting for ") + sender},
                    {"quotedSender", sender}, {"quotedBody", caption}}));
                QVERIFY2(message, qPrintable(component.errorString()));
                auto *root = qobject_cast<QQuickItem *>(message.get());
                for (const QString &needle : {QStringLiteral("<font"), QStringLiteral("<img"),
                                              QStringLiteral("<b>")}) {
                    for (QQuickItem *text : textItemsShowing(root, needle)) {
                        QVERIFY2(text->property("textFormat").toInt() == 0, // PlainText
                                 qPrintable(QStringLiteral("%1 shows %2 as markup")
                                                .arg(text->objectName(), needle)));
                    }
                }
                if (kind == 4)
                    QVERIFY(!textItemsShowing(root, QStringLiteral("<font")).isEmpty());
            }
        }

        // The tray's cards and the notice line too.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString odd = dir.filePath(QStringLiteral("<b>odd<b>.txt"));
        {
            QFile file(odd);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("x");
        }
        OpenChat::ChatController controller;
        QQmlApplicationEngine app;
        app.setInitialProperties({{QStringLiteral("chatController"), QVariant::fromValue(&controller)}});
        app.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        app.loadFromModule("OpenChat", "Main");
        QCOMPARE(app.rootObjects().size(), 1);
        auto *window = qobject_cast<QQuickWindow *>(app.rootObjects().constFirst());
        QVERIFY(window);
        controller.attachFiles({QUrl::fromLocalFile(odd)});
        auto *tray = findVisualItem(window->contentItem(), QStringLiteral("stagedAttachments"));
        QVERIFY(tray);
        QTRY_VERIFY(findVisualItem(tray, QStringLiteral("stagedAttachmentName")));
        QCOMPARE(findVisualItem(tray, QStringLiteral("stagedAttachmentName"))->property("textFormat").toInt(), 0);
        QCOMPARE(findVisualItem(tray, QStringLiteral("stagedAttachmentStatus"))->property("textFormat").toInt(), 0);
        for (QQuickItem *text : textItemsShowing(tray, QStringLiteral("odd")))
            QCOMPARE(text->property("textFormat").toInt(), 0);
        auto *notice = findVisualItem(window->contentItem(), QStringLiteral("attachmentNoticeText"));
        QVERIFY(notice);
        QCOMPARE(notice->property("textFormat").toInt(), 0);
    }

    // The chat with a sample of every kind (--attachment-demo): each bubble
    // shows its block, a hovered attachment offers Reply, Copy only with a
    // caption, never Edit, and Save for a complete photo or file, whose
    // dialog proposes the file's own name and writes it where asked.
    void attachmentBubblesInTheChatOfferTheirActions()
    {
        failOnQmlWarnings();
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        OpenChat::ChatController controller;
        controller.injectDemoAttachmentsForCapture();
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{QStringLiteral("chatController"), QVariant::fromValue(&controller)}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");
        QCOMPARE(engine.rootObjects().size(), 1);
        QQuickWindow *window = showActiveWindow(engine.rootObjects().constFirst());
        if (!window)
            QSKIP("No active window on this platform to hover over");
        window->resize(1000, 1400);
        QTest::qWait(100);

        auto *messages = controller.messages();
        const auto role = [messages](int row, int role) { return messages->data(messages->index(row), role); };
        const auto rowOf = [&](int attachmentKind, int direction) {
            for (int row = messages->rowCount() - 1; row >= 0; --row) {
                if (role(row, OpenChat::MessageListModel::KindRole).toInt() == 4
                    && role(row, OpenChat::MessageListModel::AttachmentKindRole).toInt() == attachmentKind
                    && role(row, OpenChat::MessageListModel::DirectionRole).toInt() == direction
                    && role(row, OpenChat::MessageListModel::TransferStateRole).toInt() == 1)
                    return row;
            }
            return -1;
        };
        const auto delegateFor = [window](const QString &stableId) -> QQuickItem * {
            QQuickItem *found = nullptr;
            const auto visit = [&](const auto &self, QQuickItem *item) -> void {
                if (item->objectName() == QLatin1String("messageBubble") && item->parentItem()
                    && item->parentItem()->property("stableId").toString() == stableId)
                    found = item->parentItem();
                for (QQuickItem *child : item->childItems())
                    self(self, child);
            };
            visit(visit, window->contentItem());
            return found;
        };
        auto *history = findVisualItem(window->contentItem(), QStringLiteral("messageHistory"));
        QVERIFY(history);
        // Scrolls a row into view and returns its delegate, once there.
        const auto show = [&](int row) {
            const QString stableId = role(row, OpenChat::MessageListModel::StableIdRole).toString();
            QMetaObject::invokeMethod(history, "showMessage", Q_ARG(QVariant, stableId));
            QQuickItem *delegate = nullptr;
            return QTest::qWaitFor([&] { return (delegate = delegateFor(stableId)) != nullptr; }, 2000)
                ? delegate : nullptr;
        };
        const auto hover = [window](QQuickItem *item) {
            QTest::mouseMove(window, item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint());
        };
        const auto action = [](QQuickItem *delegate, const char *name) {
            return delegate->findChild<QQuickItem *>(QLatin1String(name));
        };

        // Every kind is there, each drawn by its own block.
        const int photoRow = rowOf(1, 0);
        const int videoRow = rowOf(2, 1);
        const int audioRow = rowOf(3, 0);
        const int fileRow = rowOf(4, 1);
        QVERIFY(photoRow >= 0 && videoRow >= 0 && audioRow >= 0 && fileRow >= 0);
        QQuickItem *photo = show(photoRow);
        QVERIFY(photo && photo->findChild<QQuickItem *>(QStringLiteral("chatImageBlock")));
        QTRY_VERIFY_WITH_TIMEOUT(photo->findChild<QQuickItem *>(QStringLiteral("chatImage"))->property("ready").toBool(),
                                 10'000);
        QQuickItem *audio = show(audioRow);
        QVERIFY(audio && audio->findChild<QQuickItem *>(QStringLiteral("chatAudioRow")));
        QQuickItem *file = show(fileRow);
        QVERIFY(file && file->findChild<QQuickItem *>(QStringLiteral("chatFileRow")));
        QQuickItem *video = show(videoRow);
        QVERIFY(video && video->findChild<QQuickItem *>(QStringLiteral("chatVideoBlock")));
        QMetaObject::invokeMethod(history, "positionAtEnd");
        QTest::qWait(200);
        captureAttachmentShot(window, QStringLiteral("bubbles-light"));

        // A captioned file: Copy (the caption), Reply and Save; never Edit.
        file = show(fileRow);
        hover(file->findChild<QQuickItem *>(QStringLiteral("messageBubble")));
        QTRY_VERIFY(action(file, "messageActions")->isVisible());
        QVERIFY(action(file, "messageCopyAction")->isVisible());
        QVERIFY(action(file, "messageReplyAction")->isVisible());
        QVERIFY(action(file, "messageSaveAction")->isVisible());
        QVERIFY(!action(file, "messageEditAction")->isVisible());
        clickItem(window, action(file, "messageCopyAction"));
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("Here's the plan for Saturday"));

        // A bare sound: Reply only.
        audio = show(audioRow);
        hover(audio->findChild<QQuickItem *>(QStringLiteral("messageBubble")));
        QTRY_VERIFY(action(audio, "messageActions")->isVisible());
        QVERIFY(!action(audio, "messageCopyAction")->isVisible());
        QVERIFY(!action(audio, "messageEditAction")->isVisible());
        QVERIFY(!action(audio, "messageSaveAction")->isVisible());
        QVERIFY(action(audio, "messageReplyAction")->isVisible());

        // Save asks where, proposing the file's own name, and writes it there.
        file = show(fileRow);
        hover(file->findChild<QQuickItem *>(QStringLiteral("messageBubble")));
        QTRY_VERIFY(action(file, "messageSaveAction")->isVisible());
        QObject *saveDialog = window->findChild<QObject *>(QStringLiteral("saveAttachmentDialog"));
        QVERIFY(saveDialog);
        clickItem(window, action(file, "messageSaveAction"));
        QTRY_VERIFY(saveDialog->property("visible").toBool());
        QCOMPARE(saveDialog->property("selectedFile").toUrl().fileName(), QStringLiteral("Trip itinerary.pdf"));
        const QString target = dir.filePath(QStringLiteral("itinerary.pdf"));
        saveDialog->setProperty("selectedFile", QUrl::fromLocalFile(target));
        QMetaObject::invokeMethod(saveDialog, "accepted");
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(target).size() > 0, 10'000);
        QMetaObject::invokeMethod(saveDialog, "close");
        QTRY_VERIFY(!saveDialog->property("visible").toBool());

        // The same chat in the dark.
        OpenChat::AppearanceSettings *appearance = engine.singletonInstance<OpenChat::AppearanceSettings *>(
            "OpenChat.Native", "AppearanceSettings");
        QVERIFY(appearance);
        appearance->setDarkMode(true);
        QMetaObject::invokeMethod(history, "positionAtEnd");
        QTest::qWait(200);
        captureAttachmentShot(window, QStringLiteral("bubbles-dark"));
        appearance->setDarkMode(false);
    }

    // A photo whose bytes finish arriving while its bubble is on screen
    // shows whole there without anything being rebuilt: the bubble's media
    // handle follows the transfer state, dropping the whole picture while it
    // is on its way and fetching it once it is here.
    void aPhotoThatFinishesArrivingOnScreenShowsWhole()
    {
        failOnQmlWarnings();
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString picture = writeTestPicture(dir, QStringLiteral("arriving.png"), QColor("#6a9cc8"));
        QVERIFY(!picture.isEmpty());
        OpenChat::ChatController controller;
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{QStringLiteral("chatController"), QVariant::fromValue(&controller)}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");
        QCOMPARE(engine.rootObjects().size(), 1);
        QQuickWindow *window = showActiveWindow(engine.rootObjects().constFirst());
        if (!window)
            QSKIP("No active window on this platform");

        controller.attachFiles({QUrl::fromLocalFile(picture)});
        auto *staged = qobject_cast<QAbstractItemModel *>(controller.stagedAttachments());
        QVERIFY(staged);
        QTRY_VERIFY_WITH_TIMEOUT(staged->rowCount() == 1
                                     && staged->data(staged->index(0, 0), staged->roleNames().key("ready")).toBool(),
                                 20'000);
        auto *messages = controller.messages();
        const int before = messages->rowCount();
        QVERIFY(controller.sendMessage());
        QTRY_COMPARE(messages->rowCount(), before + 1);
        const QString id = messages->data(messages->index(before), OpenChat::MessageListModel::StableIdRole).toString();
        QCOMPARE(messages->data(messages->index(before), OpenChat::MessageListModel::AttachmentKindRole).toInt(), 1);
        // On its way: three of seven parts here.
        QVERIFY(messages->updateTransfer(id, OpenChat::AttachmentTransferState::Transferring, 0, 3, 7));

        QQuickItem *block = nullptr;
        QTRY_VERIFY(([&] {
            const auto visit = [&](const auto &self, QQuickItem *item) -> void {
                auto *owner = item->property("row").value<QQuickItem *>();
                if (item->objectName() == QLatin1String("chatImageBlock") && owner
                    && owner->property("stableId").toString() == id)
                    block = item;
                for (QQuickItem *child : item->childItems())
                    self(self, child);
            };
            visit(visit, window->contentItem());
            return block != nullptr;
        }()));
        auto *image = block->findChild<QQuickItem *>(QStringLiteral("chatImage"));
        auto *overlay = block->findChild<QQuickItem *>(QStringLiteral("chatTransferOverlay"));
        QQuickItem *holder = block;
        while (holder && holder->objectName() != QLatin1String("attachmentBlock"))
            holder = holder->parentItem();
        QVERIFY(image && overlay && holder);
        auto *media = holder->findChild<QObject *>(QStringLiteral("attachmentMedia"));
        QVERIFY(media);
        QTRY_VERIFY(overlay->isVisible());
        QTRY_VERIFY(media->property("imageKey").toString().isEmpty());
        QVERIFY(!image->property("ready").toBool());

        // The last part lands: the same bubble fetches and shows the photo.
        QVERIFY(messages->updateTransfer(id, OpenChat::AttachmentTransferState::Ready, 0, 7, 7));
        QTRY_VERIFY_WITH_TIMEOUT(!media->property("imageKey").toString().isEmpty(), 10'000);
        QTRY_VERIFY_WITH_TIMEOUT(image->property("ready").toBool(), 10'000);
        QVERIFY(!overlay->isVisible());
    }

    // A sound in the chat plays in the chat's one player, not in its bubble:
    // the bubble shows what the player does (pause, the bars filling, the
    // time), the sound keeps playing after its bubble has scrolled away, and
    // it stops when a call starts or another chat opens.
    void aSoundPlaysInTheChatsOnePlayerAndOutlivesItsBubble()
    {
        failOnQmlWarnings();
        QIODevice *stream = nullptr;
        int streamChannels = 0;
        OpenChat::SongPlayer::setOutputFactoryForTesting([&stream, &streamChannels](int channels, QString &) {
            streamChannels = channels;
            return std::make_unique<SilentSongOutput>(channels, &stream);
        });
        const auto restore = qScopeGuard([] { OpenChat::SongPlayer::setOutputFactoryForTesting({}); });
        OpenChat::ChatController controller;
        controller.injectDemoAttachmentsForCapture();
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{QStringLiteral("chatController"), QVariant::fromValue(&controller)}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");
        QCOMPARE(engine.rootObjects().size(), 1);
        QQuickWindow *window = showActiveWindow(engine.rootObjects().constFirst());
        if (!window)
            QSKIP("No active window on this platform to click into");
        window->resize(1000, 1200);
        QTest::qWait(100);

        auto *messages = controller.messages();
        QString id;
        for (int row = 0; row < messages->rowCount(); ++row) {
            if (messages->data(messages->index(row), OpenChat::MessageListModel::AttachmentKindRole).toInt() == 3)
                id = messages->data(messages->index(row), OpenChat::MessageListModel::StableIdRole).toString();
        }
        if (id.isEmpty())
            QSKIP("This build made no sample sound");
        auto *history = findVisualItem(window->contentItem(), QStringLiteral("messageHistory"));
        QObject *player = history->findChild<QObject *>(QStringLiteral("chatAudioPlayer"));
        QVERIFY(history && player);
        const auto rowFor = [window, &id]() -> QQuickItem * {
            QQuickItem *found = nullptr;
            const auto visit = [&](const auto &self, QQuickItem *item) -> void {
                auto *owner = item->property("row").value<QQuickItem *>();
                if (item->objectName() == QLatin1String("chatAudioRow") && owner
                    && owner->property("stableId").toString() == id)
                    found = item;
                for (QQuickItem *child : item->childItems())
                    self(self, child);
            };
            visit(visit, window->contentItem());
            return found;
        };
        QMetaObject::invokeMethod(history, "showMessage", Q_ARG(QVariant, id));
        QQuickItem *row = nullptr;
        QTRY_VERIFY((row = rowFor()));
        auto *play = row->findChild<QQuickItem *>(QStringLiteral("chatAudioPlay"));
        auto *time = row->findChild<QQuickItem *>(QStringLiteral("chatAudioTime"));
        QVERIFY(play && time);
        QTest::qWait(200);

        // Play: the chat's player loads this sound and plays it; the bubble
        // says so and follows it along.
        clickItem(window, play);
        QTRY_COMPARE(player->property("activeId").toString(), id);
        QTRY_VERIFY(player->property("playing").toBool());
        QVERIFY(row->property("playing").toBool());
        QTRY_VERIFY(stream != nullptr);
        stream->read(qint64(48000) * 2 * streamChannels * 2); // two seconds of it
        QTRY_VERIFY(row->property("positionMs").toReal() >= 1500);
        QVERIFY(time->property("text").toString().startsWith(QStringLiteral("0:0")));
        captureAttachmentShot(window, QStringLiteral("audio-playing"));

        // Its bubble scrolls away and is gone; the sound plays on.
        window->resize(1000, 560);
        QMetaObject::invokeMethod(history, "showMessage",
                                  Q_ARG(QVariant, messages->data(messages->index(0),
                                                                 OpenChat::MessageListModel::StableIdRole)));
        QTRY_VERIFY(rowFor() == nullptr);
        QVERIFY(player->property("playing").toBool());
        QCOMPARE(player->property("activeId").toString(), id);

        // A call silences it.
        history->setProperty("callActive", true);
        QTRY_VERIFY(!player->property("playing").toBool());
        history->setProperty("callActive", false);

        // Another chat stops it for good.
        window->resize(1000, 1200);
        QTest::qWait(100);
        QMetaObject::invokeMethod(history, "showMessage", Q_ARG(QVariant, id));
        QTRY_VERIFY((row = rowFor()));
        QTest::qWait(100);
        if (!player->property("playing").toBool())
            clickItem(window, row->findChild<QQuickItem *>(QStringLiteral("chatAudioPlay")));
        QTRY_VERIFY(player->property("playing").toBool());
        QVERIFY(controller.selectContact(QStringLiteral("alex")));
        QTRY_VERIFY(!player->property("playing").toBool());
        QCOMPARE(player->property("activeId").toString(), QString());
    }

    // A photo opens large over the window from its bubble, holding its own
    // copy of the picture; Esc, a click outside it or the cross put it away,
    // and hiding the messages (a lock) takes it away at once with everything
    // it held. While it is up, Ctrl+I opens no profile over it. A video opens
    // playing, with its controls under it.
    void theViewerOpensPicturesLargeAndGoesWhenTheMessagesAreHidden()
    {
        failOnQmlWarnings();
        OpenChat::ChatController controller;
        controller.injectDemoAttachmentsForCapture();
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{QStringLiteral("chatController"), QVariant::fromValue(&controller)}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");
        QCOMPARE(engine.rootObjects().size(), 1);
        QQuickWindow *window = showActiveWindow(engine.rootObjects().constFirst());
        if (!window)
            QSKIP("No active window on this platform to click into");
        window->resize(1000, 900);
        QTest::qWait(100);

        auto *messages = controller.messages();
        int photoRow = -1;
        int videoRow = -1;
        for (int row = 0; row < messages->rowCount(); ++row) {
            const QModelIndex index = messages->index(row);
            if (messages->data(index, OpenChat::MessageListModel::TransferStateRole).toInt() != 1)
                continue;
            const int kind = messages->data(index, OpenChat::MessageListModel::AttachmentKindRole).toInt();
            if (kind == 1 && photoRow < 0)
                photoRow = row;
            if (kind == 2 && videoRow < 0)
                videoRow = row;
        }
        QVERIFY(photoRow >= 0);
        const auto idOf = [messages](int row) {
            return messages->data(messages->index(row), OpenChat::MessageListModel::StableIdRole).toString();
        };
        auto *history = findVisualItem(window->contentItem(), QStringLiteral("messageHistory"));
        auto *viewer = findVisualItem(window->contentItem(), QStringLiteral("chatMediaViewer"));
        auto *input = qobject_cast<QQuickItem *>(window->findChild<QObject *>(QStringLiteral("messageInput")));
        QVERIFY(history && viewer && input);
        QVERIFY(!viewer->isVisible());
        // Scrolls a row into view and returns its attachment block, once there.
        const auto blockOf = [&](int row, const char *name) -> QQuickItem * {
            QMetaObject::invokeMethod(history, "showMessage", Q_ARG(QVariant, idOf(row)));
            QQuickItem *block = nullptr;
            const auto find = [&] {
                const auto visit = [&](const auto &self, QQuickItem *item) -> void {
                    auto *owner = item->property("row").value<QQuickItem *>();
                    if (item->objectName() == QLatin1String(name) && owner
                        && owner->property("stableId").toString() == idOf(row))
                        block = item;
                    for (QQuickItem *child : item->childItems())
                        self(self, child);
                };
                visit(visit, window->contentItem());
                return block != nullptr;
            };
            return QTest::qWaitFor(find, 2000) ? block : nullptr;
        };

        QQuickItem *photo = blockOf(photoRow, "chatImageBlock");
        QVERIFY(photo);
        QTest::qWait(250);
        clickItem(window, photo);
        QTRY_VERIFY(viewer->property("expanded").toBool());
        QVERIFY(viewer->isVisible());
        QCOMPARE(viewer->property("stableId").toString(), idOf(photoRow));
        auto *picture = findVisualItem(viewer, QStringLiteral("chatMediaViewerImage"));
        QVERIFY(picture);
        QTRY_VERIFY_WITH_TIMEOUT(picture->property("ready").toBool(), 10'000);
        QTest::qWait(300);
        captureAttachmentShot(window, QStringLiteral("viewer-photo"));
        if (!qEnvironmentVariableIsEmpty("OPENCHAT_ATTACHMENT_CAPTURES")) {
            auto *appearance = engine.singletonInstance<OpenChat::AppearanceSettings *>(
                "OpenChat.Native", "AppearanceSettings");
            appearance->setDarkMode(true);
            QTest::qWait(100);
            captureAttachmentShot(window, QStringLiteral("viewer-photo-dark"));
            appearance->setDarkMode(false);
        }
        // Ctrl+I opens no profile over it; Esc closes it and hands the
        // keyboard back to the field.
        QTest::keyClick(window, Qt::Key_I, Qt::ControlModifier);
        QTest::qWait(50);
        QVERIFY(!controller.profiles()->property("open").toBool());
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!viewer->property("expanded").toBool());
        QTRY_VERIFY(!viewer->isVisible());
        QCOMPARE(viewer->property("stableId").toString(), QString());
        QTRY_VERIFY(input->hasActiveFocus());

        // A click outside the picture closes it too.
        photo = blockOf(photoRow, "chatImageBlock");
        clickItem(window, photo);
        QTRY_VERIFY(viewer->property("expanded").toBool());
        QTest::qWait(300);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, QPoint(12, window->height() - 12));
        QTRY_VERIFY(!viewer->isVisible());

        // A lock takes it away at once, with the picture it held.
        photo = blockOf(photoRow, "chatImageBlock");
        clickItem(window, photo);
        QTRY_VERIFY(viewer->property("expanded").toBool());
        auto *media = viewer->findChild<QObject *>(QStringLiteral("chatMediaViewerMedia"));
        QVERIFY(media);
        QTRY_VERIFY(!media->property("imageKey").toString().isEmpty());
        controller.setSessionState(OpenChat::ChatController::SessionState::Locked);
        QVERIFY(!viewer->isVisible());
        QCOMPARE(viewer->property("stableId").toString(), QString());
        QTRY_VERIFY(media->property("imageKey").toString().isEmpty());
        controller.setSessionState(OpenChat::ChatController::SessionState::Ready);

        // A video opens playing, its controls under it.
        if (videoRow < 0 || !controller.videoAttachmentsSupported())
            return;
        QQuickItem *video = blockOf(videoRow, "chatVideoBlock");
        QVERIFY(video);
        QTest::qWait(250);
        clickItem(window, video);
        QTRY_VERIFY(viewer->property("expanded").toBool());
        auto *controls = findVisualItem(viewer, QStringLiteral("chatMediaViewerControls"));
        QVERIFY(controls);
        QTRY_VERIFY(controls->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(viewer->findChild<QObject *>(QStringLiteral("chatMediaViewerPlayer")), 15'000);
        QObject *player = viewer->findChild<QObject *>(QStringLiteral("chatMediaViewerPlayer"));
        QTRY_VERIFY_WITH_TIMEOUT(player->property("hasPicture").toBool(), 15'000);
        QTest::qWait(300);
        captureAttachmentShot(window, QStringLiteral("viewer-video"));
        if (!qEnvironmentVariableIsEmpty("OPENCHAT_ATTACHMENT_CAPTURES")) {
            auto *appearance = engine.singletonInstance<OpenChat::AppearanceSettings *>(
                "OpenChat.Native", "AppearanceSettings");
            appearance->setDarkMode(true);
            QTest::qWait(100);
            captureAttachmentShot(window, QStringLiteral("viewer-video-dark"));
            appearance->setDarkMode(false);
        }
        // Its play button pauses it where it is.
        clickItem(window, findVisualItem(viewer, QStringLiteral("chatMediaViewerPlay")));
        QTRY_VERIFY(player->property("paused").toBool());
        clickItem(window, findVisualItem(viewer, QStringLiteral("chatMediaViewerClose")));
        QTRY_VERIFY(!viewer->isVisible());
    }

    void messageActionsSitInTheGapAndDriveEditAndReply()
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
            QSKIP("No active window on this platform to hover and type into");
        window->resize(1100, 820);
        QTest::qWait(50);

        auto *messages = controller.messages();
        const auto id = [messages](int row) {
            return messages->data(messages->index(row), OpenChat::MessageListModel::StableIdRole)
                .toString();
        };
        const QString theirs = id(0);
        const QString mine = id(1);
        // A message's delegate and its named parts, found in the visual tree.
        const auto delegateFor = [window](const QString &stableId) -> QQuickItem * {
            QQuickItem *found = nullptr;
            const auto visit = [&](const auto &self, QQuickItem *item) -> void {
                if (item->objectName() == QLatin1String("messageBubble") && item->parentItem()
                    && item->parentItem()->property("stableId").toString() == stableId)
                    found = item->parentItem();
                for (QQuickItem *child : item->childItems())
                    self(self, child);
            };
            visit(visit, window->contentItem());
            return found;
        };
        const auto part = [](QQuickItem *delegate, const char *name) {
            return delegate->findChild<QQuickItem *>(QLatin1String(name));
        };
        const auto hover = [window](QQuickItem *item) {
            QTest::mouseMove(window, item->mapToScene(QPointF(item->width() / 2,
                                                              item->height() / 2)).toPoint());
        };

        QQuickItem *myRow = delegateFor(mine);
        QQuickItem *theirRow = delegateFor(theirs);
        QVERIFY(myRow && theirRow);
        // Any of a message's text can be selected, none of it changed.
        QQuickItem *myBody = part(myRow, "messageBody");
        QVERIFY(myBody);
        QVERIFY(myBody->property("readOnly").toBool());
        QVERIFY(myBody->property("selectByMouse").toBool());

        // Hovering brings the actions up in the gap below the bubble without
        // making the message any taller; only one's own message can be edited.
        QQuickItem *myActions = part(myRow, "messageActions");
        QVERIFY(myActions && !myActions->isVisible());
        const qreal restingHeight = myRow->height();
        hover(part(myRow, "messageBubble"));
        QTRY_VERIFY(myActions->isVisible());
        QCOMPARE(myRow->height(), restingHeight);
        QQuickItem *bubble = part(myRow, "messageBubble");
        QVERIFY(myActions->mapToScene(QPointF(0, 0)).y()
                >= bubble->mapToScene(QPointF(0, bubble->height())).y());
        QVERIFY(myActions->mapToScene(QPointF(0, myActions->height())).y()
                <= myRow->mapToScene(QPointF(0, myRow->height())).y());
        QVERIFY(part(myRow, "messageEditAction")->isVisible());
        hover(part(theirRow, "messageBubble"));
        QTRY_VERIFY(part(theirRow, "messageActions")->isVisible());
        QVERIFY(!myActions->isVisible());
        QVERIFY(!part(theirRow, "messageEditAction")->isVisible());

        // Copy takes the whole message and says so.
        clickItem(window, part(theirRow, "messageCopyAction"));
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("Hey Daniel!"));
        QTRY_COMPARE(part(theirRow, "messageActionCaption")->property("text").toString(),
                     QStringLiteral("Copied"));

        // Edit: the bubble says so here, the composer holds the text, and Esc
        // leaves it as it was.
        auto *input = qobject_cast<QQuickItem *>(window->findChild<QObject *>(
            QStringLiteral("messageInput")));
        auto *bar = qobject_cast<QQuickItem *>(window->findChild<QObject *>(
            QStringLiteral("composeBar")));
        auto *barTitle = window->findChild<QObject *>(QStringLiteral("composeBarTitle"));
        QVERIFY(input && bar && barTitle);
        const QString original = messages->data(messages->index(1),
                                                 OpenChat::MessageListModel::BodyRole).toString();
        hover(part(myRow, "messageBubble"));
        QTRY_VERIFY(part(myRow, "messageEditAction")->isVisible());
        clickItem(window, part(myRow, "messageEditAction"));
        QCOMPARE(controller.editingMessageId(), mine);
        QCOMPARE(myBody->property("text").toString(), QStringLiteral("editing..."));
        QVERIFY(bar->isVisible());
        QCOMPARE(barTitle->property("text").toString(), QStringLiteral("Editing message"));
        QTRY_VERIFY(input->hasActiveFocus());
        QCOMPARE(input->property("text").toString(), original);
        QTest::keyClick(window, Qt::Key_Escape);
        QVERIFY(controller.editingMessageId().isEmpty());
        QVERIFY(!bar->isVisible());
        QCOMPARE(myBody->property("text").toString(), original);
        QVERIFY(!part(myRow, "messageEdited")->isVisible());

        // Sent, the new text shows with "edited" below it, above the actions.
        QVERIFY(controller.beginEdit(mine));
        QTRY_VERIFY(input->hasActiveFocus());
        controller.setComposerText(QStringLiteral("Hey Michael, how are you?"));
        QTest::keyClick(window, Qt::Key_Return);
        QVERIFY(controller.editingMessageId().isEmpty());
        QCOMPARE(myBody->property("text").toString(), QStringLiteral("Hey Michael, how are you?"));
        QQuickItem *editedLabel = part(myRow, "messageEdited");
        QVERIFY(editedLabel->isVisible());
        QVERIFY(editedLabel->y() >= bubble->y() + bubble->height());
        QVERIFY(myActions->y() >= editedLabel->y() + editedLabel->height());

        // Reply: the bar names who is answered, and the answer quotes them.
        hover(part(theirRow, "messageBubble"));
        QTRY_VERIFY(part(theirRow, "messageReplyAction")->isVisible());
        clickItem(window, part(theirRow, "messageReplyAction"));
        QCOMPARE(controller.replyingToMessageId(), theirs);
        QCOMPARE(barTitle->property("text").toString(), QStringLiteral("Replying to Michael"));
        QTRY_VERIFY(input->hasActiveFocus());
        controller.setComposerText(QStringLiteral("All good here"));
        QTest::keyClick(window, Qt::Key_Return);
        QVERIFY(controller.replyingToMessageId().isEmpty());
        QQuickItem *answer = nullptr;
        QTRY_VERIFY((answer = delegateFor(id(messages->rowCount() - 1))));
        QVERIFY(part(answer, "messageQuote")->isVisible());
        QCOMPARE(part(answer, "messageQuoteSender")->property("text").toString(),
                 QStringLiteral("Michael"));
        QCOMPARE(part(answer, "messageQuoteText")->property("text").toString(),
                 QStringLiteral("Hey Daniel!"));

        // Clicking into a message's text gives it the keyboard, so Ctrl+C
        // copies what is selected there; typing after that still lands in the
        // composer.
        QGuiApplication::clipboard()->clear();
        QQuickItem *theirBody = part(theirRow, "messageBody");
        clickItem(window, theirBody);
        QTRY_VERIFY(theirBody->hasActiveFocus());
        QVERIFY(QMetaObject::invokeMethod(theirBody, "select", Q_ARG(int, 0), Q_ARG(int, 3)));
        QTest::keyClick(window, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("Hey"));
        typeText(window, QStringLiteral("ok"));
        QTRY_VERIFY(input->hasActiveFocus());
        QCOMPARE(controller.composerText(), QStringLiteral("ok"));
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
        appearance->setOwnedCosmetics({nebula});
        appearance->setBubbleSkin(nebula);
        QCOMPARE(myBubble->property("skin").toString(), nebula);
        QVERIFY(myBubble->property("skinned").toBool());
        QCOMPARE(myBody->property("color").value<QColor>(), OpenChat::BubbleSkins::textColor(nebula));
        QCOMPARE(myTime->property("color").value<QColor>(),
                 OpenChat::BubbleSkins::secondaryTextColor(nebula));
        QCOMPARE(myBody->property("style").toInt(), 2); // Text.Raised
        QCOMPARE(myBody->property("styleColor").value<QColor>(),
                 OpenChat::BubbleSkins::textShadowColor(nebula));
        // Mine is mine: someone else's bubble wears only what they wear.
        QCOMPARE(theirBubble->property("skin").toString(), QString());
        QCOMPARE(theirBody->property("color").value<QColor>(), classicText);

        // The choice is remembered.
        QCOMPARE(QSettings().value(QStringLiteral("Appearance/bubbleSkin")).toString(), nebula);
        OpenChat::AppearanceSettings reloaded;
        reloaded.setOwnedCosmetics({nebula});
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

    // What other people wear (PeerCosmetics, filled from the relay) shows on
    // their bubbles, their row and the conversation header -- and only there.
    void othersWearWhatTheRelaySaysTheyWear()
    {
        auto *peers = OpenChat::PeerCosmetics::instance();
        peers->clear();
        const auto cleanup = qScopeGuard([peers] { peers->clear(); });

        // Their bubbles.
        {
            QQmlEngine engine;
            engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
            QQmlComponent component(&engine);
            component.loadFromModule("OpenChat", "MessageDelegate");
            QVERIFY2(component.isReady(), qPrintable(component.errorString()));
            const auto incoming = [&component](const QString &sender) {
                return component.createWithInitialProperties({
                    {"direction", 0}, {"deliveryState", 3}, {"body", "hello"},
                    {"timestamp", "10:15 AM"}, {"kind", 0}, {"dateLabel", ""},
                    {"showDateDivider", false}, {"senderName", ""}, {"width", 540},
                    {"senderAccount", sender}});
            };
            QScopedPointer<QObject> alice(incoming(QStringLiteral("alice")));
            QScopedPointer<QObject> bob(incoming(QStringLiteral("bob")));
            auto *aliceBubble = alice->findChild<QObject *>(QStringLiteral("messageBubble"));
            auto *bobBubble = bob->findChild<QObject *>(QStringLiteral("messageBubble"));
            QVERIFY(aliceBubble && bobBubble);
            QCOMPARE(aliceBubble->property("skin").toString(), QString());
            peers->setLoadout(QStringLiteral("alice"), {{QStringLiteral("bubble"), QStringLiteral("bubble.magma")}});
            QCOMPARE(aliceBubble->property("skin").toString(), QStringLiteral("bubble.magma"));
            QVERIFY(aliceBubble->property("skinned").toBool());
            QCOMPARE(bobBubble->property("skin").toString(), QString());
            // An id in the wrong slot, or unknown to this build, is worn by nobody.
            peers->setLoadout(QStringLiteral("bob"), {{QStringLiteral("bubble"), QStringLiteral("frame.neon")}});
            QCOMPARE(bobBubble->property("skin").toString(), QString());
            peers->setLoadout(QStringLiteral("alice"), {});
            QCOMPARE(aliceBubble->property("skin").toString(), QString());
        }

        // Their row and the header of their chat (the preview opens Michael's).
        OpenChat::ChatController chats;
        chats.setLocalUserName(QStringLiteral("Developer"));
        QQmlApplicationEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        engine.setInitialProperties({{QStringLiteral("chatController"), QVariant::fromValue(&chats)}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");
        QCOMPARE(engine.rootObjects().size(), 1);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
        QVERIFY(window && QTest::qWaitForWindowExposed(window));
        QCOMPARE(chats.currentContactId(), QStringLiteral("michael"));
        QQuickItem *root = window->contentItem();
        // Rows come from repeaters, so walk the visual tree for them.
        const auto rowAvatar = [root](const QString &contact) -> QQuickItem * {
            QList<QQuickItem *> pending{root};
            while (!pending.isEmpty()) {
                QQuickItem *item = pending.takeFirst();
                if (item->objectName() == QLatin1String("contactAvatar") && item->parentItem()
                    && item->parentItem()->property("contactId").toString() == contact)
                    return item;
                pending.append(item->childItems());
            }
            return nullptr;
        };
        const auto frameOf = [](QQuickItem *avatar) -> QString {
            auto *frame = avatar ? findVisualItem(avatar, QStringLiteral("avatarFrame")) : nullptr;
            return frame ? frame->property("frameId").toString() : QString();
        };
        auto *headerAvatar = findVisualItem(root, QStringLiteral("conversationAvatar"));
        auto *headerBead = findVisualItem(root, QStringLiteral("conversationBead"));
        QVERIFY(rowAvatar(QStringLiteral("michael")) && rowAvatar(QStringLiteral("sarah")) && headerAvatar
                && headerBead);
        QCOMPARE(frameOf(headerAvatar), QString());
        QVERIFY(!findVisualItem(root, QStringLiteral("conversationFlair")));
        QVERIFY(!findVisualItem(root, QStringLiteral("conversationScene")));

        peers->setLoadouts({
            {QStringLiteral("michael"), {{QStringLiteral("frame"), QStringLiteral("frame.gilded")},
                                         {QStringLiteral("bead"), QStringLiteral("bead.gem")},
                                         {QStringLiteral("flair"), QStringLiteral("flair.holo")},
                                         {QStringLiteral("scene"), QStringLiteral("scene.aurora")}}},
            {QStringLiteral("sarah"), {{QStringLiteral("frame"), QStringLiteral("frame.neon")}}},
        });
        QTRY_COMPARE(frameOf(rowAvatar(QStringLiteral("michael"))), QStringLiteral("frame.gilded"));
        QCOMPARE(frameOf(rowAvatar(QStringLiteral("sarah"))), QStringLiteral("frame.neon"));
        QCOMPARE(frameOf(rowAvatar(QStringLiteral("alex"))), QString());
        QCOMPARE(frameOf(headerAvatar), QStringLiteral("frame.gilded"));
        QCOMPARE(headerBead->property("styleId").toString(), QStringLiteral("bead.gem"));
        QTRY_VERIFY(findVisualItem(root, QStringLiteral("conversationFlair")));
        QCOMPARE(findVisualItem(root, QStringLiteral("conversationFlair"))->property("flairId").toString(),
                 QStringLiteral("flair.holo"));
        QCOMPARE(findVisualItem(root, QStringLiteral("conversationTitle"))->property("color").value<QColor>().alpha(), 0);
        QVERIFY(findVisualItem(root, QStringLiteral("conversationScene")));
        QCOMPARE(findVisualItem(root, QStringLiteral("conversationScene"))->property("sceneId").toString(),
                 QStringLiteral("scene.aurora"));
        if (const QString dir = qEnvironmentVariable("OPENCHAT_COSMETIC_CAPTURES"); !dir.isEmpty()) {
            for (const bool dark : {false, true}) {
                engine.singletonInstance<OpenChat::AppearanceSettings *>("OpenChat.Native", "AppearanceSettings")
                    ->setDarkMode(dark);
                QTest::qWait(300);
                window->grabWindow().save(dir + (dark ? "/peer-cosmetics-dark.png" : "/peer-cosmetics-light.png"));
            }
            engine.singletonInstance<OpenChat::AppearanceSettings *>("OpenChat.Native", "AppearanceSettings")
                ->setDarkMode(false);
        }
        // My own header is not theirs.
        QVERIFY(!findVisualItem(root, QStringLiteral("profileScene")));
        QVERIFY(!findVisualItem(root, QStringLiteral("localNameFlair")));

        // Switching chats follows the person; taking it all off restores the stock look.
        chats.selectContact(QStringLiteral("sarah"));
        QTRY_COMPARE(frameOf(headerAvatar), QStringLiteral("frame.neon"));
        QTRY_VERIFY(!findVisualItem(root, QStringLiteral("conversationScene")));
        peers->clear();
        QTRY_COMPARE(frameOf(headerAvatar), QString());
        QCOMPARE(frameOf(rowAvatar(QStringLiteral("michael"))), QString());
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.isEmpty() ? QString()
            : warnings.constFirst().constFirst().value<QList<QQmlError>>().value(0).toString()));
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

        const auto bubbleWidth = [&component](const QString &body, qreal paneWidth = 540) {
            QScopedPointer<QObject> delegate(component.createWithInitialProperties(
                {{QStringLiteral("deliveryState"), 0},
                 {QStringLiteral("direction"), 0},
                 {QStringLiteral("body"), body},
                 {QStringLiteral("timestamp"), QStringLiteral("10:15 AM")},
                 {QStringLiteral("kind"), 0},
                 {QStringLiteral("dateLabel"), QStringLiteral("May 24, 2010")},
                 {QStringLiteral("showDateDivider"), false},
                 {QStringLiteral("senderName"), QString()},
                 {QStringLiteral("width"), paneWidth}}));
            return delegate ? delegate->property("bubbleWidth").toDouble() : -1.0;
        };

        const qreal shortWidth = bubbleWidth(QStringLiteral("Hi"));
        const qreal referenceWidth = bubbleWidth(QStringLiteral("Hey Daniel!"));
        const qreal sentenceWidth = bubbleWidth(
            QStringLiteral("Pretty good, just working on some stuff. You?"));
        QVERIFY(shortWidth >= 70.0);
        QVERIFY(shortWidth < 100.0);
        QVERIFY(shortWidth < referenceWidth);
        QVERIFY(referenceWidth < sentenceWidth);

        // A wider pane leaves short messages alone and lets a long one run
        // past the old 360px cap, up to a readable line length.
        const QString paragraph = QStringLiteral(
            "Pretty good, just working on some stuff. The crash report came back with "
            "a full stack trace this time, so I can finally see where it goes wrong.");
        QCOMPARE(bubbleWidth(QStringLiteral("Hi"), 1600), shortWidth);
        QCOMPARE(bubbleWidth(QStringLiteral("Hey Daniel!"), 1600), referenceWidth);
        const qreal wideParagraphWidth = bubbleWidth(paragraph, 1600);
        QVERIFY2(wideParagraphWidth > 600.0, qPrintable(QString::number(wideParagraphWidth)));
        QVERIFY(wideParagraphWidth <= 720.0);

        // A narrow pane still wraps it within most of its width.
        QVERIFY(bubbleWidth(paragraph) <= 540.0 * 0.72);

        // Wrapped text sizes the bubble by its widest line: two words that
        // cannot share a line leave no slack beside either.
        const QString word(30, QLatin1Char('m'));
        const qreal wordWidth = bubbleWidth(word, 1600);
        QVERIFY2(wordWidth > 720.0 / 2 && wordWidth < 720.0, qPrintable(QString::number(wordWidth)));
        QCOMPARE(bubbleWidth(word + QLatin1Char(' ') + word, 1600), wordWidth);
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
        // The bubble also offers the way into the profile.
        auto *profileLine = bubble->findChild<QQuickItem *>(QStringLiteral("contactStatusBubbleProfileLine"));
        QVERIFY(profileLine && profileLine->isVisible());

        // The picture opens Alex's profile and leaves the open chat alone; the
        // rest of the row still selects the chat.
        OpenChat::ProfileController *profiles = controller.profiles();
        const QString openChat = controller.currentContactId();
        QVERIFY(openChat != QStringLiteral("alex"));
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, avatarPoint);
        QTRY_VERIFY(profiles->isOpen());
        QCOMPARE(profiles->personId(), QStringLiteral("alex"));
        QCOMPARE(controller.currentContactId(), openChat);
        QTRY_VERIFY(!bubble->isVisible());
        profiles->closeAll();
        auto *profileLoader = window->findChild<QQuickItem *>(QStringLiteral("profileLoader"));
        QVERIFY(profileLoader);
        QTRY_VERIFY(!profileLoader->property("active").toBool());
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, namePoint);
        QCOMPARE(controller.currentContactId(), QStringLiteral("alex"));
        QVERIFY(!profiles->isOpen());

        // Without a status line a person's bubble still says how to reach
        // their profile, on its own.
        QTest::mouseMove(window, namePoint);
        QVERIFY(row->setProperty("statusText", QString()));
        QTest::mouseMove(window, avatarPoint);
        QVERIFY(row->property("avatarHovered").toBool());
        QTRY_COMPARE(bubble->opacity(), 1.0);
        QVERIFY(profileLine->isVisible());
        QVERIFY(!text->isVisible());
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

        // Everything is present but dormant: no rings, shades, menu or editor.
        // Your own picture opens your profile now, so its hover is the profile
        // rings, and the picture dialog lives on that page, not here.
        auto *avatarButton = findVisualItem(window->contentItem(), QStringLiteral("localAvatarButton"));
        auto *avatarShade = avatarButton ? findVisualItem(avatarButton, QStringLiteral("profileAffordanceRings"))
                                         : nullptr;
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
        QVERIFY(!root->findChild<QObject *>(QStringLiteral("localAvatarFileDialog")));
        QVERIFY(!avatarShade->isVisible());
        QVERIFY(!statusShade->isVisible());
        QVERIFY(!statusInput->isVisible());
        QVERIFY(!presenceShade->isVisible());
        QVERIFY(!presenceMenu->isVisible());
        QVERIFY(!notice->isVisible());
        QCOMPARE(statusText->property("text").toString(), QStringLiteral("Available"));

        // Hovering the picture rings it; hovering the status line tints the
        // field; hovering the bead darkens it.
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

        // Clicking your own picture opens your profile, where the picture
        // dialog now lives.
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centre(avatarButton));
        QTRY_VERIFY(controller.profiles()->isOpen());
        QVERIFY(controller.profiles()->isOwnProfile());
        QTRY_VERIFY(root->findChild<QObject *>(QStringLiteral("localAvatarFileDialog")));
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

        // Sound controls appear only while the share is arriving with sound.
        auto *sound = root->findChild<QQuickItem *>(QStringLiteral("remoteScreenSoundControls"));
        auto *speaker = root->findChild<QQuickItem *>(QStringLiteral("remoteScreenSoundButton"));
        auto *volume = root->findChild<QQuickItem *>(QStringLiteral("remoteScreenVolume"));
        QVERIFY(sound && speaker && volume);
        QVERIFY2(!sound->isVisible(), "sound controls showed for a share without sound");
        callController.setPreviewRemoteScreenAudio(true);
        QCoreApplication::processEvents();
        QVERIFY2(sound->isVisible(), "a share arriving with sound showed no way to mute it");
        QCOMPARE(speaker->property("icon").toString(), QStringLiteral("speaker"));
        // The speaker mutes, and shows it; muting keeps the controls up.
        QVERIFY(QMetaObject::invokeMethod(speaker, "clicked"));
        QCoreApplication::processEvents();
        QVERIFY(callController.screenAudioMuted());
        QCOMPARE(speaker->property("icon").toString(), QStringLiteral("speakerMuted"));
        QVERIFY(sound->isVisible());
        QVERIFY(QMetaObject::invokeMethod(speaker, "clicked"));
        QVERIFY(!callController.screenAudioMuted());
        // The slider moves the volume, and follows it.
        QVERIFY(QMetaObject::invokeMethod(volume, "moved", Q_ARG(double, 0.4)));
        QCOMPARE(callController.screenAudioVolume(), 0.4);
        QCOMPARE(volume->property("value").toDouble(), 0.4);
        callController.setScreenAudioVolume(1.0);
        callController.setPreviewRemoteScreenAudio(false);
        QCoreApplication::processEvents();
        QVERIFY(!sound->isVisible());

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

        // The sound switch reflects, sets and remembers the choice, or is off
        // and says why on a machine that cannot share sound.
        auto *soundSwitch = root->findChild<QQuickItem *>(QStringLiteral("screenShareSoundSwitch"));
        auto *soundNote = root->findChild<QQuickItem *>(QStringLiteral("screenShareSoundNote"));
        QVERIFY(soundSwitch && soundNote);
        QVERIFY(!soundNote->property("text").toString().isEmpty());
        if (callController.screenAudioAvailable()) {
            QVERIFY(soundSwitch->isEnabled());
            const bool before = callController.shareScreenAudio();
            QCOMPARE(soundSwitch->property("checked").toBool(), before);
            QVERIFY(QMetaObject::invokeMethod(soundSwitch, "toggled", Q_ARG(bool, !before)));
            QCOMPARE(callController.shareScreenAudio(), !before);
            QCOMPARE(soundSwitch->property("checked").toBool(), !before);
            QCOMPARE(QSettings().value(QStringLiteral("Calls/shareScreenAudio")).toBool(), !before);
            callController.setShareScreenAudio(before);
        } else {
            QVERIFY(!soundSwitch->isEnabled());
            QCOMPARE(soundNote->property("text").toString(),
                     callController.screenAudioUnavailableReason());
        }

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

    void theTrayOrbColoursWhatTheMicrophoneIsDoing()
    {
        // Sampled below the gloss, where the glass shows its own colour.
        const auto glass = [](QStringView state) {
            return OpenChat::TrayOrb::render(state, 64).pixelColor(32, 46);
        };
        const QColor call = glass(u"call");
        const QColor talking = glass(u"talking");
        const QColor muted = glass(u"muted");
        const QColor deafened = glass(u"deafened");

        // Dark green in the call, bright green while talking.
        QVERIFY(call.green() > call.red() + 50 && call.green() > call.blue() + 50);
        QVERIFY(call.lightness() < 100);
        QVERIFY(talking.green() > talking.red() + 50 && talking.green() > talking.blue() + 50);
        QVERIFY(talking.lightness() > call.lightness() + 40);
        // Red when muted, dark grey when deafened.
        QVERIFY(muted.red() > muted.green() + 80 && muted.red() > muted.blue() + 80);
        QVERIFY(deafened.hslSaturation() < 50);
        QVERIFY(deafened.lightness() < 120);

        // A round orb: the corners are clear, the middle is solid glass.
        const QImage orb = OpenChat::TrayOrb::render(u"call", 64);
        QCOMPARE(orb.pixelColor(0, 0).alpha(), 0);
        QCOMPARE(orb.pixelColor(63, 63).alpha(), 0);
        QCOMPARE(orb.pixelColor(32, 32).alpha(), 255);
        QVERIFY(OpenChat::TrayOrb::render(u"idle", 64).isNull());

        // The icon is drawn at each size a tray asks for, never scaled.
        const QIcon icon = OpenChat::TrayOrb::icon(u"talking");
        const QList<QSize> sizes = icon.availableSizes();
        for (const int side : {16, 20, 24, 32, 48, 64})
            QVERIFY2(sizes.contains(QSize(side, side)), qPrintable(QString::number(side)));
        QCOMPARE(icon.pixmap(QSize(16, 16), 1.0).size(), QSize(16, 16));
        QVERIFY(OpenChat::TrayOrb::icon(u"").isNull());
    }

    void theTrayIconShowsTheCallAndOffersOpenAndClose()
    {
        const QIcon applicationIcon = QGuiApplication::windowIcon();
        QGuiApplication::setWindowIcon(QIcon(QStringLiteral(OPENCHAT_SOURCE_DIR "/assets/icons/openchat-256.png")));
        const auto restoreIcon = qScopeGuard([&] { QGuiApplication::setWindowIcon(applicationIcon); });

        FakeTray::Record shown;
        {
            OpenChat::TrayIcon tray(std::make_unique<FakeTray>(&shown));
            QVERIFY(shown.shown);

            // Outside a call: the application's own icon.
            QCOMPARE(shown.icon.cacheKey(), QGuiApplication::windowIcon().cacheKey());
            QCOMPARE(shown.toolTip, QStringLiteral("OpenChat"));

            // In one: the orb, and the tooltip says so without following speech.
            tray.setCallState(QStringLiteral("call"));
            QColor colour = trayColour(shown.icon);
            QVERIFY(colour.green() > colour.red() + 50 && colour.lightness() < 100);
            QCOMPARE(shown.toolTip, QStringLiteral("OpenChat — in a call"));
            tray.setCallState(QStringLiteral("talking"));
            QVERIFY(trayColour(shown.icon).lightness() > colour.lightness() + 40);
            QCOMPARE(shown.toolTip, QStringLiteral("OpenChat — in a call"));
            tray.setCallState(QStringLiteral("muted"));
            colour = trayColour(shown.icon);
            QVERIFY(colour.red() > colour.green() + 80);
            QCOMPARE(shown.toolTip, QStringLiteral("OpenChat — in a call, muted"));
            tray.setCallState(QStringLiteral("deafened"));
            QVERIFY(trayColour(shown.icon).hslSaturation() < 50);
            QCOMPARE(shown.toolTip, QStringLiteral("OpenChat — in a call, deafened"));
            tray.setCallState(QString());
            QCOMPARE(shown.icon.cacheKey(), QGuiApplication::windowIcon().cacheKey());

            // Right-click: Open, then Close.
            QVERIFY(shown.menu);
            QCOMPARE(shown.menu->items.size(), 2);
            QCOMPARE(static_cast<FakeTrayMenuItem *>(shown.menu->items.at(0))->text, QStringLiteral("Open"));
            QCOMPARE(static_cast<FakeTrayMenuItem *>(shown.menu->items.at(1))->text, QStringLiteral("Close"));
            QSignalSpy open(&tray, &OpenChat::TrayIcon::openRequested);
            QSignalSpy close(&tray, &OpenChat::TrayIcon::closeRequested);
            emit shown.menu->items.at(0)->activated();
            QCOMPARE(open.count(), 1);
            QCOMPARE(close.count(), 0);
            emit shown.menu->items.at(1)->activated();
            QCOMPARE(close.count(), 1);
        }
        // Gone with the application.
        QVERIFY(!shown.shown);
    }

    // A click or a double click on the icon opens the window; the right click
    // is the menu's, and the middle click does nothing.
    void aClickOnTheTrayIconOpensTheWindow()
    {
        FakeTray::Record shown;
        auto platform = std::make_unique<FakeTray>(&shown);
        FakeTray *fake = platform.get();
        OpenChat::TrayIcon tray(std::move(platform));
        QSignalSpy open(&tray, &OpenChat::TrayIcon::openRequested);
        emit fake->activated(QPlatformSystemTrayIcon::Trigger);
        emit fake->activated(QPlatformSystemTrayIcon::DoubleClick);
        QCOMPARE(open.count(), 2);
        emit fake->activated(QPlatformSystemTrayIcon::Context);
        emit fake->activated(QPlatformSystemTrayIcon::MiddleClick);
        QCOMPARE(open.count(), 2);
    }

    void theWindowDrivesItsTrayIcon()
    {
        OpenChat::ChatController chatController;
        chatController.setLocalUserName(QStringLiteral("Developer"));
        OpenChat::CallController callController;
        FakeTray::Record shown;
        OpenChat::TrayIcon tray(std::make_unique<FakeTray>(&shown));
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{QStringLiteral("chatController"), QVariant::fromValue(&chatController)},
             {QStringLiteral("callController"), QVariant::fromValue(&callController)},
             {QStringLiteral("tray"), QVariant::fromValue(&tray)}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");
        QCOMPARE(engine.rootObjects().size(), 1);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));

        // The icon follows the call.
        QCOMPARE(tray.callState(), QString());
        callController.enableForPreview(OpenChat::CallState::Active, QStringLiteral("Jessica"),
                                        QStringLiteral("jessica"), false, true);
        QCOMPARE(tray.callState(), QStringLiteral("talking"));
        callController.setPreviewMuted(true);
        QCOMPARE(tray.callState(), QStringLiteral("muted"));

        // Open brings the hidden window back; Close quits the application.
        window->hide();
        emit tray.openRequested();
        QTRY_VERIFY(window->isVisible());
        QSignalSpy quit(&engine, &QQmlEngine::quit);
        emit tray.closeRequested();
        QCOMPARE(quit.count(), 1);
    }

    void theTrayShowsAnOrbOnlyWhileInAVoiceCall()
    {
        OpenChat::ChatController chatController;
        chatController.setLocalUserName(QStringLiteral("Developer"));
        OpenChat::CallController callController;
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{QStringLiteral("chatController"), QVariant::fromValue(&chatController)},
             {QStringLiteral("callController"), QVariant::fromValue(&callController)}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");
        QCOMPARE(engine.rootObjects().size(), 1);
        QObject *root = engine.rootObjects().constFirst();
        const auto trayState = [root] { return root->property("trayCallState").toString(); };

        // Only the application has a tray; a preview never closes into one.
        QVERIFY(root->property("tray").value<QObject *>() == nullptr);

        QCOMPARE(trayState(), QString());
        // Ringing is not being in the call yet.
        callController.enableForPreview(OpenChat::CallState::Ringing, QStringLiteral("Jessica"),
                                        QStringLiteral("jessica"), false, false);
        QCOMPARE(trayState(), QString());
        callController.enableForPreview(OpenChat::CallState::Active, QStringLiteral("Jessica"),
                                        QStringLiteral("jessica"), true, false);
        QCOMPARE(trayState(), QStringLiteral("call"));
        callController.enableForPreview(OpenChat::CallState::Active, QStringLiteral("Jessica"),
                                        QStringLiteral("jessica"), false, true);
        QCOMPARE(trayState(), QStringLiteral("talking"));
        // Muted outranks talking: the orb says nobody hears it.
        callController.setPreviewMuted(true);
        QCOMPARE(trayState(), QStringLiteral("muted"));
        callController.setPreviewMuted(false);
        QCOMPARE(trayState(), QStringLiteral("talking"));
        // The call's ended surface is no longer a voice chat.
        callController.enableForPreview(OpenChat::CallState::Ended, QStringLiteral("Jessica"),
                                        QStringLiteral("jessica"), false, false);
        QCOMPARE(trayState(), QString());
    }

    void closingTheWindowHidesItIntoTheTrayUntilTheApplicationQuits()
    {
        OpenChat::ChatController chatController;
        chatController.setLocalUserName(QStringLiteral("Developer"));
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{QStringLiteral("chatController"), QVariant::fromValue(&chatController)}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");
        QCOMPARE(engine.rootObjects().size(), 1);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));

        const bool quitOnLastWindowClosed = QGuiApplication::quitOnLastWindowClosed();
        {
            OpenChat::CloseToTray closeToTray(window);
            // Another window closing must not end an application whose main
            // window is out of sight in the tray.
            QVERIFY(!QGuiApplication::quitOnLastWindowClosed());

            // Closed, the window is only hidden: still there to come back.
            QVERIFY(!window->close());
            QVERIFY(!window->isVisible());
            QVERIFY(window->handle() != nullptr);

            // Qt closes every window on its way out, and a quit must never be
            // refused by a window hiding itself.
            window->show();
            QEvent quit(QEvent::Quit);
            QVERIFY(!closeToTray.eventFilter(QCoreApplication::instance(), &quit));
            QVERIFY(window->close());
            QVERIFY(!window->isVisible());

            // A quit that something else cancelled leaves it closing to the tray.
            QCoreApplication::processEvents();
            window->show();
            QVERIFY(QTest::qWaitForWindowExposed(window));
            QVERIFY(!window->close());
            QVERIFY(!window->isVisible());
        }
        QCOMPARE(QGuiApplication::quitOnLastWindowClosed(), quitOnLastWindowClosed);
    }

    void theWindowComesBackFromTheTrayAsItWasLeft()
    {
        OpenChat::ChatController chatController;
        chatController.setLocalUserName(QStringLiteral("Developer"));
        OpenChat::CallController callController;
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{QStringLiteral("chatController"), QVariant::fromValue(&chatController)},
             {QStringLiteral("callController"), QVariant::fromValue(&callController)}});
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        engine.loadFromModule("OpenChat", "Main");
        QCOMPARE(engine.rootObjects().size(), 1);
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        const auto bringToFront = [window] {
            QVERIFY(QMetaObject::invokeMethod(window, "bringToFront"));
        };

        // Maximised, hidden into the tray, and opened again: still maximised.
        window->showMaximized();
        QTRY_COMPARE(window->visibility(), QWindow::Maximized);
        window->hide();
        QVERIFY(!window->isVisible());
        bringToFront();
        QVERIFY(window->isVisible());
        QTRY_COMPARE(window->visibility(), QWindow::Maximized);

        // Minimised to the taskbar, it comes back up as well.
        window->showNormal();
        QTRY_COMPARE(window->visibility(), QWindow::Windowed);
        window->showMinimized();
        QTRY_COMPARE(window->visibility(), QWindow::Minimized);
        bringToFront();
        QTRY_COMPARE(window->visibility(), QWindow::Windowed);

        // A call that rings while the window is in the tray brings it back.
        window->hide();
        callController.enableForPreview(OpenChat::CallState::Ringing, QStringLiteral("Jessica"),
                                        QStringLiteral("jessica"), false, false);
        QTRY_VERIFY(window->isVisible());
    }

    // --- Profile pages in the window (ARCH §9.9) ---------------------------

    void contactRowPictureHoverShowsTheAffordance()
    {
        failOnQmlWarnings();
        OpenChat::ChatController controller;
        QQmlApplicationEngine engine;
        QQuickWindow *window = showMain(engine, {{QStringLiteral("chatController"),
                                                  QVariant::fromValue(&controller)}});
        QVERIFY(window);
        auto *category = window->findChild<QQuickItem *>(QStringLiteral("contactsCategory"));
        QVERIFY(category);
        auto *row = findVisualItem(category, QStringLiteral("contactRow_alex"));
        QVERIFY(row);
        auto *avatar = row->findChild<QQuickItem *>(QStringLiteral("contactAvatar"));
        auto *affordance = row->findChild<QQuickItem *>(QStringLiteral("contactAvatarAffordance"));
        QVERIFY(avatar && affordance);
        auto *rings = findVisualItem(affordance, QStringLiteral("profileAffordanceRings"));
        QVERIFY(rings);

        // The rest of the row is the chat; only the picture lights up.
        QTest::mouseMove(window, row->mapToScene(QPointF(row->width() - 30, 14)).toPoint());
        QVERIFY(!rings->isVisible());
        QTest::mouseMove(window, centreOf(avatar));
        QTRY_VERIFY(rings->isVisible());
        QTRY_COMPARE(rings->opacity(), 1.0);
        QVERIFY(row->property("pointerOnAvatar").toBool());
        captureProfileShot(window, QStringLiteral("in-context-row-hover"));
        QTest::mouseMove(window, row->mapToScene(QPointF(row->width() - 30, 14)).toPoint());
        QTRY_VERIFY(!rings->isVisible());
    }

    void ownSidebarAvatarOpensOwnProfile()
    {
        failOnQmlWarnings();
        OpenChat::ChatController controller;
        QQmlApplicationEngine engine;
        QQuickWindow *window = showMain(engine, {{QStringLiteral("chatController"),
                                                  QVariant::fromValue(&controller)}});
        QVERIFY(window);
        auto *affordance = findVisualItem(window->contentItem(), QStringLiteral("localAvatarAffordance"));
        QVERIFY(affordance);
        QCOMPARE(affordance->property("accessibleName").toString(), QStringLiteral("View your profile"));
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centreOf(affordance));
        OpenChat::ProfileController *profiles = controller.profiles();
        QTRY_VERIFY(profiles->isOpen());
        QVERIFY(profiles->isOwnProfile());
        QTRY_VERIFY(settledProfilePage(window));
        // "Change picture" belongs to your own page now.
        QVERIFY(window->findChild<QObject *>(QStringLiteral("localAvatarFileDialog")));
        captureProfileShot(window, QStringLiteral("in-context-own-profile"));
    }

    void conversationHeaderAvatarOpensProfile()
    {
        failOnQmlWarnings();
        OpenChat::ChatController controller;
        QQmlApplicationEngine engine;
        QQuickWindow *window = showMain(engine, {{QStringLiteral("chatController"),
                                                  QVariant::fromValue(&controller)}});
        QVERIFY(window);
        const QString contact = controller.currentContactId();
        QVERIFY(!contact.isEmpty());
        auto *affordance =
            findVisualItem(window->contentItem(), QStringLiteral("conversationAvatarAffordance"));
        QVERIFY(affordance && affordance->isVisible());
        QCOMPARE(affordance->property("shownBadgeSize").toInt(), 22); // the 68 px picture's badge
        OpenChat::ProfileController *profiles = controller.profiles();

        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centreOf(affordance));
        QTRY_VERIFY(profiles->isOpen());
        QCOMPARE(profiles->personId(), contact);
        QCOMPARE(profiles->origin(), int(OpenChat::Profile::Origin::FromChat));
        QTRY_VERIFY(settledProfilePage(window));
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!profiles->isOpen());
        QTRY_VERIFY(!profileLoaderActive(window));

        // The picture is a Tab stop: Enter opens the profile from the keyboard.
        affordance->forceActiveFocus(Qt::TabFocusReason);
        QVERIFY(affordance->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_VERIFY(profiles->isOpen());
        QCOMPARE(profiles->personId(), contact);
    }

    void groupHeaderAvatarDoesNothing()
    {
        failOnQmlWarnings();
        OpenChat::ChatController controller;
        QVector<OpenChat::Contact> contacts;
        for (int i = 0; i < controller.contacts()->rowCount(); ++i)
            contacts.append(*controller.contacts()->contactAt(i));
        OpenChat::Contact group;
        group.id = QStringLiteral("weekend");
        group.name = QStringLiteral("Weekend plans");
        group.avatarKey = QStringLiteral("group");
        group.isGroup = true;
        contacts.append(group);
        controller.contacts()->setContacts(contacts);
        QQmlApplicationEngine engine;
        QQuickWindow *window = showMain(engine, {{QStringLiteral("chatController"),
                                                  QVariant::fromValue(&controller)}});
        QVERIFY(window);
        // A group's row: no rings on its picture, and the picture selects the
        // chat like the rest of the row.
        auto *category = window->findChild<QQuickItem *>(QStringLiteral("contactsCategory"));
        auto *row = category ? findVisualItem(category, QStringLiteral("contactRow_weekend")) : nullptr;
        QVERIFY(row);
        auto *rowAffordance = row->findChild<QQuickItem *>(QStringLiteral("contactAvatarAffordance"));
        auto *rowPicture = row->findChild<QQuickItem *>(QStringLiteral("contactAvatar"));
        QVERIFY(rowAffordance && rowPicture);
        QVERIFY(!rowAffordance->isVisible());
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centreOf(rowPicture));
        QTRY_COMPARE(controller.currentContactId(), QStringLiteral("weekend"));
        QVERIFY(!controller.profiles()->isOpen());
        // The header's picture opens nothing for a group either. (Only a live
        // session knows its groups, so this preview's header cannot hide its
        // rings; the controller refuses a group all the same.)
        auto *header = findVisualItem(window->contentItem(), QStringLiteral("conversationHeader"));
        auto *picture = header ? findVisualItem(header, QStringLiteral("roundedAvatarArtwork")) : nullptr;
        QVERIFY(picture);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centreOf(picture));
        QTest::qWait(200);
        QVERIFY(!controller.profiles()->isOpen());
        QVERIFY(!controller.profiles()->openContact(QStringLiteral("weekend")));
    }

    void requestRowAvatarOpensStubWithAcceptDecline()
    {
        failOnQmlWarnings();
        OpenChat::ChatController chatController;
        OpenChat::ContactController contactController;
        contactController.enableForPreview();
        contactController.addMockRequest(QStringLiteral("Grace"), QStringLiteral("wants to chat with you"));
        QQmlApplicationEngine engine;
        QQuickWindow *window = showMain(engine,
            {{QStringLiteral("chatController"), QVariant::fromValue(&chatController)},
             {QStringLiteral("contactController"), QVariant::fromValue(&contactController)}});
        QVERIFY(window);
        OpenChat::RequestListModel *model = contactController.requests();
        QCOMPARE(model->count(), 1);
        const QString requestId = model->data(model->index(0), OpenChat::RequestListModel::IdRole).toString();
        auto *panel = window->findChild<QQuickItem *>(QStringLiteral("requestsPanel"));
        QVERIFY(panel);
        auto *row = findVisualItem(panel, QStringLiteral("requestRow_") + requestId);
        QVERIFY(row);
        auto *identity = findVisualItem(row, QStringLiteral("requestIdentityArea"));
        auto *affordance = findVisualItem(row, QStringLiteral("requestAvatarAffordance"));
        QVERIFY(identity && affordance && affordance->isVisible());

        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centreOf(affordance));
        OpenChat::ProfileController *profiles = chatController.profiles();
        QTRY_VERIFY(profiles->isOpen());
        QCOMPARE(profiles->relationship(), int(OpenChat::Profile::Relationship::IncomingRequestPerson));
        QCOMPARE(profiles->requestId(), requestId);
        QCOMPARE(profiles->pageState(), int(OpenChat::Profile::PageState::StubPage));
        QTRY_VERIFY(settledProfilePage(window));
        auto *accept = findVisualItem(window->contentItem(), QStringLiteral("profileAcceptButton"));
        auto *decline = findVisualItem(window->contentItem(), QStringLiteral("profileDeclineButton"));
        QVERIFY(accept && accept->isVisible());
        QVERIFY(decline && decline->isVisible());
        captureProfileShot(window, QStringLiteral("in-context-request-stub"));

        // Decline really declines, and leaves the stub.
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centreOf(decline));
        QTRY_COMPARE(model->count(), 0);
        QTRY_VERIFY(!profiles->isOpen());
    }

    void directoryResultAvatarOpensStub()
    {
        failOnQmlWarnings();
        OpenChat::ChatController chatController;
        OpenChat::ContactController contactController;
        contactController.enableForPreview();
        contactController.setMockDirectory({QStringLiteral("alice")});
        QQmlApplicationEngine engine;
        QQuickWindow *window = showMain(engine,
            {{QStringLiteral("chatController"), QVariant::fromValue(&chatController)},
             {QStringLiteral("contactController"), QVariant::fromValue(&contactController)}});
        QVERIFY(window);
        chatController.setSearchQuery(QStringLiteral("alice"));
        contactController.lookup(QStringLiteral("alice"));
        QTRY_COMPARE(contactController.lookupState(), OpenChat::ContactController::LookupState::Found);
        auto *affordance = findVisualItem(window->contentItem(), QStringLiteral("directoryAvatarAffordance"));
        QVERIFY(affordance);
        QTRY_VERIFY(affordance->isVisible());
        QCOMPARE(affordance->property("accessibleName").toString(), QStringLiteral("Open @alice"));

        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centreOf(affordance));
        OpenChat::ProfileController *profiles = chatController.profiles();
        QTRY_VERIFY(profiles->isOpen());
        QCOMPARE(profiles->relationship(), int(OpenChat::Profile::Relationship::StrangerPerson));
        QCOMPARE(profiles->origin(), int(OpenChat::Profile::Origin::FromSearch));
        QTRY_VERIFY(settledProfilePage(window));
        // The one real action for a stranger found by handle.
        auto *send = findVisualItem(window->contentItem(), QStringLiteral("profileSendRequestButton"));
        QVERIFY(send && send->isVisible());
    }

    void callTilesOpenProfilesWhenCameraIsOff()
    {
        failOnQmlWarnings();
        OpenChat::ChatController chatController;
        OpenChat::CallController callController;
        QQmlApplicationEngine engine;
        QQuickWindow *window = showMain(engine,
            {{QStringLiteral("chatController"), QVariant::fromValue(&chatController)},
             {QStringLiteral("callController"), QVariant::fromValue(&callController)}});
        QVERIFY(window);
        OpenChat::ProfileController *profiles = chatController.profiles();

        // One-to-one: the far end's picture opens their page, ours our own.
        callController.enableForPreview(OpenChat::CallState::Active, QStringLiteral("Jessica"),
                                        QStringLiteral("jessica"), false, false);
        callController.setPreviewCallChatId(QStringLiteral("jessica"));
        QCoreApplication::processEvents();
        auto *remote = window->findChild<QQuickItem *>(QStringLiteral("remoteParticipant"));
        auto *local = window->findChild<QQuickItem *>(QStringLiteral("localParticipant"));
        QVERIFY(remote && local);
        auto *remoteAffordance = findVisualItem(remote, QStringLiteral("participantAvatarAffordance"));
        auto *localAffordance = findVisualItem(local, QStringLiteral("participantAvatarAffordance"));
        QVERIFY(remoteAffordance && localAffordance);
        QTRY_VERIFY(remoteAffordance->isVisible());
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centreOf(remoteAffordance));
        QTRY_VERIFY(profiles->isOpen());
        QCOMPARE(profiles->personId(), QStringLiteral("jessica"));
        QCOMPARE(profiles->relationship(), int(OpenChat::Profile::Relationship::ContactPerson));
        profiles->closeAll();
        QTRY_VERIFY(!profileLoaderActive(window));
        QTRY_VERIFY(localAffordance->isVisible());
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centreOf(localAffordance));
        QTRY_VERIFY(profiles->isOpen());
        QVERIFY(profiles->isOwnProfile());
        profiles->closeAll();
        QTRY_VERIFY(!profileLoaderActive(window));

        // A group: members the roster names open (a contact's page or a
        // stranger's stub); a member it never named opens nothing.
        OpenChat::CallParticipantRow jessica{QStringLiteral("d1"), QStringLiteral("Jessica"),
                                             QStringLiteral("jessica"), QString(), true, false, false, 0.0};
        jessica.accountId = OpenChat::ProfileReferencePages::mockAccountFor(QStringLiteral("jessica")).toHex();
        OpenChat::CallParticipantRow stranger{QStringLiteral("d2"), QStringLiteral("Dana"),
                                              QStringLiteral("userpfp_none"), QString(), true, false, false, 0.0};
        stranger.accountId = OpenChat::ProfileReferencePages::mockAccountFor(QStringLiteral("dana-whitfield")).toHex();
        OpenChat::CallParticipantRow unnamed{QStringLiteral("d3"), QStringLiteral("Someone"),
                                             QStringLiteral("userpfp_none"), QString(), true, false, false, 0.0};
        callController.enableForGroupPreview(OpenChat::CallState::Active, QStringLiteral("Weekend plans"),
                                             {jessica, stranger, unnamed});
        QCoreApplication::processEvents();
        auto *jessicaTile = findVisualItem(window->contentItem(), QStringLiteral("groupParticipant_d1"));
        auto *strangerTile = findVisualItem(window->contentItem(), QStringLiteral("groupParticipant_d2"));
        auto *unnamedTile = findVisualItem(window->contentItem(), QStringLiteral("groupParticipant_d3"));
        QVERIFY(jessicaTile && strangerTile && unnamedTile);
        auto *jessicaAffordance = findVisualItem(jessicaTile, QStringLiteral("participantAvatarAffordance"));
        auto *strangerAffordance = findVisualItem(strangerTile, QStringLiteral("participantAvatarAffordance"));
        auto *unnamedAffordance = findVisualItem(unnamedTile, QStringLiteral("participantAvatarAffordance"));
        QVERIFY(jessicaAffordance && strangerAffordance && unnamedAffordance);
        // Wait for the Flow to lay the tiles out side by side.
        QTRY_VERIFY(strangerTile->x() > jessicaTile->x() + jessicaTile->width() / 2
                    || strangerTile->y() > jessicaTile->y());
        QTRY_VERIFY(jessicaAffordance->isVisible());
        QVERIFY(!unnamedAffordance->isVisible());
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centreOf(jessicaAffordance));
        QTRY_VERIFY(profiles->isOpen());
        QCOMPARE(profiles->personId(), QStringLiteral("jessica"));
        profiles->closeAll();
        QTRY_VERIFY(!profileLoaderActive(window));
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centreOf(strangerAffordance));
        QTRY_VERIFY(profiles->isOpen());
        QCOMPARE(profiles->relationship(), int(OpenChat::Profile::Relationship::StrangerPerson));
        profiles->closeAll();
        QTRY_VERIFY(!profileLoaderActive(window));

        // A live camera keeps meaning "enlarge": no profile affordance on it.
        QVERIFY(jessicaTile->setProperty("cameraEnabled", true));
        QTRY_VERIFY(!jessicaAffordance->isVisible());
    }

    void profileStaysOpenWhenACallRings()
    {
        failOnQmlWarnings();
        OpenChat::ChatController chatController;
        OpenChat::CallController callController;
        QQmlApplicationEngine engine;
        QQuickWindow *window = showMain(engine,
            {{QStringLiteral("chatController"), QVariant::fromValue(&chatController)},
             {QStringLiteral("callController"), QVariant::fromValue(&callController)}});
        QVERIFY(window);
        OpenChat::ProfileController *profiles = chatController.profiles();
        QVERIFY(profiles->openContact(QStringLiteral("jessica")));
        QTRY_VERIFY(settledProfilePage(window));
        callController.enableForPreview(OpenChat::CallState::Ringing, QStringLiteral("Ryan"),
                                        QStringLiteral("ryan"), false, false);
        QCoreApplication::processEvents();
        // A profile never hides a call: the page stays and shows the strip.
        QTest::qWait(200);
        QVERIFY(profiles->isOpen());
        auto *strip = findVisualItem(window->contentItem(), QStringLiteral("profileCallStripLoader"));
        QVERIFY(strip);
        QTRY_VERIFY(strip->isVisible() && strip->height() > 0);
        captureProfileShot(window, QStringLiteral("in-context-ringing"));
    }

    void escapeClosesProfileNotFullscreenCall()
    {
        failOnQmlWarnings();
        OpenChat::ChatController chatController;
        OpenChat::CallController callController;
        QQmlApplicationEngine engine;
        QQuickWindow *window = showMain(engine,
            {{QStringLiteral("chatController"), QVariant::fromValue(&chatController)},
             {QStringLiteral("callController"), QVariant::fromValue(&callController)}});
        QVERIFY(window);
        callController.enableForPreview(OpenChat::CallState::Active, QStringLiteral("Jessica"),
                                        QStringLiteral("jessica"), false, false);
        QCoreApplication::processEvents();
        QVERIFY(window->setProperty("callFullscreen", true));
        OpenChat::ProfileController *profiles = chatController.profiles();
        QVERIFY(profiles->openContact(QStringLiteral("michael")));
        QTRY_VERIFY(settledProfilePage(window));
        // One press closes one thing: the page first, then the full screen.
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!profiles->isOpen());
        QVERIFY(window->property("callFullscreen").toBool());
        QTRY_VERIFY(!profileLoaderActive(window));
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!window->property("callFullscreen").toBool());
    }

    void escapeWithTheSafetyNumberOpenClosesOnlyTheDialog()
    {
        failOnQmlWarnings();
        OpenChat::ChatController chatController;
        OpenChat::ContactController contactController;
        contactController.enableForPreview();
        QQmlApplicationEngine engine;
        QQuickWindow *window = showMain(engine,
            {{QStringLiteral("chatController"), QVariant::fromValue(&chatController)},
             {QStringLiteral("contactController"), QVariant::fromValue(&contactController)}});
        QVERIFY(window);
        OpenChat::ProfileController *profiles = chatController.profiles();
        QVERIFY(profiles->openContact(QStringLiteral("michael")));
        QTRY_VERIFY(settledProfilePage(window));
        contactController.openSafetyNumberPreview();
        QTRY_VERIFY(contactController.safetyNumberOpen());
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!contactController.safetyNumberOpen());
        QTest::qWait(100);
        QVERIFY(profiles->isOpen());
        // The next press is the page's.
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!profiles->isOpen());
    }

    void dialogsFloatAboveTheProfile()
    {
        failOnQmlWarnings();
        OpenChat::ChatController chatController;
        OpenChat::ContactController contactController;
        contactController.enableForPreview();
        QQmlApplicationEngine engine;
        QQuickWindow *window = showMain(engine,
            {{QStringLiteral("chatController"), QVariant::fromValue(&chatController)},
             {QStringLiteral("contactController"), QVariant::fromValue(&contactController)}});
        QVERIFY(window);
        QVERIFY(chatController.profiles()->openContact(QStringLiteral("michael")));
        QTRY_VERIFY(settledProfilePage(window));
        contactController.openSafetyNumberPreview();
        auto *dialog = window->findChild<QQuickItem *>(QStringLiteral("safetyNumberDialog"));
        QTRY_VERIFY(dialog = window->findChild<QQuickItem *>(QStringLiteral("safetyNumberDialog")));
        QVERIFY(dialog->isVisible());
        // Siblings stack in declaration order: the dialog's loader comes after
        // the page's, so it draws and takes the pointer above it.
        auto *pageLoader = window->findChild<QQuickItem *>(QStringLiteral("profileLoader"));
        QVERIFY(pageLoader);
        QQuickItem *dialogLoader = dialog;
        while (dialogLoader && dialogLoader->parentItem() != pageLoader->parentItem())
            dialogLoader = dialogLoader->parentItem();
        QVERIFY(dialogLoader);
        const QList<QQuickItem *> siblings = pageLoader->parentItem()->childItems();
        QVERIFY(siblings.indexOf(dialogLoader) > siblings.indexOf(pageLoader));
        QVERIFY(dialogLoader->z() >= pageLoader->z());
        captureProfileShot(window, QStringLiteral("in-context-safety-number"));
    }

    void typingOnAProfileNeverReachesTheComposer()
    {
        failOnQmlWarnings();
        OpenChat::ChatController controller;
        QQmlApplicationEngine engine;
        QQuickWindow *window = showMain(engine, {{QStringLiteral("chatController"),
                                                  QVariant::fromValue(&controller)}});
        QVERIFY(window);
        auto *input = findVisualItem(window->contentItem(), QStringLiteral("messageInput"));
        QVERIFY(input);
        QTRY_VERIFY(input->hasActiveFocus());
        const int messagesBefore = controller.messages()->rowCount();
        QTest::keyClick(window, Qt::Key_I, Qt::ControlModifier);
        QTRY_VERIFY(settledProfilePage(window));
        QVERIFY(!input->hasActiveFocus());
        typeText(window, QStringLiteral("hello"));
        QTest::keyClick(window, Qt::Key_Return);
        QTest::qWait(100);
        QCOMPARE(controller.messages()->rowCount(), messagesBefore);
        QCOMPARE(input->property("text").toString(), QString());
    }

    void sidebarUnderThePageGetsNoHover()
    {
        failOnQmlWarnings();
        OpenChat::ChatController controller;
        QVector<OpenChat::Contact> contacts;
        for (int i = 0; i < controller.contacts()->rowCount(); ++i) {
            auto contact = *controller.contacts()->contactAt(i);
            if (contact.id == QStringLiteral("alex"))
                contact.statusText = QStringLiteral("Back after coffee");
            contacts.append(contact);
        }
        controller.contacts()->setContacts(contacts);
        QQmlApplicationEngine engine;
        QQuickWindow *window = showMain(engine, {{QStringLiteral("chatController"),
                                                  QVariant::fromValue(&controller)}});
        QVERIFY(window);
        auto *category = window->findChild<QQuickItem *>(QStringLiteral("contactsCategory"));
        auto *row = category ? findVisualItem(category, QStringLiteral("contactRow_alex")) : nullptr;
        QVERIFY(row);
        auto *avatar = row->findChild<QQuickItem *>(QStringLiteral("contactAvatar"));
        auto *bubble = row->findChild<QQuickItem *>(QStringLiteral("contactStatusBubble_alex"));
        QVERIFY(avatar && bubble);
        const QPoint avatarPoint = centreOf(avatar);
        QVERIFY(controller.profiles()->openContact(QStringLiteral("michael")));
        QTRY_VERIFY(settledProfilePage(window));
        QTest::mouseMove(window, avatarPoint);
        QTest::qWait(500);
        QVERIFY(!bubble->isVisible());
        QVERIFY(!row->property("pointerOnAvatar").toBool());
    }

    void messagesArrivingUnderThePageStayUnread()
    {
        failOnQmlWarnings();
        OpenChat::ChatController controller;
        QQmlApplicationEngine engine;
        QQuickWindow *window = showMain(engine, {{QStringLiteral("chatController"),
                                                  QVariant::fromValue(&controller)}});
        QVERIFY(window);
        const bool readBefore = controller.conversationVisible();
        QVERIFY(controller.profiles()->openContact(QStringLiteral("michael")));
        QTRY_VERIFY(!controller.conversationVisible());
        controller.profiles()->closeAll();
        QTRY_COMPARE(controller.conversationVisible(), readBefore);
    }

    void clickingTheEmptyTopBarDoesNotEditStatus()
    {
        failOnQmlWarnings();
        OpenChat::ChatController controller;
        QQmlApplicationEngine engine;
        QQuickWindow *window = showMain(engine, {{QStringLiteral("chatController"),
                                                  QVariant::fromValue(&controller)}});
        QVERIFY(window);
        auto *statusEditor = findVisualItem(window->contentItem(), QStringLiteral("localStatusEditor"));
        auto *statusInput = findVisualItem(window->contentItem(), QStringLiteral("localStatusInput"));
        QVERIFY(statusEditor && statusInput);
        const QPoint underneath = centreOf(statusEditor);
        QVERIFY(controller.profiles()->openContact(QStringLiteral("michael")));
        QTRY_VERIFY(settledProfilePage(window));
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, underneath);
        QTest::qWait(100);
        QVERIFY(!statusInput->isVisible());
        QVERIFY(controller.profiles()->isOpen());
    }

    void focusReturnsAfterClosing()
    {
        failOnQmlWarnings();
        OpenChat::ChatController controller;
        QQmlApplicationEngine engine;
        QQuickWindow *window = showMain(engine, {{QStringLiteral("chatController"),
                                                  QVariant::fromValue(&controller)}});
        QVERIFY(window);
        auto *input = findVisualItem(window->contentItem(), QStringLiteral("messageInput"));
        QVERIFY(input);
        QTRY_VERIFY(input->hasActiveFocus());
        QTest::keyClick(window, Qt::Key_I, Qt::ControlModifier);
        QTRY_VERIFY(settledProfilePage(window));
        QVERIFY(!input->hasActiveFocus());
        auto *page = settledProfilePage(window);
        QVERIFY(page);
        const QPointer<QQuickItem> firstPage(page);
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!controller.profiles()->isOpen());
        QTRY_VERIFY(input->hasActiveFocus());
        // The page that fades out is the page that was open, not a new one,
        // and once it has gone the keyboard is still with the composer.
        QTRY_VERIFY(!profileLoaderActive(window));
        QVERIFY(firstPage.isNull());
        QVERIFY(input->hasActiveFocus());
    }

    void sendMessageFromAProfileFocusesTheComposer()
    {
        failOnQmlWarnings();
        OpenChat::ChatController controller;
        QQmlApplicationEngine engine;
        QQuickWindow *window = showMain(engine, {{QStringLiteral("chatController"),
                                                  QVariant::fromValue(&controller)}});
        QVERIFY(window);
        auto *search = findVisualItem(window->contentItem(), QStringLiteral("contactSearch"));
        auto *input = findVisualItem(window->contentItem(), QStringLiteral("messageInput"));
        QVERIFY(search && input);
        // The keyboard was in the search field when the profile opened...
        search->forceActiveFocus();
        QTRY_VERIFY(search->hasActiveFocus());
        QVERIFY(controller.profiles()->openContact(QStringLiteral("jessica")));
        QTRY_VERIFY(settledProfilePage(window));
        auto *message = findVisualItem(window->contentItem(), QStringLiteral("profileAction_message"));
        QVERIFY(message && message->isVisible());
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centreOf(message));
        // ...but Send Message leaves it in Jessica's composer, not back there.
        QTRY_VERIFY(!controller.profiles()->isOpen());
        QTRY_COMPARE(controller.currentContactId(), QStringLiteral("jessica"));
        QTRY_VERIFY(input->hasActiveFocus());
        QTRY_VERIFY(!profileLoaderActive(window));
        QVERIFY(input->hasActiveFocus());
        QVERIFY(!search->hasActiveFocus());
    }

    void ctrlIOpensTheSelectedChatsProfile()
    {
        failOnQmlWarnings();
        OpenChat::ChatController controller;
        QQmlApplicationEngine engine;
        QQuickWindow *window = showMain(engine, {{QStringLiteral("chatController"),
                                                  QVariant::fromValue(&controller)}});
        QVERIFY(window);
        QVERIFY(controller.selectContact(QStringLiteral("ryan")));
        QTest::keyClick(window, Qt::Key_I, Qt::ControlModifier);
        QTRY_VERIFY(controller.profiles()->isOpen());
        QCOMPARE(controller.profiles()->personId(), QStringLiteral("ryan"));
        // Pressed again on the page it does not stack another copy.
        QTest::keyClick(window, Qt::Key_I, Qt::ControlModifier);
        QTest::qWait(100);
        QCOMPARE(controller.profiles()->depth(), 1);
    }

    void viewProfileContextMenu()
    {
        failOnQmlWarnings();
        OpenChat::ChatController controller;
        QQmlApplicationEngine engine;
        QQuickWindow *window = showMain(engine, {{QStringLiteral("chatController"),
                                                  QVariant::fromValue(&controller)}});
        QVERIFY(window);
        auto *category = window->findChild<QQuickItem *>(QStringLiteral("contactsCategory"));
        auto *row = category ? findVisualItem(category, QStringLiteral("contactRow_jessica")) : nullptr;
        QVERIFY(row);
        const QString openChat = controller.currentContactId();
        QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier,
                          row->mapToScene(QPointF(row->width() - 40, row->height() / 2)).toPoint());
        QQuickItem *item = nullptr;
        QTRY_VERIFY((item = findVisualItem(window->contentItem(), QStringLiteral("viewProfileMenuItem")))
                    && item->isVisible());
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centreOf(item));
        QTRY_VERIFY(controller.profiles()->isOpen());
        QCOMPARE(controller.profiles()->personId(), QStringLiteral("jessica"));
        // A right click neither selects the chat nor opens it.
        QCOMPARE(controller.currentContactId(), openChat);
    }

    void plainProfilesSettingSwitch()
    {
        failOnQmlWarnings();
        OpenChat::ChatController controller;
        QQmlApplicationEngine engine;
        QQuickWindow *window = showMain(engine, {{QStringLiteral("chatController"),
                                                  QVariant::fromValue(&controller)}});
        QVERIFY(window);
        auto *appearance = engine.singletonInstance<OpenChat::AppearanceSettings *>(
            QStringLiteral("OpenChat.Native"), QStringLiteral("AppearanceSettings"));
        QVERIFY(appearance);
        appearance->setPlainProfiles(false);
        OpenChat::ProfileController *profiles = controller.profiles();
        QVERIFY(profiles->openContact(QStringLiteral("jessica")));
        QTRY_VERIFY(settledProfilePage(window));
        QCOMPARE(profiles->pageState(), int(OpenChat::Profile::PageState::CustomPage));
        QVERIFY(!profiles->plainStyle());
        // The top bar's switch writes the setting, and every page follows it.
        auto *toggle = findVisualItem(window->contentItem(), QStringLiteral("profilePlainStyleSwitch"));
        QVERIFY(toggle && toggle->isVisible());
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centreOf(toggle));
        QTRY_VERIFY(appearance->plainProfiles());
        QTRY_VERIFY(profiles->plainStyle());
        captureProfileShot(window, QStringLiteral("in-context-plain-style"));
        // And the setting (Settings › Appearance) drives the page back.
        appearance->setPlainProfiles(false);
        QTRY_VERIFY(!profiles->plainStyle());
    }

    void profileFitsMinimumWindow()
    {
        failOnQmlWarnings();
        OpenChat::ChatController controller;
        QQmlApplicationEngine engine;
        QQuickWindow *window = showMain(engine, {{QStringLiteral("chatController"),
                                                  QVariant::fromValue(&controller)}});
        QVERIFY(window);
        window->resize(window->minimumSize());
        QTRY_COMPARE(window->width(), 720);
        QVERIFY(controller.profiles()->openContact(QStringLiteral("sarah")));
        QQuickItem *page = nullptr;
        QTRY_VERIFY((page = settledProfilePage(window)));
        QCOMPARE(page->width(), qreal(window->width()));
        auto *flickable = findVisualItem(page, QStringLiteral("profilePageFlickable"));
        QVERIFY(flickable);
        // Nothing sideways: the page scrolls down only.
        QVERIFY(flickable->property("contentWidth").toReal() <= flickable->width() + 0.5);
        auto *wide = findVisualItem(page, QStringLiteral("profileWideColumn"));
        auto *narrow = findVisualItem(page, QStringLiteral("profileNarrowColumn"));
        QVERIFY(wide && narrow);
        // Two columns hold down to the minimum window (SPEC §3.1).
        QVERIFY(wide->isVisible() && narrow->isVisible());
        QVERIFY(wide->mapToScene(QPointF(wide->width(), 0)).x() <= window->width());
        captureProfileShot(window, QStringLiteral("in-context-minimum"));
    }
};

// One registration list for every suite that hosts the app's QML
// (tests/ProfileQmlHarness.h), so they never drift apart.
OPENCHAT_PROFILE_QML_TEST_MAIN(QmlLoadTest, "qml-appearance")

#include "tst_qmlload.moc"
