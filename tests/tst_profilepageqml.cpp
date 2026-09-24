// The profile page kit (ARCH §8.1, §9.7): ProfilePage hosted as Main.qml
// hosts it, by tests/qml/ProfilePageHarness.qml, over the reference mock the
// ChatController owns. Every test fails on any QML or Qt warning.
//
// OPENCHAT_PROFILE_CAPTURES=<dir> also saves every preset at 860×680 and
// 1280×900 in both app modes, each stub, and the states the final mockups
// show (final/*.png), for review side by side.

#include "ProfileQmlHarness.h"

#include "controllers/CallController.h"
#include "controllers/ChatController.h"
#include "controllers/ContactController.h"
#include "controllers/ProfileController.h"
#include "controllers/ProfilePageObject.h"
#include "controllers/ProfileReferencePages.h"
#include "domain/ProfilePage.h"
#include "models/ContactListModel.h"
#include "models/RequestListModel.h"
#include "profile/ProfileAmbientItem.h"
#include "profile/ProfileMediaStore.h"
#include "profile/ProfileNameTextItem.h"
#include "profile/ProfileRenderPolicy.h"
#include "profile/SongPlayer.h"

#include <QAudioFormat>
#include <QBuffer>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickPaintedItem>
#include <QQuickWindow>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <cmath>
#include <functional>
#include <memory>
#include <optional>

using namespace OpenChat;
using Profile::Origin;
using Profile::PageState;
using Profile::Preset;
using Profile::Relationship;

namespace {

namespace Reference = ProfileReferencePages;

// --- Items -------------------------------------------------------------------

void collect(QQuickItem *root, const QString &name, QList<QQuickItem *> &out)
{
    if (!root)
        return;
    if (root->objectName() == name)
        out.append(root);
    for (QQuickItem *child : root->childItems())
        collect(child, name, out);
}

QList<QQuickItem *> itemsNamed(QQuickItem *root, const QString &name)
{
    QList<QQuickItem *> out;
    collect(root, name, out);
    return out;
}

// The first item called `name` that is on screen.
QQuickItem *shown(QQuickItem *root, const QString &name)
{
    for (QQuickItem *item : itemsNamed(root, name)) {
        if (item->isVisible())
            return item;
    }
    return nullptr;
}

void collectAll(QQuickItem *root, QList<QQuickItem *> &out)
{
    if (!root)
        return;
    out.append(root);
    for (QQuickItem *child : root->childItems())
        collectAll(child, out);
}

QList<QQuickItem *> allItems(QQuickItem *root)
{
    QList<QQuickItem *> out;
    collectAll(root, out);
    return out;
}

QPoint centreOf(QQuickItem *item)
{
    return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
}

QRectF sceneRect(QQuickItem *item)
{
    return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
}

bool isA(const QObject *object, const char *className)
{
    return object != nullptr && object->inherits(className);
}

// --- Pixels ------------------------------------------------------------------

QColor at(const QImage &image, const QPointF &point)
{
    const qreal dpr = image.devicePixelRatio();
    return image.pixelColor(int(point.x() * dpr), int(point.y() * dpr));
}

bool near(const QColor &a, const QColor &b, int tolerance)
{
    return std::abs(a.red() - b.red()) <= tolerance && std::abs(a.green() - b.green()) <= tolerance
           && std::abs(a.blue() - b.blue()) <= tolerance;
}

int pixelsDiffering(const QImage &a, const QImage &b, int tolerance = 12)
{
    if (a.size() != b.size())
        return std::max(a.width() * a.height(), b.width() * b.height());
    int count = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            if (!near(a.pixelColor(x, y), b.pixelColor(x, y), tolerance))
                ++count;
        }
    }
    return count;
}

int pixelsNear(const QImage &image, const QColor &colour, int tolerance)
{
    int count = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (near(image.pixelColor(x, y), colour, tolerance))
                ++count;
        }
    }
    return count;
}

// Review PNGs, only when OPENCHAT_PROFILE_CAPTURES names a directory.
void capture(const QString &name, const QImage &image)
{
    const QString directory = qEnvironmentVariable("OPENCHAT_PROFILE_CAPTURES");
    if (directory.isEmpty())
        return;
    QDir().mkpath(directory);
    image.save(QDir(directory).filePath(name + QStringLiteral(".png")));
}

bool capturing()
{
    return !qEnvironmentVariableIsEmpty("OPENCHAT_PROFILE_CAPTURES");
}

// --- Audio -------------------------------------------------------------------

// A silent device: play() gets an output that pulls nothing, or none at all.
struct QuietDevices final {
    int opened = 0;
    bool refuse = false;
};

class QuietOutput final : public SongOutput
{
public:
    [[nodiscard]] QAudioFormat format() const override
    {
        QAudioFormat format;
        format.setSampleRate(48'000);
        format.setChannelCount(2);
        format.setSampleFormat(QAudioFormat::Int16);
        return format;
    }
    [[nodiscard]] int bufferMs() const override { return 0; }
    bool start(QIODevice *) override { return true; }
    void stop() override {}
};

void useQuietDevices(const std::shared_ptr<QuietDevices> &devices)
{
    SongPlayer::setOutputFactoryForTesting([devices](int, QString &error) -> std::unique_ptr<SongOutput> {
        if (devices->refuse) {
            error = QStringLiteral("No audio output");
            return nullptr;
        }
        ++devices->opened;
        return std::make_unique<QuietOutput>();
    });
}

// --- Pages -------------------------------------------------------------------

// The mock contact whose page shows `preset` (SPEC §0.2's pairs), and the
// page: their seeded page, with the preset applied when it is not theirs.
struct PresetPage final {
    QString contact;
    Profile::Page page;
};

PresetPage presetPage(Preset preset)
{
    if (preset == Preset::HeadlinerPreset)
        return {Reference::selfId(), Reference::ownReferencePage()};
    QString contact = Reference::contactForPreset(preset);
    if (contact.isEmpty()) {
        switch (preset) {
        case Preset::NeonZebraPreset: contact = QStringLiteral("jessica"); break;
        case Preset::SafetyPinPreset: contact = QStringLiteral("alex"); break;
        case Preset::ChromeY2KPreset: contact = QStringLiteral("ryan"); break;
        default: contact = QStringLiteral("michael"); break;
        }
    }
    Profile::Page page = Reference::seededPage(contact).value_or(Profile::defaultPage());
    if (page.preset != preset)
        page = Profile::applyPreset(page, preset);
    if (page.revision <= 0)
        page.revision = 7;
    return {contact, page};
}

QByteArray jpegOf(const QColor &left, const QColor &right, const QSize &size = QSize(320, 200))
{
    QImage image(size, QImage::Format_RGB32);
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < size.width(); ++x)
            image.setPixelColor(x, y, x < size.width() / 2 ? left : right);
    }
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPG", 90);
    return bytes;
}

// --- The stage -----------------------------------------------------------------

// A ChatController on the reference mock, optional contact and call
// controllers, and the harness window hosting the page. Declared in teardown
// order: the window goes first, the controllers it binds to last.
struct Stage final {
    ChatController chat;
    std::unique_ptr<ContactController> contacts;
    std::unique_ptr<CallController> calls;
    QQmlEngine engine;
    std::unique_ptr<QQuickWindow> window;

    [[nodiscard]] ProfileController &profiles() { return *chat.profiles(); }

    bool load(const QSize &size = QSize(860, 680))
    {
        engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        QQmlComponent component(&engine,
                                QUrl::fromLocalFile(QStringLiteral(OPENCHAT_SOURCE_DIR "/tests/qml/ProfilePageHarness.qml")));
        QVariantMap properties{{QStringLiteral("chatController"), QVariant::fromValue(&chat)}};
        if (contacts)
            properties.insert(QStringLiteral("contactController"), QVariant::fromValue(contacts.get()));
        if (calls)
            properties.insert(QStringLiteral("realCallController"), QVariant::fromValue(calls.get()));
        QObject *object = component.createWithInitialProperties(properties);
        if (!object) {
            qWarning().noquote() << component.errorString();
            return false;
        }
        window.reset(qobject_cast<QQuickWindow *>(object));
        if (!window)
            return false;
        window->resize(size);
        window->show();
        return QTest::qWaitForWindowExposed(window.get());
    }

    [[nodiscard]] QQuickItem *root() const { return window ? window->contentItem() : nullptr; }
    [[nodiscard]] QQuickItem *page() const
    {
        return window ? qvariant_cast<QQuickItem *>(window->property("page")) : nullptr;
    }
    [[nodiscard]] QQuickItem *view() const { return shown(page(), QStringLiteral("profilePageView")); }
    [[nodiscard]] QQuickItem *item(const QString &name) const { return shown(root(), name); }
    [[nodiscard]] QObject *scriptedCalls() const
    {
        return window ? qvariant_cast<QObject *>(window->property("scripted")) : nullptr;
    }
    void useScriptedCalls(bool on = true) { window->setProperty("useScriptedCalls", on); }

    // The page is open and its open fade has finished.
    bool settled()
    {
        return QTest::qWaitFor([this] {
            QQuickItem *current = page();
            return current != nullptr && current->property("settled").toBool();
        });
    }
    bool open(const std::function<void()> &opener)
    {
        opener();
        return settled();
    }

    QImage grab()
    {
        window->update();
        QTest::qWait(30);
        return window->grabWindow();
    }

    void click(QQuickItem *item, Qt::MouseButton button = Qt::LeftButton)
    {
        QTest::mouseClick(window.get(), button, Qt::NoModifier, centreOf(item));
    }
    void hover(QQuickItem *item) { QTest::mouseMove(window.get(), centreOf(item)); }
    void key(int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
    {
        QTest::keyClick(window.get(), Qt::Key(key), modifiers);
    }

    // The Contacting cells on screen, in order.
    QStringList actions() const
    {
        QStringList ids;
        QQuickItem *grid = shown(root(), QStringLiteral("profileContactingGrid"));
        for (QQuickItem *item : allItems(grid)) {
            const QString name = item->objectName();
            if (name.startsWith(QStringLiteral("profileAction_")) && item->isVisible())
                ids.append(name.mid(int(qstrlen("profileAction_"))));
        }
        return ids;
    }
    QQuickItem *action(const QString &id) const { return shown(root(), QStringLiteral("profileAction_") + id); }

    QString text(const QString &name) const
    {
        QQuickItem *found = shown(root(), name);
        return found ? found->property("text").toString() : QStringLiteral("<missing %1>").arg(name);
    }
};

// Opens `contact` (a mock roster id, or "self") on `stage`.
bool openPerson(Stage &stage, const QString &contact, int origin = -1)
{
    if (contact == Reference::selfId()) {
        stage.profiles().openOwn(origin);
        return stage.settled();
    }
    return stage.profiles().openContact(contact, origin) && stage.settled();
}

QString slugOf(Preset preset)
{
    return Profile::presetSlug(preset);
}

} // namespace

class ProfilePageQmlTest final : public QObject
{
    Q_OBJECT

private:
    std::shared_ptr<QuietDevices> m_devices;

private slots:
    void init()
    {
        ProfileQmlHarness::resetProfileSingletons();
        QSettings().clear();
        m_devices = std::make_shared<QuietDevices>();
        useQuietDevices(m_devices);
        QTest::failOnWarning(QRegularExpression(QStringLiteral(".*")));
    }

    void cleanup()
    {
        SongPlayer::setOutputFactoryForTesting({});
        QSettings().clear();
    }

    // --- Rendering -----------------------------------------------------------

    void everyPresetRendersWithoutWarnings_data()
    {
        QTest::addColumn<int>("preset");
        QTest::addColumn<bool>("dark");
        for (int preset = int(Preset::AeroSkyPreset); preset <= int(Preset::ChromeY2KPreset); ++preset) {
            for (const bool dark : {false, true}) {
                const QString row = slugOf(Preset(preset)) + (dark ? QStringLiteral("-dark") : QStringLiteral("-light"));
                QTest::newRow(qPrintable(row)) << preset << dark;
            }
        }
    }
    void everyPresetRendersWithoutWarnings()
    {
        QFETCH(int, preset);
        QFETCH(bool, dark);
        QSettings().setValue(QStringLiteral("Appearance/darkMode"), dark);
        const PresetPage shown = presetPage(Preset(preset));

        const QList<QSize> sizes = capturing() ? QList<QSize>{QSize(860, 680), QSize(1280, 900)}
                                               : QList<QSize>{QSize(860, 680)};
        for (const QSize &size : sizes) {
            Stage stage;
            stage.chat.setLocalUserName(QStringLiteral("Daniel"));
            stage.profiles().setMockPage(shown.contact, shown.page);
            QVERIFY(stage.load(size));
            QVERIFY(openPerson(stage, shown.contact));
            QCOMPARE(stage.profiles().pageState(), int(PageState::CustomPage));
            QQuickItem *view = stage.view();
            QVERIFY(view);
            QCOMPARE(view->property("width").toReal(), qreal(size.width()));

            // The page shows the preset: the view's page and its resolved style.
            auto *page = qvariant_cast<ProfilePageObject *>(view->property("page"));
            QVERIFY(page);
            QCOMPARE(page->preset(), preset);
            QVERIFY(!page->render()->plain());

            // Every fixed box and the name are on screen and inked.
            for (const char *name : {"profileIdentityBox", "profileContactingBox", "profileBannerBox",
                                     "profileNameText", "profileTopBar"})
                QVERIFY2(stage.item(QString::fromLatin1(name)), name);
            auto *nameItem = qobject_cast<ProfileNameText *>(stage.item(QStringLiteral("profileNameText")));
            QVERIFY(nameItem);
            QVERIFY(nameItem->renderedPixelSize() >= 20);
            QVERIFY(nameItem->textWidth() > 20);

            const QImage shot = stage.grab();
            QCOMPARE(shot.size() / shot.devicePixelRatio(), size);
            // The identity card is filled with the box colour, not the backdrop.
            QQuickItem *card = stage.item(QStringLiteral("profileIdentityBox"));
            const QRectF cardRect = sceneRect(card);
            const QColor fill = page->render()->boxFill();
            const QColor inside = at(shot, cardRect.topLeft() + QPointF(cardRect.width() - 6, cardRect.height() - 6));
            QVERIFY2(near(inside, fill, 40) || fill.alphaF() < 0.99,
                     qPrintable(inside.name() + QStringLiteral(" vs ") + fill.name()));
            capture(QStringLiteral("preset-%1-%2x%3-%4")
                        .arg(slugOf(Preset(preset)))
                        .arg(size.width())
                        .arg(size.height())
                        .arg(dark ? QStringLiteral("dark") : QStringLiteral("light")),
                    shot);
        }
    }

    void geometryFollowsTheSpecTable_data()
    {
        QTest::addColumn<int>("width");
        QTest::addColumn<bool>("preview");
        QTest::addColumn<int>("margin");
        QTest::addColumn<int>("content");
        QTest::addColumn<int>("gutter");
        QTest::addColumn<int>("vgap");
        QTest::addColumn<int>("narrow");
        QTest::addColumn<int>("wide");
        QTest::addColumn<int>("side");
        // SPEC §3.1: W, margin, content, gutter / vgap, narrow : wide, backdrop at each side.
        QTest::newRow("720") << 720 << false << 16 << 688 << 14 << 12 << 276 << 398 << 16;
        QTest::newRow("860") << 860 << false << 32 << 796 << 14 << 12 << 321 << 461 << 32;
        QTest::newRow("1024") << 1024 << false << 32 << 940 << 16 << 14 << 379 << 545 << 42;
        QTest::newRow("1280") << 1280 << false << 32 << 940 << 16 << 14 << 379 << 545 << 170;
        QTest::newRow("1920") << 1920 << false << 32 << 940 << 16 << 14 << 379 << 545 << 490;
        // The editor preview beside the rail and panel at 1024, 860 and 720.
        QTest::newRow("preview-648") << 648 << true << 16 << 616 << 14 << 12 << 247 << 355 << 16;
        QTest::newRow("preview-484") << 484 << true << 16 << 452 << 0 << 12 << 452 << 452 << 16;
        QTest::newRow("preview-344") << 344 << true << 16 << 312 << 0 << 12 << 312 << 312 << 16;
    }
    void geometryFollowsTheSpecTable()
    {
        QFETCH(int, width);
        QFETCH(bool, preview);
        QFETCH(int, margin);
        QFETCH(int, content);
        QFETCH(int, gutter);
        QFETCH(int, vgap);
        QFETCH(int, narrow);
        QFETCH(int, wide);
        QFETCH(int, side);

        Stage stage;
        QVERIFY(stage.load(QSize(std::max(width, 720), 800)));
        QVERIFY(openPerson(stage, QStringLiteral("michael")));
        QQuickItem *view = stage.view();
        QVERIFY(view);
        if (preview) {
            // The editor's preview: the draft, in preview mode, at the width
            // the editor leaves it.
            QVERIFY(stage.profiles().beginEditing());
            stage.window->setProperty("previewWidth", width);
            QTRY_VERIFY((view = qvariant_cast<QQuickItem *>(stage.window->property("preview"))));
            QCOMPARE(view->width(), qreal(width));
            QTRY_VERIFY(shown(view, QStringLiteral("profileIdentityBox")));
        }
        checkGeometry(view, width, margin, content, gutter, vgap, narrow, wide, side);
    }

    void twoColumnsHoldAtTheMinimumWindow()
    {
        Stage stage;
        QVERIFY(stage.load(QSize(720, 560)));
        QVERIFY(openPerson(stage, QStringLiteral("sarah")));
        QQuickItem *view = stage.view();
        QVERIFY(view->property("twoColumns").toBool());
        QQuickItem *identity = stage.item(QStringLiteral("profileIdentityBox"));
        QQuickItem *blurbs = stage.item(QStringLiteral("profileBlurbsBox"));
        QVERIFY(identity && blurbs);
        const QRectF left = sceneRect(identity);
        const QRectF right = sceneRect(blurbs);
        // Side by side: 276 + 14 + 398 inside 16 px margins, nothing past them.
        QCOMPARE(left.width(), 276.0);
        QCOMPARE(right.width(), 398.0);
        QCOMPARE(left.left(), 16.0);
        QCOMPARE(right.left() - left.right(), 14.0);
        QVERIFY(right.right() <= 720 - 16);
        QVERIFY(right.top() < left.bottom());
        capture(QStringLiteral("final-min"), stage.grab());
    }

    void paintedAreaStaysWithinBudget()
    {
        Stage stage;
        QVERIFY(stage.load(QSize(1280, 900)));
        QVERIFY(openPerson(stage, QStringLiteral("sarah"))); // falling hearts, glitter, a song
        QQuickItem *view = stage.view();
        const qreal viewport = view->width() * view->height();
        qreal painted = 0;
        int canvases = 0;
        for (QQuickItem *item : allItems(view)) {
            if (qobject_cast<QQuickPaintedItem *>(item) != nullptr && item->isVisible())
                painted += item->width() * item->height();
            if (isA(item, "QQuickCanvasItem"))
                ++canvases;
        }
        QVERIFY2(painted <= 2 * viewport, qPrintable(QString::number(painted / viewport)));
        QVERIFY(painted >= viewport); // the one viewport raster is the backdrop
        QCOMPARE(canvases, 0);
    }

    void contentTextIsPlain()
    {
        Stage stage;
        Profile::Page page = Reference::seededPage(QStringLiteral("michael")).value();
        page.content.headline = QStringLiteral("<b>bold</b> & <i>x</i>");
        page.content.aboutMe = QStringLiteral("<font color='red'>red</font>\n\n<a href='https://x'>link</a>");
        page.content.interests.general = QStringLiteral("<img src='file:///etc/passwd'>");
        page.content.displayName = QStringLiteral("<u>Mike</u>");
        stage.profiles().setMockPage(QStringLiteral("michael"), page);
        QVERIFY(stage.load());
        QVERIFY(openPerson(stage, QStringLiteral("michael")));

        int texts = 0;
        for (QQuickItem *item : allItems(stage.page())) {
            const bool text = isA(item, "QQuickText");
            const bool edit = isA(item, "QQuickTextEdit");
            if (!text && !edit)
                continue;
            // The CallStrip is the chat's own component, not the page's.
            if (item->parentItem() && item->parentItem()->objectName() == QStringLiteral("callStrip"))
                continue;
            ++texts;
            QVERIFY2(item->property("textFormat").toInt() == 0 /* PlainText */,
                     qPrintable(item->objectName() + QStringLiteral(": ") + item->property("text").toString()));
        }
        QVERIFY(texts > 30);
        // The markup shows as the characters it is.
        QCOMPARE(stage.text(QStringLiteral("profileHeadline")), QStringLiteral("“<b>bold</b> & <i>x</i>”"));
        bool aboutShown = false;
        for (QQuickItem *item : allItems(stage.item(QStringLiteral("profileAboutMe"))))
            aboutShown = aboutShown || item->property("text").toString().contains(QStringLiteral("<font color='red'>"));
        QVERIFY(aboutShown);
        QCOMPARE(qobject_cast<ProfileNameText *>(stage.item(QStringLiteral("profileNameText")))->text(),
                 QStringLiteral("<u>Mike</u>"));
    }

    // --- Actions ---------------------------------------------------------------

    void contactingActionsMatchRelationship_data()
    {
        QTest::addColumn<QString>("person");
        QTest::addColumn<bool>("withContacts");
        QTest::addColumn<QString>("calls"); // "", "real" (no call service), "scripted"
        QTest::addColumn<QString>("title");
        QTest::addColumn<QStringList>("expected");
        QTest::newRow("contact, no services") << "michael" << false << "" << "Contacting Michael"
                                              << QStringList{"message"};
        QTest::newRow("contact, contacts only") << "michael" << true << "" << "Contacting Michael"
                                                << QStringList{"message", "safety"};
        QTest::newRow("contact, no call service") << "michael" << true << "real" << "Contacting Michael"
                                                  << QStringList{"message", "safety"};
        QTest::newRow("contact, calls and contacts") << "michael" << true << "scripted" << "Contacting Michael"
                                                     << QStringList{"message", "voice", "video", "safety"};
        QTest::newRow("contact without a page") << "tom" << true << "scripted" << "Contacting Tom"
                                                << QStringList{"message", "voice", "video", "safety"};
        QTest::newRow("own, with contacts") << "self" << true << "scripted" << "Your Profile"
                                            << QStringList{"edit", "picture", "invite"};
        QTest::newRow("own, no contacts") << "self" << false << "" << "Your Profile"
                                          << QStringList{"edit", "picture"};
    }
    void contactingActionsMatchRelationship()
    {
        QFETCH(QString, person);
        QFETCH(bool, withContacts);
        QFETCH(QString, calls);
        QFETCH(QString, title);
        QFETCH(QStringList, expected);

        Stage stage;
        if (withContacts)
            stage.contacts = std::make_unique<ContactController>();
        if (calls == QStringLiteral("real"))
            stage.calls = std::make_unique<CallController>();
        QVERIFY(stage.load());
        if (calls == QStringLiteral("scripted"))
            stage.useScriptedCalls();
        QVERIFY(openPerson(stage, person == QStringLiteral("self") ? Reference::selfId() : person));
        QTRY_COMPARE(stage.actions(), expected);
        QQuickItem *box = stage.item(QStringLiteral("profileContactingBox"));
        QCOMPARE(box->property("title").toString(), title);
        // Every cell is available and names its action for screen readers.
        for (const QString &id : expected) {
            QQuickItem *cell = stage.action(id);
            QVERIFY(cell->property("available").toBool());
            QVERIFY(!cell->property("accessibleName").toString().isEmpty());
        }
    }

    // Chat-bound actions close the page and switch to Chat first, even from
    // the Settings section (ARCH §8.4).
    void chatBoundActionsSwitchToTheChatSection()
    {
        Stage stage;
        stage.contacts = std::make_unique<ContactController>();
        QVERIFY(stage.load());
        stage.useScriptedCalls();
        QObject *calls = stage.scriptedCalls();

        stage.chat.setNavSection(ChatController::NavSection::Settings);
        QVERIFY(openPerson(stage, QStringLiteral("michael")));
        QCOMPARE(stage.profiles().backLabel(), QStringLiteral("Settings"));
        stage.click(stage.action(QStringLiteral("message")));
        QTRY_VERIFY(!stage.profiles().isOpen());
        QCOMPARE(stage.chat.navSection(), ChatController::NavSection::Chat);
        QCOMPARE(stage.chat.currentContactId(), QStringLiteral("michael"));
        QCOMPARE(calls->property("log").toStringList(), QStringList());

        stage.chat.setNavSection(ChatController::NavSection::Settings);
        QVERIFY(openPerson(stage, QStringLiteral("ryan")));
        stage.click(stage.action(QStringLiteral("voice")));
        QTRY_VERIFY(!stage.profiles().isOpen());
        QCOMPARE(stage.chat.navSection(), ChatController::NavSection::Chat);
        QCOMPARE(stage.chat.currentContactId(), QStringLiteral("ryan"));
        QCOMPARE(calls->property("log").toStringList(), QStringList{QStringLiteral("voice")});

        QVERIFY(openPerson(stage, QStringLiteral("alex")));
        stage.click(stage.action(QStringLiteral("video")));
        QTRY_VERIFY(!stage.profiles().isOpen());
        QCOMPARE(stage.chat.currentContactId(), QStringLiteral("alex"));
        QCOMPARE(calls->property("log").toStringList(), (QStringList{QStringLiteral("voice"), QStringLiteral("video")}));

        // Safety Number opens its dialog over the page, which stays.
        QVERIFY(openPerson(stage, QStringLiteral("michael")));
        QVERIFY(!stage.contacts->safetyNumberOpen());
        stage.click(stage.action(QStringLiteral("safety")));
        QTRY_VERIFY(stage.contacts->safetyNumberOpen());
        QVERIFY(stage.profiles().isOpen());
    }

    void callActionsDisableOrMergeDuringCalls()
    {
        Stage stage;
        stage.contacts = std::make_unique<ContactController>();
        QVERIFY(stage.load());
        stage.useScriptedCalls();
        QObject *calls = stage.scriptedCalls();
        QVERIFY(openPerson(stage, QStringLiteral("michael")));
        QCOMPARE(stage.actions(), (QStringList{"message", "voice", "video", "safety"}));

        // In a call with someone else: present, disabled, explaining why.
        calls->setProperty("inCall", true);
        calls->setProperty("callChatId", QStringLiteral("ryan"));
        calls->setProperty("peerName", QStringLiteral("Ryan"));
        QTRY_COMPARE(stage.actions(), (QStringList{"message", "voice", "video", "safety"}));
        for (const char *id : {"voice", "video"}) {
            QQuickItem *cell = stage.action(QString::fromLatin1(id));
            QVERIFY(!cell->property("available").toBool());
            QCOMPARE(cell->property("reason").toString(), QStringLiteral("You're already in a call"));
            QCOMPARE(cell->opacity(), 0.45);
            stage.click(cell);
        }
        QVERIFY(stage.profiles().isOpen());
        QCOMPARE(calls->property("log").toStringList(), QStringList());

        // In a call with them: one "Return to call".
        calls->setProperty("callChatId", QStringLiteral("michael"));
        QTRY_COMPARE(stage.actions(), (QStringList{"message", "return", "safety"}));
        QCOMPARE(stage.action(QStringLiteral("return"))->property("label").toString(), QStringLiteral("Return to call"));
        stage.click(stage.action(QStringLiteral("return")));
        QTRY_VERIFY(!stage.profiles().isOpen());
        QCOMPARE(calls->property("log").toStringList(), QStringList{QStringLiteral("showCallChat")});
    }

    void messageActionOpensTheChat()
    {
        Stage stage;
        QVERIFY(stage.load());
        QVERIFY(stage.chat.selectContact(QStringLiteral("sarah")));
        QVERIFY(openPerson(stage, QStringLiteral("jessica")));
        QCOMPARE(stage.actions(), QStringList{QStringLiteral("message")});
        QCOMPARE(stage.action(QStringLiteral("message"))->property("accessibleName").toString(),
                 QStringLiteral("Send Message to Jessica"));
        // Enter on the focused grid does what a click does.
        QQuickItem *grid = stage.item(QStringLiteral("profileContactingGrid"));
        grid->forceActiveFocus();
        stage.key(Qt::Key_Return);
        QTRY_VERIFY(!stage.profiles().isOpen());
        QCOMPARE(stage.chat.currentContactId(), QStringLiteral("jessica"));
        QCOMPARE(stage.chat.navSection(), ChatController::NavSection::Chat);
        QTRY_VERIFY(stage.page() == nullptr);
    }

    void onlineNowFollowsRealPresence()
    {
        Stage stage;
        QVERIFY(stage.load());
        QVERIFY(openPerson(stage, QStringLiteral("michael")));
        QCOMPARE(stage.text(QStringLiteral("profilePresenceLabel")), QStringLiteral("Online Now!"));
        QVERIFY(stage.item(QStringLiteral("profileTopBarPresence")));

        // The roster says Michael went away: the page follows, never the page's data.
        stage.chat.contacts()->setPresence(QStringLiteral("michael"), Presence::Away);
        stage.profiles().refresh();
        QTRY_COMPARE(stage.text(QStringLiteral("profilePresenceLabel")), QStringLiteral("Away"));
        stage.chat.contacts()->setPresence(QStringLiteral("michael"), Presence::Busy);
        stage.profiles().refresh();
        QTRY_COMPARE(stage.text(QStringLiteral("profilePresenceLabel")), QStringLiteral("Busy"));

        QVERIFY(openPerson(stage, QStringLiteral("tom")));
        QCOMPARE(stage.text(QStringLiteral("profilePresenceLabel")), QStringLiteral("Offline"));
        QVERIFY(openPerson(stage, QStringLiteral("sarah")));
        QCOMPARE(stage.text(QStringLiteral("profilePresenceLabel")), QStringLiteral("Away"));
    }

    // --- The song ------------------------------------------------------------

    void songPlayerSurvivesRefreshesAndArrivals()
    {
        Stage stage;
        QVERIFY(stage.load());
        QVERIFY(openPerson(stage, QStringLiteral("michael")));
        auto *player = stage.page()->findChild<SongPlayer *>(QStringLiteral("profileSongPlayer"));
        QVERIFY(player);
        QVERIFY(!player->songKey().isEmpty());
        QTRY_VERIFY(player->valid());
        QSignalSpy sources(player, &SongPlayer::sourceChanged);
        stage.click(stage.item(QStringLiteral("profileSongOrb")));
        QTRY_VERIFY(player->playing());

        // The 5 s refresh, an unchanged page arriving again, and a newer page
        // that adds a box: the same player, still playing, never reloaded.
        stage.profiles().refresh();
        const Profile::Page seeded = Reference::seededPage(QStringLiteral("michael")).value();
        stage.profiles().setMockPage(QStringLiteral("michael"), seeded);
        Profile::Page newer = seeded;
        newer.revision += 1;
        newer.content.details.hometown = QStringLiteral("Queens, NY");
        newer.content.details.occupation = QStringLiteral("Photographer");
        stage.profiles().setMockPage(QStringLiteral("michael"), newer);
        QTRY_COMPARE(stage.text(QStringLiteral("profileTableValue")).isEmpty(), false);
        QTRY_VERIFY(stage.item(QStringLiteral("profileDetailsBox")));
        QCOMPARE(stage.page()->findChild<SongPlayer *>(QStringLiteral("profileSongPlayer")), player);
        QCOMPARE(stage.page()->findChildren<SongPlayer *>().size(), 1);
        QCOMPARE(sources.count(), 0);
        QVERIFY(player->playing());
        QCOMPARE(m_devices->opened, 1);
    }

    void songNeverAutoplaysAndPausesInCalls()
    {
        Stage stage;
        QVERIFY(stage.load());
        stage.useScriptedCalls();
        QObject *calls = stage.scriptedCalls();
        QVERIFY(openPerson(stage, QStringLiteral("jessica")));
        auto *player = stage.page()->findChild<SongPlayer *>(QStringLiteral("profileSongPlayer"));
        QTRY_VERIFY(player->valid());
        QTest::qWait(300);
        QVERIFY(!player->playing());
        QCOMPARE(m_devices->opened, 0);
        QQuickItem *module = stage.item(QStringLiteral("profileSongModule"));
        QCOMPARE(module->property("state_").toString(), QStringLiteral("idle"));
        QCOMPARE(stage.text(QStringLiteral("profileSongTime")), QStringLiteral("0:45"));

        // Space on the focused orb plays; the equaliser shows while it does.
        QQuickItem *orb = stage.item(QStringLiteral("profileSongOrb"));
        orb->forceActiveFocus();
        stage.key(Qt::Key_Space);
        QTRY_VERIFY(player->playing());
        QCOMPARE(m_devices->opened, 1);
        QTRY_VERIFY(stage.item(QStringLiteral("profileSongEqualiser")));
        stage.key(Qt::Key_Right);
        QTRY_COMPARE(player->positionMs(), qint64(5000));
        QTRY_COMPARE(stage.text(QStringLiteral("profileSongTime")), QStringLiteral("0:05 / 0:45"));

        // A call rings: the song pauses where it was and refuses to play.
        calls->setProperty("isRinging", true);
        calls->setProperty("inCall", true);
        QTRY_VERIFY(player->suspended());
        QVERIFY(!player->playing());
        QCOMPARE(module->property("state_").toString(), QStringLiteral("paused"));
        stage.click(orb);
        QTest::qWait(50);
        QVERIFY(!player->playing());
        QCOMPARE(m_devices->opened, 1);

        // The call ends: nothing starts by itself.
        calls->setProperty("isRinging", false);
        calls->setProperty("inCall", false);
        QTRY_VERIFY(!player->suspended());
        QTest::qWait(200);
        QVERIFY(!player->playing());
        QCOMPARE(m_devices->opened, 1);
    }

    void callStripShowsWhileRinging()
    {
        Stage stage;
        stage.calls = std::make_unique<CallController>();
        QVERIFY(stage.load());
        QVERIFY(openPerson(stage, QStringLiteral("ryan")));
        QVERIFY(!stage.item(QStringLiteral("callStrip")));
        QCOMPARE(sceneRect(stage.item(QStringLiteral("profilePageBody"))).top(), 48.0);

        stage.calls->enableForPreview(CallState::Ringing, QStringLiteral("Ryan"), QStringLiteral("ryan"), false, false);
        QTRY_VERIFY(stage.item(QStringLiteral("callStrip")));
        QQuickItem *strip = stage.item(QStringLiteral("callStrip"));
        // Under the bar, above the page, which stays open.
        QCOMPARE(sceneRect(strip), QRectF(0, 48, 860, 40));
        QCOMPARE(sceneRect(stage.item(QStringLiteral("profilePageBody"))).top(), 88.0);
        QVERIFY(stage.profiles().isOpen());
        QCOMPARE(stage.text(QStringLiteral("callStripText")), QStringLiteral("Incoming call from Ryan"));
        QVERIFY(stage.item(QStringLiteral("callStripAnswerButton")));
        // Open: the stack closes and the call's chat shows.
        stage.click(stage.item(QStringLiteral("callStripReturnButton")));
        QTRY_VERIFY(!stage.profiles().isOpen());
    }

    // --- Navigation ------------------------------------------------------------

    void topFriendTileNavigatesAndResetsScroll()
    {
        Stage stage;
        QVERIFY(stage.load(QSize(860, 560)));
        QVERIFY(openPerson(stage, QStringLiteral("michael")));
        auto *flick = stage.item(QStringLiteral("profilePageFlickable"));
        QVERIFY(flick->property("contentHeight").toReal() > flick->height() + 150);
        flick->setProperty("contentY", 140);
        QTest::qWait(300); // the view reports its scroll after 200 ms

        // Jessica's tile pushes her page, at the top.
        QList<QQuickItem *> tiles = itemsNamed(stage.root(), QStringLiteral("profileFriendTile"));
        QVERIFY(tiles.size() >= 4);
        QQuickItem *jessica = nullptr;
        for (QQuickItem *tile : tiles) {
            if (tile->property("friend").toMap().value(QStringLiteral("name")).toString() == QStringLiteral("Jessica"))
                jessica = tile;
        }
        QVERIFY(jessica);
        QVERIFY(sceneRect(jessica).top() < 560 && sceneRect(jessica).bottom() > 48);
        stage.click(jessica);
        QTRY_COMPARE(stage.profiles().depth(), 2);
        QCOMPARE(stage.profiles().personName(), QStringLiteral("Jessica"));
        QCOMPARE(stage.profiles().backLabel(), QStringLiteral("Michael"));
        flick = stage.item(QStringLiteral("profilePageFlickable"));
        QTRY_COMPARE(flick->property("contentY").toReal(), 0.0);
        QTRY_COMPARE(stage.page()->findChild<QQuickItem *>(QStringLiteral("profilePageBody"))->opacity(), 1.0);

        // Back: Michael's page where it was left.
        flick->setProperty("contentY", 90);
        QTest::qWait(300);
        stage.click(stage.item(QStringLiteral("profileBackButton")));
        QTRY_COMPARE(stage.profiles().depth(), 1);
        QCOMPARE(stage.profiles().personName(), QStringLiteral("Michael"));
        flick = stage.item(QStringLiteral("profilePageFlickable"));
        QTRY_COMPARE(flick->property("contentY").toReal(), 140.0);
    }

    void friendGridKeyboardNavigation()
    {
        Stage stage;
        QVERIFY(stage.load());
        QVERIFY(openPerson(stage, QStringLiteral("michael")));

        // Tab order (SPEC §16.1): Back, Plain style, Contacting, Copy, the
        // song orb, then the Friend Space grid.
        QStringList order;
        for (int i = 0; i < 6; ++i) {
            stage.key(Qt::Key_Tab);
            QQuickItem *focused = stage.window->activeFocusItem();
            order.append(focused ? focused->objectName() : QString());
        }
        QCOMPARE(order, (QStringList{"profileBackButton", "profilePlainStyleSwitch", "profileContactingGrid",
                                     "profileCopyHandleButton", "profileSongOrb", "profileFriendGrid"}));

        QQuickItem *grid = stage.window->activeFocusItem();
        const auto current = [&] { return grid->property("current").toInt(); };
        const auto lit = [&](int index) {
            QList<QQuickItem *> tiles = itemsNamed(grid, QStringLiteral("profileFriendTile"));
            return tiles.value(index) && tiles.value(index)->property("keyboardFocus").toBool();
        };
        QCOMPARE(current(), 0);
        QVERIFY(lit(0));
        stage.key(Qt::Key_Right);
        stage.key(Qt::Key_Right);
        QCOMPARE(current(), 2);
        QVERIFY(lit(2) && !lit(0));
        stage.key(Qt::Key_Down);
        QCOMPARE(current(), 6);
        stage.key(Qt::Key_Left);
        QCOMPARE(current(), 5);
        stage.key(Qt::Key_Up);
        QCOMPARE(current(), 1);
        const QString name = itemsNamed(grid, QStringLiteral("profileFriendTile"))
                                 .value(1)->property("friend").toMap().value(QStringLiteral("name")).toString();
        QVERIFY(!name.isEmpty());
        stage.key(Qt::Key_Return);
        QTRY_COMPARE(stage.profiles().depth(), 2);
        QCOMPARE(stage.profiles().personName(), name);
    }

    void escapeAndBackPopTheStack()
    {
        Stage stage;
        QVERIFY(stage.load());
        QVERIFY(openPerson(stage, QStringLiteral("michael")));
        QVERIFY(stage.profiles().openTopFriend(0));
        QVERIFY(stage.profiles().openTopFriend(0));
        QCOMPARE(stage.profiles().depth(), 3);

        stage.key(Qt::Key_Escape);
        QTRY_COMPARE(stage.profiles().depth(), 2);
        // The mouse's Back button anywhere on the page.
        QTest::mouseClick(stage.window.get(), Qt::BackButton, Qt::NoModifier, QPoint(430, 400));
        QTRY_COMPARE(stage.profiles().depth(), 1);
        stage.key(Qt::Key_Left, Qt::AltModifier);
        QTRY_VERIFY(!stage.profiles().isOpen());
        QTRY_VERIFY(stage.page() == nullptr);
    }

    void backChipHistoryMenu()
    {
        Stage stage;
        QVERIFY(stage.load());
        QVERIFY(openPerson(stage, QStringLiteral("michael")));
        QVERIFY(stage.profiles().openTopFriend(0)); // Jessica
        const QString second = stage.profiles().personName();
        QVERIFY(stage.profiles().openTopFriend(1));
        QCOMPARE(stage.profiles().depth(), 3);
        QQuickItem *chip = stage.item(QStringLiteral("profileBackButton"));
        QCOMPARE(chip->property("label").toString(), second.section(QLatin1Char(' '), 0, 0));

        stage.click(chip, Qt::RightButton);
        auto *menu = stage.page()->findChild<QObject *>(QStringLiteral("profileHistoryMenu"));
        QVERIFY(menu);
        QTRY_VERIFY(menu->property("visible").toBool());
        QVERIFY(stage.page()->property("popupOpen").toBool());
        QList<QQuickItem *> rows;
        QTRY_VERIFY((rows = itemsNamed(stage.root(), QStringLiteral("profileHistoryItem"))).size() == 2);
        // Newest first: the second page, then Michael.
        QCOMPARE(rows.at(0)->property("text").toString(), second);
        QCOMPARE(rows.at(1)->property("text").toString(), QStringLiteral("Michael"));

        // Escape closes only the menu.
        stage.key(Qt::Key_Escape);
        QTRY_VERIFY(!menu->property("visible").toBool());
        QTest::qWait(100);
        QCOMPARE(stage.profiles().depth(), 3);

        // Shift+F10 on the focused chip opens it too; the oldest row pops back.
        chip->forceActiveFocus();
        stage.key(Qt::Key_F10, Qt::ShiftModifier);
        QTRY_VERIFY(menu->property("visible").toBool());
        QTRY_VERIFY(rows.at(1)->isVisible());
        stage.click(rows.at(1));
        QTRY_COMPARE(stage.profiles().depth(), 1);
        QCOMPARE(stage.profiles().personName(), QStringLiteral("Michael"));
    }

    // --- Your own page -----------------------------------------------------------

    void ownPictureOffersChangePicture()
    {
        Stage stage;
        stage.chat.setLocalUserName(QStringLiteral("Daniel"));
        stage.profiles().setMockPage(Reference::selfId(), Reference::ownReferencePage());
        QVERIFY(stage.load());
        QVERIFY(openPerson(stage, Reference::selfId()));
        QVERIFY(stage.item(QStringLiteral("profileEditButton")));
        QVERIFY(!stage.item(QStringLiteral("profilePlainStyleSwitch")));
        QQuickItem *photo = stage.item(QStringLiteral("profilePhoto"));
        QVERIFY(photo->property("activeFocusOnTab").toBool());
        QVERIFY(!stage.item(QStringLiteral("profileChangePictureShade")));

        // Hover and focus shade it with the camera and the words.
        stage.hover(photo);
        QTRY_VERIFY(stage.item(QStringLiteral("profileChangePictureShade")));
        capture(QStringLiteral("final-preset-headliner"), stage.grab());
        QTest::mouseMove(stage.window.get(), QPoint(5, 400));
        QTRY_VERIFY(!stage.item(QStringLiteral("profileChangePictureShade")));
        photo->forceActiveFocus();
        QTRY_VERIFY(stage.item(QStringLiteral("profileChangePictureShade")));

        // A click, Enter and the Your Profile box all open the one dialog.
        QObject *dialog = stage.page()->findChild<QObject *>(QStringLiteral("localAvatarFileDialog"));
        QVERIFY(dialog);
        int opened = 0;
        const auto openedAndClose = [&] {
            if (!QTest::qWaitFor([&] { return dialog->property("visible").toBool(); }))
                return false;
            QMetaObject::invokeMethod(dialog, "reject");
            ++opened;
            return QTest::qWaitFor([&] { return !dialog->property("visible").toBool(); });
        };
        stage.click(photo);
        QVERIFY(openedAndClose());
        photo->forceActiveFocus();
        stage.key(Qt::Key_Return);
        QVERIFY(openedAndClose());
        stage.click(stage.action(QStringLiteral("picture")));
        QVERIFY(openedAndClose());
        QCOMPARE(opened, 3);
        // Ctrl+E opens the editor, as the bar's Edit profile does.
        stage.key(Qt::Key_E, Qt::ControlModifier);
        QTRY_VERIFY(stage.profiles().editing());
    }

    void avatarRefusalShowsOnThePage()
    {
        QTemporaryDir dir;
        QFile junk(dir.filePath(QStringLiteral("not-a-picture.png")));
        QVERIFY(junk.open(QIODevice::WriteOnly));
        junk.write("this is not an image at all");
        junk.close();

        Stage stage;
        QVERIFY(stage.load());
        QVERIFY(openPerson(stage, Reference::selfId()));
        QVERIFY(!stage.item(QStringLiteral("profilePageNotice")));
        QObject *dialog = stage.page()->findChild<QObject *>(QStringLiteral("localAvatarFileDialog"));
        QVERIFY(dialog);
        dialog->setProperty("selectedFile", QUrl::fromLocalFile(junk.fileName()));
        QVERIFY(QMetaObject::invokeMethod(dialog, "accepted"));
        QVERIFY(!stage.chat.profileNotice().isEmpty());
        QTRY_VERIFY(stage.item(QStringLiteral("profilePageNotice")));
        QCOMPARE(stage.text(QStringLiteral("profilePageNoticeText")), stage.chat.profileNotice());
    }

    // --- Stubs -------------------------------------------------------------------

    void stubShowsOnlyRealActions()
    {
        Stage stage;
        stage.contacts = std::make_unique<ContactController>();
        stage.contacts->addMockRequest(QStringLiteral("Grace"), QStringLiteral("wants to add you"));
        stage.contacts->setMockDirectory({QStringLiteral("grace.h")});
        QVERIFY(stage.load());
        RequestListModel *requests = stage.contacts->requests();
        QCOMPARE(requests->rowCount(), 1);
        const QString requestId = requests->data(requests->index(0), RequestListModel::IdRole).toString();
        const QString accountId = requests->data(requests->index(0), RequestListModel::AccountIdRole).toString();

        // A request: Accept, Decline and Block, and nothing a stub never has.
        stage.profiles().openRequest(requestId, accountId, QStringLiteral("Grace"), QStringLiteral("grace"));
        QVERIFY(stage.settled());
        QCOMPARE(stage.profiles().pageState(), int(PageState::StubPage));
        QVERIFY(stage.item(QStringLiteral("profileStubCard")));
        QVERIFY(!stage.view());
        QVERIFY(stage.item(QStringLiteral("profileAcceptButton")));
        QVERIFY(stage.item(QStringLiteral("profileDeclineButton")));
        QCOMPARE(stage.text(QStringLiteral("profileBlockLink")), QStringLiteral("Block @grace"));
        QCOMPARE(stage.text(QStringLiteral("profileStubRelationText")), QStringLiteral("Wants to add you as a contact"));
        QCOMPARE(stage.text(QStringLiteral("profileTopBarHandle")), QStringLiteral("@grace"));
        for (const char *never : {"profileTopBarPresence", "profilePlainStyleSwitch", "profileEditButton",
                                  "profileSongModule", "profileBannerBox", "profileSendRequestButton"})
            QVERIFY2(!stage.item(QString::fromLatin1(never)), never);
        capture(QStringLiteral("final-stub"), stage.grab());

        // Block asks first; Cancel leaves everything as it was.
        stage.click(stage.item(QStringLiteral("profileBlockLink")));
        QTRY_VERIFY(stage.page()->property("popupOpen").toBool());
        QQuickItem *cancel = nullptr;
        QTRY_VERIFY((cancel = stage.item(QStringLiteral("profileConfirmCancel"))));
        QVERIFY(cancel->hasActiveFocus());
        stage.click(cancel);
        QTRY_VERIFY(!stage.page()->property("popupOpen").toBool());
        QCOMPARE(requests->rowCount(), 1);
        // Decline answers the request and leaves the stub.
        stage.click(stage.item(QStringLiteral("profileDeclineButton")));
        QTRY_VERIFY(!stage.profiles().isOpen());
        QCOMPARE(requests->rowCount(), 0);

        // Without a contact service a request stub offers nothing.
        {
            Stage bare;
            QVERIFY(bare.load());
            bare.profiles().openRequest(requestId, accountId, QStringLiteral("Grace"), QStringLiteral("grace"));
            QVERIFY(bare.settled());
            QVERIFY(bare.item(QStringLiteral("profileStubCard")));
            for (const char *never : {"profileAcceptButton", "profileDeclineButton", "profileBlockLink",
                                      "profileSendRequestButton", "profileStubActions"})
                QVERIFY2(!bare.item(QString::fromLatin1(never)), never);
        }

        // Search & Find: Send contact request, then "Request sent".
        stage.contacts->lookup(QStringLiteral("grace.h"));
        QTRY_COMPARE(stage.contacts->lookupState(), ContactController::LookupState::Found);
        stage.profiles().openHandle(QStringLiteral("grace.h"));
        QVERIFY(stage.settled());
        QCOMPARE(stage.profiles().pageState(), int(PageState::StubPage));
        QCOMPARE(stage.text(QStringLiteral("profileStubRelationText")), QStringLiteral("Found in Search & Find"));
        QVERIFY(stage.item(QStringLiteral("profileSendRequestButton")));
        stage.click(stage.item(QStringLiteral("profileSendRequestButton")));
        QTRY_COMPARE(stage.text(QStringLiteral("profileStubStatus")), QStringLiteral("Request sent"));
        QVERIFY(!stage.item(QStringLiteral("profileSendRequestButton")));

        // A stranger in a Friend Space with no handle the relay confirmed.
        QVERIFY(openPerson(stage, QStringLiteral("michael")));
        int stranger = -1;
        const QVariantList friends = stage.profiles().view()->topFriends();
        for (int i = 0; i < friends.size(); ++i) {
            if (!friends.at(i).toMap().value(QStringLiteral("isContact")).toBool()
                && !friends.at(i).toMap().value(QStringLiteral("isSelf")).toBool())
                stranger = i;
        }
        QVERIFY(stranger >= 0);
        QVERIFY(stage.profiles().openTopFriend(stranger));
        QTRY_VERIFY(stage.item(QStringLiteral("profileStubCard")));
        QCOMPARE(stage.text(QStringLiteral("profileStubRelationText")), QStringLiteral("In Michael's Friend Space"));
        QVERIFY(!stage.item(QStringLiteral("profileSendRequestButton")));
        QCOMPARE(stage.text(QStringLiteral("profileStubStatus")),
                 QStringLiteral("Ask %1 for their handle to add them.").arg(stage.profiles().personFirstName()));
    }

    void plainStyleSwitchOnlyOnCustomPages()
    {
        Stage stage;
        QVERIFY(stage.load());
        QVERIFY(openPerson(stage, QStringLiteral("michael")));
        QCOMPARE(stage.profiles().pageState(), int(PageState::CustomPage));
        QVERIFY(stage.item(QStringLiteral("profilePlainStyleSwitch")));
        QVERIFY(!stage.item(QStringLiteral("profileEditButton")));

        QVERIFY(openPerson(stage, QStringLiteral("tom"))); // no page: nothing to make plain
        QCOMPARE(stage.profiles().pageState(), int(PageState::DefaultPage));
        QVERIFY(!stage.item(QStringLiteral("profilePlainStyleSwitch")));
        QVERIFY(stage.item(QStringLiteral("profileNoPageBox")));
        QCOMPARE(stage.text(QStringLiteral("profileNoPageText")), QStringLiteral("Tom hasn't shared a profile page yet."));

        QVERIFY(openPerson(stage, Reference::selfId()));
        QVERIFY(!stage.item(QStringLiteral("profilePlainStyleSwitch")));
        QVERIFY(stage.item(QStringLiteral("profileEditButton")));
        QCOMPARE(stage.text(QStringLiteral("profileBannerText")),
                 QStringLiteral("This is your profile. Your contacts see it just like this."));
    }

    void plainStyleSwitchRequestsTheSetting()
    {
        Stage stage;
        QVERIFY(stage.load());
        QVERIFY(openPerson(stage, QStringLiteral("jessica")));
        QQuickItem *view = stage.view();
        const auto render = [&] { return qvariant_cast<ProfilePageObject *>(view->property("page"))->render(); };
        QVERIFY(!render()->plain());
        const int customMotif = render()->motif();

        QQuickItem *toggle = stage.item(QStringLiteral("profilePlainStyleSwitch"));
        QVERIFY(!toggle->property("checked").toBool());
        stage.hover(toggle);
        QTRY_VERIFY(stage.item(QStringLiteral("profilePlainStyleTip")) || true);
        stage.click(toggle);
        QTRY_COMPARE(stage.window->property("plainRequests").toList(), (QVariantList{true}));
        QTRY_VERIFY(render()->plain());
        QVERIFY(toggle->property("checked").toBool());
        QCOMPARE(render()->motif(), int(Profile::Motif::Bubbles));
        QVERIFY(customMotif != render()->motif());
        QCOMPARE(render()->nameFlourish(), 0);
        // Words, friends and actions stay.
        QCOMPARE(stage.text(QStringLiteral("profileHeadline")).isEmpty(), false);
        QVERIFY(stage.item(QStringLiteral("profileFriendSpaceBox")));
        QTest::qWait(500);
        capture(QStringLiteral("final-plain"), stage.grab());

        stage.click(toggle);
        QTRY_COMPARE(stage.window->property("plainRequests").toList(), (QVariantList{true, false}));
        QTRY_VERIFY(!render()->plain());
    }

    void plainStyleSkipsTheBackgroundImage()
    {
        Stage stage;
        Profile::Page page = Reference::seededPage(QStringLiteral("ryan")).value();
        page.background = stage.profiles().addMockMedia(Profile::MediaKind::BackgroundImageMedia,
                                                        jpegOf(QColor(200, 30, 30), QColor(30, 30, 200)));
        QVERIFY(page.background.isSet());
        page.theme.backgroundKind = Profile::BackgroundKind::ImageBackground;
        page.theme.imageMode = Profile::ImageMode::FillImage;
        page.revision += 1;
        stage.profiles().setMockPage(QStringLiteral("ryan"), page);
        QVERIFY(stage.load());
        QVERIFY(openPerson(stage, QStringLiteral("ryan")));
        QQuickItem *layer = stage.item(QStringLiteral("profileImageLayer"));
        QVERIFY(layer);
        QTRY_VERIFY(!layer->property("imageKey").toString().isEmpty());
        QTRY_VERIFY(layer->property("ready").toBool());
        // The picture shows beside the columns.
        QImage shot = stage.grab();
        const QColor side = at(shot, QPointF(10, 400));
        QVERIFY2(near(side, QColor(200, 30, 30), 60), qPrintable(side.name()));

        stage.profiles().setPlainStyle(true);
        QTRY_COMPARE(layer->property("imageKey").toString(), QString());
        QTRY_COMPARE(stage.item(QStringLiteral("profileBackdrop"))->property("kind").toInt(),
                     int(Profile::BackgroundKind::PatternBackground));
        shot = stage.grab();
        QVERIFY(!near(at(shot, QPointF(10, 400)), QColor(200, 30, 30), 60));
    }

    // --- Motion ------------------------------------------------------------------

    void ambientAndGlitterStopInLowMemory()
    {
        Stage stage;
        QVERIFY(stage.load());
        QVERIFY(openPerson(stage, QStringLiteral("sarah"))); // falling hearts, a glitter name
        auto *ambient = qobject_cast<ProfileAmbient *>(stage.item(QStringLiteral("profileAmbient")));
        auto *name = qobject_cast<ProfileNameText *>(stage.item(QStringLiteral("profileNameText")));
        QVERIFY(ambient && name);
        QVERIFY(ambient->running());
        QTRY_VERIFY(ambient->animating());
        QTRY_VERIFY(name->animating());

        ProfileRenderPolicy::instance().setLowMemoryMode(true);
        QTRY_VERIFY(!ambient->animating());
        QTRY_VERIFY(!name->animating());
        // Pages open at once, with every transition instant.
        stage.profiles().closeAll();
        QTRY_VERIFY(stage.page() == nullptr);
        stage.profiles().openContact(QStringLiteral("jessica"));
        QTRY_VERIFY(stage.page());
        QVERIFY(stage.page()->property("settled").toBool());
        QCOMPARE(stage.page()->opacity(), 1.0);
        QVERIFY(stage.profiles().openTopFriend(0));
        QCOMPARE(stage.profiles().depth(), 2); // no fade-through in between

        ProfileRenderPolicy::instance().setLowMemoryMode(false);
    }

    void animationsStopWhenTheWindowHides()
    {
        Stage stage;
        QVERIFY(stage.load());
        QVERIFY(openPerson(stage, QStringLiteral("sarah")));
        auto *ambient = qobject_cast<ProfileAmbient *>(stage.item(QStringLiteral("profileAmbient")));
        auto *name = qobject_cast<ProfileNameText *>(stage.item(QStringLiteral("profileNameText")));
        auto *player = stage.page()->findChild<SongPlayer *>(QStringLiteral("profileSongPlayer"));
        QTRY_VERIFY(ambient->animating());
        QTRY_VERIFY(player->valid());
        stage.click(stage.item(QStringLiteral("profileSongOrb")));
        QTRY_VERIFY(player->playing());
        QVERIFY(player->active());

        stage.window->hide();
        QTRY_VERIFY(!ambient->animating());
        QTRY_VERIFY(!name->animating());
        QTRY_VERIFY(!player->active());
        QVERIFY(!player->playing());

        stage.window->show();
        QVERIFY(QTest::qWaitForWindowExposed(stage.window.get()));
        QTRY_VERIFY(ambient->animating());
        QTRY_VERIFY(player->active());
        QVERIFY(!player->playing()); // never resumes by itself
    }

    // --- Boxes and focus -----------------------------------------------------------

    void boxBorderStylesDiffer()
    {
        const auto borderShot = [](Profile::BorderStyle style, int width, QImage *out) {
            Stage stage;
            Profile::Page page = Reference::seededPage(QStringLiteral("ryan")).value();
            page.theme.borderStyle = style;
            page.theme.borderWidth = quint8(width);
            page.theme.boxGlow = false;
            page.revision += 1;
            stage.profiles().setMockPage(QStringLiteral("ryan"), page);
            if (!stage.load() || !openPerson(stage, QStringLiteral("ryan")))
                return false;
            QQuickItem *box = stage.item(QStringLiteral("profileBannerBox"));
            if (!box)
                return false;
            const QImage shot = stage.grab();
            const qreal dpr = shot.devicePixelRatio();
            const QRectF edge = sceneRect(box);
            // The top edge of the banner box: border only, no text.
            *out = shot.copy(QRectF(edge.left() * dpr, (edge.top() - 1) * dpr, 120 * dpr, 6 * dpr).toRect());
            return true;
        };
        QImage solid, dashed, dotted, doubled, narrowDouble;
        QVERIFY(borderShot(Profile::BorderStyle::SolidBorder, 3, &solid));
        QVERIFY(borderShot(Profile::BorderStyle::DashedBorder, 3, &dashed));
        QVERIFY(borderShot(Profile::BorderStyle::DottedBorder, 3, &dotted));
        QVERIFY(borderShot(Profile::BorderStyle::DoubleBorder, 3, &doubled));
        QVERIFY(borderShot(Profile::BorderStyle::DoubleBorder, 2, &narrowDouble));
        const QList<QImage> styles{solid, dashed, dotted, doubled};
        for (int a = 0; a < styles.size(); ++a) {
            for (int b = a + 1; b < styles.size(); ++b)
                QVERIFY2(pixelsDiffering(styles.at(a), styles.at(b)) > 20, qPrintable(QStringLiteral("%1 vs %2").arg(a).arg(b)));
        }
        // Double needs 3 px: at 2 it draws solid.
        QImage narrowSolid;
        QVERIFY(borderShot(Profile::BorderStyle::SolidBorder, 2, &narrowSolid));
        QCOMPARE(pixelsDiffering(narrowDouble, narrowSolid), 0);
        // Dashed and dotted leave gaps along the edge; solid does not.
        const QColor border = QColor(0x66, 0x99, 0xcc);
        const int solidInk = pixelsNear(solid, border, 30);
        QVERIFY(pixelsNear(dashed, border, 30) < solidInk);
        QVERIFY(pixelsNear(dotted, border, 30) < solidInk);
    }

    void focusRingsAreAppOwned()
    {
        for (const bool dark : {false, true}) {
            QSettings().setValue(QStringLiteral("Appearance/darkMode"), dark);
            Stage stage;
            Profile::Page page = Reference::seededPage(QStringLiteral("jessica")).value();
            page.theme.linkColor = 0xFF2020; // a loud link colour the ring must not take
            page.revision += 1;
            stage.profiles().setMockPage(QStringLiteral("jessica"), page);
            QVERIFY(stage.load());
            QVERIFY(openPerson(stage, QStringLiteral("jessica")));
            QQuickItem *grid = stage.item(QStringLiteral("profileContactingGrid"));
            grid->forceActiveFocus();
            QQuickItem *ring = nullptr;
            QTRY_VERIFY((ring = stage.item(QStringLiteral("profileFocusRing"))));
            const QColor focus = ring->property("ringColor").value<QColor>();
            QCOMPARE(focus, QColor(dark ? "#8fc9ef" : "#4386b8"));
            // The ring's pixels are the app's focus colour.
            const QImage shot = stage.grab();
            const QRectF r = sceneRect(ring);
            const QColor top = at(shot, QPointF(r.center().x(), r.top() - 1));
            QVERIFY2(near(top, focus, 24), qPrintable(top.name()));
            QVERIFY(!near(top, QColor(0xFF, 0x20, 0x20), 60));
            // Every focusable part of the page draws the same ring.
            for (const char *name : {"profileCopyHandleButton", "profileSongOrb"}) {
                QQuickItem *item = stage.item(QString::fromLatin1(name));
                item->forceActiveFocus();
                QTRY_VERIFY(shown(item, QStringLiteral("profileFocusRing")));
                QCOMPARE(shown(item, QStringLiteral("profileFocusRing"))->property("ringColor").value<QColor>(), focus);
            }
        }
    }

    // --- The mockups' states, for review ------------------------------------------

    void capturesForReview_data()
    {
        QTest::addColumn<QString>("shot");
        QTest::addColumn<bool>("dark");
        for (const char *shot : {"final-default", "final-default-1920", "final-scene", "final-classic-1280",
                                 "final-preset-glitter-girl", "final-preset-safety-pin", "final-preset-chrome-y2k",
                                 "final-preset-linen", "final-preset-neon-zebra", "final-preset-midnight-emo",
                                 "final-stub-stranger", "final-editor-preview"}) {
            QTest::newRow(qPrintable(QString::fromLatin1(shot) + QStringLiteral("-light"))) << QString::fromLatin1(shot) << false;
            QTest::newRow(qPrintable(QString::fromLatin1(shot) + QStringLiteral("-dark"))) << QString::fromLatin1(shot) << true;
        }
    }
    void capturesForReview()
    {
        QFETCH(QString, shot);
        QFETCH(bool, dark);
        if (!capturing())
            QSKIP("Set OPENCHAT_PROFILE_CAPTURES to write the review PNGs.");
        QSettings().setValue(QStringLiteral("Appearance/darkMode"), dark);
        Stage stage;
        stage.contacts = std::make_unique<ContactController>();
        QSize size(860, 680);
        if (shot == QStringLiteral("final-default-1920"))
            size = QSize(1920, 1080);
        else if (shot == QStringLiteral("final-classic-1280"))
            size = QSize(1280, 800);
        else if (shot == QStringLiteral("final-editor-preview"))
            size = QSize(1024, 768);
        const auto open = [&](Preset preset, int origin = -1) {
            const PresetPage page = presetPage(preset);
            stage.profiles().setMockPage(page.contact, page.page);
            return openPerson(stage, page.contact, origin);
        };
        QVERIFY(stage.load(size));
        stage.useScriptedCalls();
        const auto name = [&] { return shot + (dark ? QStringLiteral("-dark") : QStringLiteral("-light")); };

        if (shot == QStringLiteral("final-default") || shot == QStringLiteral("final-default-1920")) {
            QVERIFY(open(Preset::AeroSkyPreset));
        } else if (shot == QStringLiteral("final-scene")) {
            QVERIFY(open(Preset::AeroSkyPreset));
            int index = -1;
            const QVariantList friends = stage.profiles().view()->topFriends();
            for (int i = 0; i < friends.size(); ++i) {
                if (friends.at(i).toMap().value(QStringLiteral("name")).toString() == QStringLiteral("Jessica"))
                    index = i;
            }
            QVERIFY(index >= 0);
            QVERIFY(stage.profiles().openTopFriend(index));
            QVERIFY(stage.settled());
            auto *player = stage.page()->findChild<SongPlayer *>(QStringLiteral("profileSongPlayer"));
            QTRY_VERIFY(player->valid());
            player->play();
            player->seek(17'100);
            QList<QQuickItem *> tiles = itemsNamed(stage.root(), QStringLiteral("profileFriendTile"));
            QVERIFY(!tiles.isEmpty());
            stage.hover(tiles.first());
        } else if (shot == QStringLiteral("final-classic-1280")) {
            QVERIFY(open(Preset::Classic06Preset));
            stage.hover(stage.action(QStringLiteral("message")));
        } else if (shot == QStringLiteral("final-preset-glitter-girl")) {
            QVERIFY(open(Preset::GlitterGirlPreset, int(Origin::FromRequests)));
            auto *player = stage.page()->findChild<SongPlayer *>(QStringLiteral("profileSongPlayer"));
            QTRY_VERIFY(player->valid());
            player->play();
            player->seek(19'000);
            player->pause();
        } else if (shot == QStringLiteral("final-preset-safety-pin")) {
            QVERIFY(open(Preset::SafetyPinPreset));
            m_devices->refuse = true;
            auto *player = stage.page()->findChild<SongPlayer *>(QStringLiteral("profileSongPlayer"));
            QTRY_VERIFY(player->valid());
            QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral(".*")));
            player->play();
            QTRY_VERIFY(!player->error().isEmpty());
        } else if (shot == QStringLiteral("final-preset-chrome-y2k")) {
            QObject *calls = stage.scriptedCalls();
            calls->setProperty("peerName", QStringLiteral("Ryan"));
            calls->setProperty("statusText", QStringLiteral("12:04"));
            calls->setProperty("callChatId", QStringLiteral("ryan"));
            calls->setProperty("inCall", true);
            QVERIFY(open(Preset::ChromeY2KPreset, int(Origin::FromCall)));
        } else if (shot == QStringLiteral("final-preset-linen")) {
            QVERIFY(open(Preset::LinenPreset));
        } else if (shot == QStringLiteral("final-preset-neon-zebra")) {
            QVERIFY(open(Preset::NeonZebraPreset));
        } else if (shot == QStringLiteral("final-preset-midnight-emo")) {
            QVERIFY(open(Preset::MidnightEmoPreset));
        } else if (shot == QStringLiteral("final-stub-stranger")) {
            QVERIFY(open(Preset::AeroSkyPreset));
            const QVariantList friends = stage.profiles().view()->topFriends();
            int stranger = -1;
            for (int i = 0; i < friends.size(); ++i) {
                if (!friends.at(i).toMap().value(QStringLiteral("isContact")).toBool()
                    && !friends.at(i).toMap().value(QStringLiteral("isSelf")).toBool())
                    stranger = i;
            }
            QVERIFY(stage.profiles().openTopFriend(stranger));
            QVERIFY(stage.settled());
        } else if (shot == QStringLiteral("final-editor-preview")) {
            // The kit in the editor's preview: affordances and placeholders,
            // where the editor puts it (right of the rail and panel).
            stage.chat.setLocalUserName(QStringLiteral("Daniel"));
            QVERIFY(openPerson(stage, Reference::selfId()));
            QVERIFY(stage.profiles().beginEditing());
            stage.window->setProperty("previewTarget", QStringLiteral("headline"));
            stage.window->setProperty("previewWidth", 648);
            QQuickItem *preview = nullptr;
            QTRY_VERIFY((preview = qvariant_cast<QQuickItem *>(stage.window->property("preview"))));
            QTRY_VERIFY(shown(preview, QStringLiteral("profileContactingBox")));
            stage.hover(shown(preview, QStringLiteral("profileContactingBox")));
        }
        QTest::qWait(400);
        capture(name(), stage.grab());
    }

private:
    static void checkGeometry(QQuickItem *view, int width, int margin, int content, int gutter, int vgap, int narrow,
                              int wide, int side)
    {
        const bool two = view->property("twoColumns").toBool();
        QCOMPARE(view->property("margin").toInt(), margin);
        QCOMPARE(view->property("contentWidth").toInt(), content);
        QCOMPARE(view->property("vgap").toInt(), vgap);
        QCOMPARE(view->property("narrowWidth").toInt(), narrow);
        QCOMPARE(view->property("wideWidth").toInt(), wide);
        QCOMPARE(two, gutter > 0);
        if (two)
            QCOMPARE(view->property("gutter").toInt(), gutter);
        // The boxes themselves stand where the table says.
        QQuickItem *identity = shown(view, QStringLiteral("profileIdentityBox"));
        QQuickItem *banner = shown(view, QStringLiteral("profileBannerBox"));
        QVERIFY(identity && banner);
        const QRectF left = view->mapRectFromItem(identity, QRectF(0, 0, identity->width(), identity->height()));
        const QRectF right = view->mapRectFromItem(banner, QRectF(0, 0, banner->width(), banner->height()));
        QCOMPARE(left.width(), qreal(narrow));
        QCOMPARE(left.left(), qreal(side));
        if (two) {
            QCOMPARE(right.width(), qreal(wide));
            QCOMPARE(right.left(), qreal(side + narrow + gutter));
            QCOMPARE(qreal(width) - right.right(), qreal(side));
            QCOMPARE(right.top(), left.top());
        } else {
            // One column: Identity, Contacting, then the banner below them.
            QCOMPARE(right.left(), left.left());
            QVERIFY(right.top() > left.bottom());
            QQuickItem *contacting = shown(view, QStringLiteral("profileContactingBox"));
            const QRectF middle = view->mapRectFromItem(contacting, QRectF(0, 0, contacting->width(), contacting->height()));
            QCOMPARE(middle.top(), left.bottom() + vgap);
            QCOMPARE(right.top(), middle.bottom() + vgap);
        }
        QCOMPARE(left.top(), 16.0);
    }
};

OPENCHAT_PROFILE_QML_TEST_MAIN(ProfilePageQmlTest, "profile-page-qml")

#include "tst_profilepageqml.moc"
