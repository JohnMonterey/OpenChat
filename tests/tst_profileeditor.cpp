// The owner's editor in QML (ARCH §9.8): tests/qml/ProfileEditorHarness.qml
// hosts ProfileEditorBar and ProfileEditor as the profile page does, over the
// reference mock's ChatController, and every test drives them the way a
// person would (mouse, keys, focus) and checks the draft, the controller and
// what is on screen. Every QML warning fails the test.
//
// The preview is the page kit's ProfilePageView. Until it lands (the
// scaffold's placeholder is an empty Item) the preview shows nothing, and the
// parts of clickingAModuleOpensItsTabAndField and appOwnedBoxesAreInert that
// click real modules say so and check only the editor's side.
//
// OPENCHAT_PROFILE_CAPTURES=<dir> also saves the editor's states at the
// mockups' sizes (final/final-editor-*.png), light and dark.

#include "ProfileQmlHarness.h"

#include "app/AppearanceSettings.h"
#include "controllers/ChatController.h"
#include "controllers/ProfileController.h"
#include "controllers/ProfilePageObject.h"
#include "controllers/ProfileReferencePages.h"
#include "domain/ProfilePage.h"
#include "profile/ProfileReadability.h"
#include "profile/ProfileRenderPolicy.h"
#include "profile/SongPlayer.h"

#include <QAudioFormat>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QImage>
#include <QLocale>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest/QtTest>

#include <cmath>
#include <memory>
#include <numbers>

using namespace OpenChat;

namespace {

namespace Reference = ProfileReferencePages;
using Profile::EditorTab;

// --- Files a person would pick ----------------------------------------------

void appendLe16(QByteArray &out, quint16 value)
{
    char bytes[2];
    qToLittleEndian(value, bytes);
    out.append(bytes, 2);
}

void appendLe32(QByteArray &out, quint32 value)
{
    char bytes[4];
    qToLittleEndian(value, bytes);
    out.append(bytes, 4);
}

void appendChunk(QByteArray &out, const char *id, const QByteArray &payload)
{
    out.append(id, 4);
    appendLe32(out, quint32(payload.size()));
    out.append(payload);
    if (payload.size() % 2 != 0)
        out.append('\0');
}

// A 48 kHz mono 16-bit WAV of a swelling 440 Hz tone after `silence` seconds,
// tagged with a title and artist.
[[nodiscard]] QString writeSong(const QTemporaryDir &dir, const QString &name, double seconds, double silence)
{
    constexpr int rate = 48'000;
    const qsizetype frames = qsizetype(seconds * rate);
    QByteArray samples;
    samples.reserve(frames * 2);
    for (qsizetype frame = 0; frame < frames; ++frame) {
        const double t = double(frame) / rate;
        const double level = t < silence ? 0.0 : 0.35 * (0.6 + 0.4 * std::sin(2 * std::numbers::pi * 0.5 * t));
        appendLe16(samples, quint16(qint16(std::lround(level * std::sin(2 * std::numbers::pi * 440 * t) * 32767))));
    }
    QByteArray format;
    appendLe16(format, 1); // PCM
    appendLe16(format, 1);
    appendLe32(format, rate);
    appendLe32(format, rate * 2);
    appendLe16(format, 2);
    appendLe16(format, 16);
    QByteArray info("INFO");
    appendChunk(info, "INAM", QByteArrayLiteral("Paper Planes") + '\0');
    appendChunk(info, "IART", QByteArrayLiteral("M.I.A.") + '\0');
    QByteArray body("WAVE");
    appendChunk(body, "fmt ", format);
    appendChunk(body, "LIST", info);
    appendChunk(body, "data", samples);
    QByteArray file("RIFF");
    appendLe32(file, quint32(body.size()));
    file.append(body);

    const QString path = dir.filePath(name);
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly) || out.write(file) != file.size())
        return {};
    return path;
}

[[nodiscard]] QString writePicture(const QTemporaryDir &dir, const QString &name, const QSize &size)
{
    QImage image(size, QImage::Format_RGB32);
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < size.width(); ++x)
            image.setPixel(x, y, qRgb((x * 255) / size.width(), (y * 255) / size.height(), 140));
    }
    const QString path = dir.filePath(name);
    return image.save(path, "PNG") ? path : QString();
}

// A sound card that plays nothing: Listen presses play on the page's player.
class SilentOutput final : public SongOutput
{
public:
    explicit SilentOutput(int channels)
    {
        m_format.setSampleRate(48'000);
        m_format.setChannelCount(channels);
        m_format.setSampleFormat(QAudioFormat::Int16);
    }
    [[nodiscard]] QAudioFormat format() const override { return m_format; }
    [[nodiscard]] int bufferMs() const override { return 0; }
    bool start(QIODevice *) override { return true; }
    void stop() override {}

private:
    QAudioFormat m_format;
};

// --- The editor in its harness ----------------------------------------------

// The editor over the reference mock, in the harness window.
class EditorFixture final
{
public:
    // Opens the viewer's own profile (Daniel's reference page, Headliner,
    // when `daniel`; else the default Aero Sky page), starts editing and shows
    // the editor at `size`.
    explicit EditorFixture(QSize size = QSize(1024, 768), bool daniel = true)
    {
        if (daniel)
            m_chat.profiles()->setMockPage(Reference::selfId(), Reference::ownReferencePage());
        m_chat.profiles()->openOwn();
        m_editing = m_chat.profiles()->beginEditing();
        m_engine.addImportPath(QStringLiteral(OPENCHAT_SOURCE_DIR "/qml"));
        QQmlComponent component(&m_engine,
                                QUrl::fromLocalFile(QStringLiteral(OPENCHAT_SOURCE_DIR
                                                                   "/tests/qml/ProfileEditorHarness.qml")));
        QObject *root = component.createWithInitialProperties(
            {{QStringLiteral("chatController"), QVariant::fromValue(&m_chat)}});
        if (!root)
            qWarning().noquote() << component.errorString();
        m_window.reset(qobject_cast<QQuickWindow *>(root));
        if (m_window) {
            m_window->resize(size);
            m_window->show();
            m_window->requestActivate();
        }
    }

    // Once the window is up and the rail and panel have slid in.
    [[nodiscard]] bool ready()
    {
        if (!m_editing || !m_window || !QTest::qWaitForWindowExposed(m_window.get())
            || !QTest::qWaitForWindowActive(m_window.get()))
            return false;
        // The pointer enters the window once, in a corner, so later moves
        // are hovers (the offscreen window takes its first move as the entry).
        QTest::mouseMove(m_window.get(), QPoint(1, 1));
        QTest::mouseMove(m_window.get(), QPoint(2, 2));
        return waitForEditor();
    }
    [[nodiscard]] bool waitForEditor()
    {
        return QTest::qWaitFor([this] { return editor() && editor()->property("slide").toReal() == 0.0; });
    }

    [[nodiscard]] ChatController &chat() { return m_chat; }
    [[nodiscard]] ProfileController &profiles() { return *m_chat.profiles(); }
    [[nodiscard]] ProfilePageObject &draft() { return *m_chat.profiles()->draft(); }
    [[nodiscard]] QQuickWindow *window() const { return m_window.get(); }
    [[nodiscard]] QQuickItem *editor() const { return item(QStringLiteral("profileEditor")); }
    [[nodiscard]] QQuickItem *bar() const { return item(QStringLiteral("profileEditorBar")); }
    [[nodiscard]] QQuickItem *frame() const { return item(QStringLiteral("profilePreviewFrame")); }

    // Items are looked up in the visual tree (Repeater delegates have no
    // QObject parent, and popups live in the window's overlay).
    [[nodiscard]] QQuickItem *item(const QString &name) const
    {
        const QList<QQuickItem *> all = items(name);
        return all.isEmpty() ? nullptr : all.first();
    }
    [[nodiscard]] QList<QQuickItem *> items(const QString &name, QQuickItem *root = nullptr) const
    {
        QList<QQuickItem *> found;
        if (m_window)
            collect(root ? root : m_window->contentItem(), name, found);
        return found;
    }
    [[nodiscard]] QQuickItem *itemIn(QQuickItem *root, const QString &name) const
    {
        const QList<QQuickItem *> all = items(name, root);
        return all.isEmpty() ? nullptr : all.first();
    }
    // Non-visual objects (popups, dialogs).
    [[nodiscard]] QObject *object(const QString &name) const
    {
        return m_window ? m_window->findChild<QObject *>(name) : nullptr;
    }
    // A shown item called `name` (hidden copies are skipped).
    [[nodiscard]] QQuickItem *visibleItem(const QString &name) const
    {
        for (QQuickItem *candidate : items(name)) {
            if (isShown(candidate))
                return candidate;
        }
        return nullptr;
    }
    static void collect(QQuickItem *root, const QString &name, QList<QQuickItem *> &found)
    {
        if (!root)
            return;
        if (root->objectName() == name)
            found.append(root);
        for (QQuickItem *child : root->childItems())
            collect(child, name, found);
    }
    [[nodiscard]] static bool isShown(const QQuickItem *item)
    {
        for (const QQuickItem *at = item; at; at = at->parentItem()) {
            if (!at->isVisible() || at->opacity() == 0)
                return false;
        }
        return true;
    }
    [[nodiscard]] static bool isInside(const QQuickItem *item, const QQuickItem *ancestor)
    {
        for (const QQuickItem *at = item; at; at = at->parentItem()) {
            if (at == ancestor)
                return true;
        }
        return false;
    }

    // Scrolls the panel so `target` is in view (as a person scrolls to it).
    void scrollTo(QQuickItem *target)
    {
        QQuickItem *flick = item(QStringLiteral("profileEditorPanelFlickable"));
        if (!flick || !isInside(target, flick))
            return;
        auto *content = flick->property("contentItem").value<QQuickItem *>();
        const qreal top = target->mapToItem(content, QPointF(0, 0)).y();
        const qreal contentY = flick->property("contentY").toReal();
        if (top >= contentY && top + target->height() <= contentY + flick->height())
            return; // already in view
        const qreal contentHeight = flick->property("contentHeight").toReal();
        const qreal y = std::clamp(top - 40.0, 0.0, std::max(0.0, contentHeight - flick->height()));
        flick->setProperty("contentY", y);
        QTest::qWait(20);
    }
    // Waits until `target` stays put: positioners place what just appeared
    // (or grew) on the next polish, so its position is only then final.
    static void settle(QQuickItem *target)
    {
        QPointF last = target->mapToScene(QPointF(0, 0));
        for (int frame = 0; frame < 20; ++frame) {
            QTest::qWait(16);
            const QPointF now = target->mapToScene(QPointF(0, 0));
            if (now == last)
                return;
            last = now;
        }
    }
    // Clicks the middle of `target` (or `at`, in its coordinates).
    void click(QQuickItem *target, QPointF at = QPointF(-1, -1), Qt::MouseButton button = Qt::LeftButton,
               Qt::KeyboardModifiers modifiers = Qt::NoModifier)
    {
        QVERIFY(target);
        settle(target);
        scrollTo(target);
        if (at.x() < 0)
            at = QPointF(target->width() / 2, target->height() / 2);
        QTest::mouseClick(m_window.get(), button, modifiers, target->mapToScene(at).toPoint());
    }
    void click(const QString &name) { click(visibleItem(name)); }
    void hover(QQuickItem *target, QPointF at = QPointF(-1, -1))
    {
        QVERIFY(target);
        settle(target);
        if (at.x() < 0)
            at = QPointF(target->width() / 2, target->height() / 2);
        QTest::mouseMove(m_window.get(), target->mapToScene(at).toPoint());
        QTest::qWait(20);
    }
    void key(int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
    {
        QTest::keyClick(m_window.get(), Qt::Key(key), modifiers);
    }
    // Types `text` key by key ("\n" is Return), as QTest::keyClicks does for widgets.
    void type(const QString &text)
    {
        for (const QChar c : text) {
            if (c == QLatin1Char('\n'))
                QTest::keyClick(m_window.get(), Qt::Key_Return);
            else
                QTest::keyClick(m_window.get(), c.toLatin1());
        }
    }
    [[nodiscard]] QQuickItem *focused() const { return m_window ? m_window->activeFocusItem() : nullptr; }
    [[nodiscard]] bool focusIn(const QString &name) const
    {
        QQuickItem *at = focused();
        QQuickItem *target = item(name);
        return at && target && isInside(at, target);
    }

    // Opens editor tab `name` through the rail, as a click would.
    void openTab(const QString &name)
    {
        QQuickItem *tab = item(QStringLiteral("profileEditorTab_") + name);
        QVERIFY2(tab, qPrintable(name));
        click(tab);
        QTRY_VERIFY(tabItem(name) != nullptr);
    }
    [[nodiscard]] QQuickItem *tabItem(const QString &name) const
    {
        static const QHash<QString, QString> names{
            {QStringLiteral("themes"), QStringLiteral("Themes")},   {QStringLiteral("background"), QStringLiteral("Background")},
            {QStringLiteral("boxes"), QStringLiteral("Boxes")},     {QStringLiteral("text"), QStringLiteral("Text")},
            {QStringLiteral("name"), QStringLiteral("Name")},       {QStringLiteral("about"), QStringLiteral("About")},
            {QStringLiteral("friends"), QStringLiteral("Friends")}, {QStringLiteral("song"), QStringLiteral("Song")},
            {QStringLiteral("layout"), QStringLiteral("Layout")}};
        return item(QStringLiteral("profile") + names.value(name) + QStringLiteral("Tab"));
    }
    // The input inside the editor text field `name` (a TextInput or TextEdit).
    [[nodiscard]] QQuickItem *input(const QString &name) const
    {
        QQuickItem *field = item(name);
        return field ? field->property("input").value<QQuickItem *>() : nullptr;
    }
    // Clicks into field `name` and selects what it holds.
    void focusField(const QString &name)
    {
        QQuickItem *field = input(name);
        QVERIFY2(field, qPrintable(name));
        click(field, QPointF(4, 6));
        QTRY_VERIFY(field->hasActiveFocus());
        key(Qt::Key_A, Qt::ControlModifier);
    }
    // A segment called `label` inside the segmented control `control`.
    [[nodiscard]] QQuickItem *segment(const QString &control, const QString &label) const
    {
        return itemIn(item(control), QStringLiteral("profileSegment_") + label);
    }
    [[nodiscard]] QObject *popup(const QString &name) const { return object(name); }
    // Cancels a file dialog the editor opened; the window gets the focus back
    // as it would from the desktop.
    void closeDialog(QObject *dialog)
    {
        QMetaObject::invokeMethod(dialog, "close");
        QTRY_VERIFY(!dialog->property("visible").toBool());
        m_window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
    }
    [[nodiscard]] bool isOpen(const QString &name) const
    {
        QObject *found = popup(name);
        return found && found->property("opened").toBool();
    }
    [[nodiscard]] QString text(QQuickItem *target) const
    {
        return target ? target->property("text").toString() : QString();
    }
    [[nodiscard]] QString text(const QString &name) const { return text(visibleItem(name)); }

    // OPENCHAT_PROFILE_CAPTURES: saves the window as `name`.png.
    void capture(const QString &name) const
    {
        const QString dir = qEnvironmentVariable("OPENCHAT_PROFILE_CAPTURES");
        if (dir.isEmpty() || !m_window)
            return;
        QDir().mkpath(dir);
        QTest::qWait(80);
        const QImage image = m_window->grabWindow();
        QVERIFY2(image.save(QDir(dir).filePath(name + QStringLiteral(".png"))), qPrintable(name));
    }

private:
    ChatController m_chat;
    bool m_editing = false;
    QQmlEngine m_engine;
    std::unique_ptr<QQuickWindow> m_window;
};

// Picks `hex` in the open colour picker through its hex field.
void typeHex(EditorFixture &f, const QString &hex)
{
    QQuickItem *field = f.item(QStringLiteral("profileColorPickerHex"));
    QVERIFY(field);
    QQuickItem *input = nullptr;
    for (QQuickItem *child : field->childItems()) {
        if (child->inherits("QQuickTextInput"))
            input = child;
    }
    QVERIFY(input);
    f.click(input);
    QTRY_VERIFY(input->hasActiveFocus());
    f.key(Qt::Key_A, Qt::ControlModifier);
    f.type(hex);
}

} // namespace

class ProfileEditorTest final : public QObject
{
    Q_OBJECT

private slots:
    void init()
    {
        ProfileQmlHarness::resetProfileSingletons();
        QSettings().clear();
        QTest::failOnWarning(QRegularExpression(QStringLiteral(".*")));
    }

    void cleanup()
    {
        SongPlayer::setOutputFactoryForTesting({});
    }

    // --- Structure -----------------------------------------------------------

    void editorOpensOnlyForOwnProfile()
    {
        EditorFixture f;
        QVERIFY(f.ready());
        ProfileController &profiles = f.profiles();

        // A contact's page has no editor: beginEditing refuses and nothing loads.
        profiles.closeAll();
        QTRY_VERIFY(f.editor() == nullptr);
        QVERIFY(profiles.openContact(QStringLiteral("michael")));
        QVERIFY(!profiles.beginEditing());
        QVERIFY(!profiles.editing());
        QTest::qWait(20);
        QCOMPARE(f.editor(), nullptr);
        QCOMPARE(f.bar(), nullptr);

        // Your own page does: the bar, the rail, the panel and the preview.
        profiles.openOwn();
        QVERIFY(profiles.beginEditing());
        QVERIFY(f.waitForEditor());
        QVERIFY(f.bar() != nullptr);
        QVERIFY(f.item(QStringLiteral("profileEditorRail")));
        QVERIFY(f.item(QStringLiteral("profileEditorPanel")));
        QVERIFY(f.frame());
        // The editor and bar were created with no inputs; they found the
        // page's by themselves (the page kit creates them the same way).
        QCOMPARE(f.editor()->property("profiles").value<QObject *>(), &profiles);
        QCOMPARE(f.editor()->property("chatController").value<QObject *>(), &f.chat());
        QCOMPARE(f.editor()->property("songPlayer").value<QObject *>(), f.object(QStringLiteral("profileSongPlayer")));
        QCOMPARE(f.editor()->property("avatarFileDialog").value<QObject *>(),
                 f.object(QStringLiteral("localAvatarFileDialog")));
        QCOMPARE(f.bar()->property("editor").value<QQuickItem *>(), f.editor());
        QCOMPARE(f.editor()->property("bar").value<QQuickItem *>(), f.bar());
        // The preview shows the draft.
        QCOMPARE(f.frame()->property("shownPage").value<QObject *>(), profiles.draft());
    }

    void railHasNineTabsAndRemembersTheLast()
    {
        const QStringList names{QStringLiteral("themes"), QStringLiteral("background"), QStringLiteral("boxes"),
                                QStringLiteral("text"),   QStringLiteral("name"),       QStringLiteral("about"),
                                QStringLiteral("friends"), QStringLiteral("song"),      QStringLiteral("layout")};
        {
            EditorFixture f;
            QVERIFY(f.ready());
            // Themes the first time.
            QCOMPARE(f.profiles().lastTab(), int(EditorTab::ThemesTab));
            QVERIFY(f.tabItem(QStringLiteral("themes")));
            // Nine tabs, Design then Content, top to bottom.
            qreal previous = -1;
            for (const QString &name : names) {
                QQuickItem *tab = f.item(QStringLiteral("profileEditorTab_") + name);
                QVERIFY2(tab && EditorFixture::isShown(tab), qPrintable(name));
                const qreal y = tab->mapToScene(QPointF(0, 0)).y();
                QVERIFY2(y > previous, qPrintable(name));
                previous = y;
            }
            f.openTab(QStringLiteral("song"));
            QCOMPARE(f.profiles().lastTab(), int(EditorTab::SongTab));
            QVERIFY(f.tabItem(QStringLiteral("song")));
            QCOMPARE(f.tabItem(QStringLiteral("themes")), nullptr);
        }
        // The next visit opens on the tab used last.
        EditorFixture again;
        QVERIFY(again.ready());
        QCOMPARE(again.profiles().lastTab(), int(EditorTab::SongTab));
        QVERIFY(again.tabItem(QStringLiteral("song")));

        // The rail is one Tab stop; ↑/↓ move between tabs.
        again.item(QStringLiteral("profileEditorRail"))->forceActiveFocus();
        again.key(Qt::Key_Down);
        QCOMPARE(again.profiles().lastTab(), int(EditorTab::LayoutTab));
        again.key(Qt::Key_Down); // the last stays the last
        QCOMPARE(again.profiles().lastTab(), int(EditorTab::LayoutTab));
        again.key(Qt::Key_Up);
        again.key(Qt::Key_Up);
        QCOMPARE(again.profiles().lastTab(), int(EditorTab::FriendsTab));
        QTRY_VERIFY(again.tabItem(QStringLiteral("friends")));
    }

    // --- Text fields ---------------------------------------------------------

    void textFieldsRespectLimitsAndCounters()
    {
        EditorFixture f;
        QVERIFY(f.ready());
        f.openTab(QStringLiteral("about"));
        const auto counterOf = [&f](const QString &field) {
            return f.itemIn(f.item(field)->parentItem(), QStringLiteral("profileFieldCounter"));
        };

        QCOMPARE(f.text(counterOf(QStringLiteral("profileField_headline"))), QStringLiteral("20 / 80"));
        QCOMPARE(f.text(counterOf(QStringLiteral("profileField_displayName"))), QStringLiteral("6 / 48"));
        QCOMPARE(f.text(counterOf(QStringLiteral("profileField_aboutMe"))), QStringLiteral("139 / 2000"));

        // Typing stops at the bound; the counter warns in the last 10%.
        f.focusField(QStringLiteral("profileField_headline"));
        f.type(QString(71, QLatin1Char('h')));
        QCOMPARE(f.draft().headline().size(), 71);
        QVERIFY(!counterOf(QStringLiteral("profileField_headline"))->parentItem()->property("nearLimit").toBool());
        f.type(QString(12, QLatin1Char('h')));
        QCOMPARE(f.draft().headline(), QString(80, QLatin1Char('h')));
        QCOMPARE(f.text(f.input(QStringLiteral("profileField_headline"))), QString(80, QLatin1Char('h')));
        QCOMPARE(f.text(counterOf(QStringLiteral("profileField_headline"))), QStringLiteral("80 / 80"));
        QVERIFY(counterOf(QStringLiteral("profileField_headline"))->parentItem()->property("nearLimit").toBool());

        // A text area takes what fits of a paste, and shows what the page holds.
        QQuickItem *about = f.input(QStringLiteral("profileField_aboutMe"));
        about->setProperty("text", QString(2100, QLatin1Char('y')));
        QTRY_COMPARE(f.draft().aboutMe().size(), 2000);
        QTRY_COMPARE(f.text(about).size(), 2000);
        QCOMPARE(f.text(counterOf(QStringLiteral("profileField_aboutMe"))), QStringLiteral("2000 / 2000"));

        // The three short lines are 40 each.
        f.focusField(QStringLiteral("profileField_infoLine2"));
        f.type(QString(45, QLatin1Char('l')));
        QCOMPARE(f.draft().infoLine2(), QString(40, QLatin1Char('l')));
    }

    void typingSpacesAndBlankLinesKeepsThem()
    {
        EditorFixture f;
        QVERIFY(f.ready());
        f.openTab(QStringLiteral("about"));

        f.focusField(QStringLiteral("profileField_headline"));
        f.type(QStringLiteral("a "));
        // The space just typed is kept while typing goes on.
        QCOMPARE(f.draft().headline(), QStringLiteral("a "));
        QCOMPARE(f.text(f.input(QStringLiteral("profileField_headline"))), QStringLiteral("a "));
        f.type(QStringLiteral("b"));
        QCOMPARE(f.draft().headline(), QStringLiteral("a b"));

        f.focusField(QStringLiteral("profileField_aboutMe"));
        f.type(QStringLiteral("x\n\ny"));
        QCOMPARE(f.draft().aboutMe(), QStringLiteral("x\n\ny"));
        QCOMPARE(f.text(f.input(QStringLiteral("profileField_aboutMe"))), QStringLiteral("x\n\ny"));
    }

    // --- Colours -------------------------------------------------------------

    void colorPickerWritesHexSwatchesAndRecent()
    {
        EditorFixture f;
        QVERIFY(f.ready());
        f.openTab(QStringLiteral("background"));
        const QColor original = f.draft().backgroundColor1();
        QSignalSpy history(&f.profiles(), &ProfileController::historyChanged);

        f.click(QStringLiteral("profileBaseColourWell"));
        QTRY_VERIFY(f.isOpen(QStringLiteral("profileColorPicker")));
        QVERIFY(f.editor()->property("popupOpen").toBool());
        // The picker's "From your page" row is the draft's own eight colours.
        QCOMPARE(f.item(QStringLiteral("profileColorPickerPage"))->property("count").toInt(), 8);

        // A classic swatch applies at once.
        f.click(QStringLiteral("profileClassicSwatch_ff2e97"));
        QCOMPARE(f.draft().backgroundColor1(), QColor(QStringLiteral("#ff2e97")));
        // So does a hex value, once it is whole.
        typeHex(f, QStringLiteral("0033"));
        QCOMPARE(f.draft().backgroundColor1(), QColor(QStringLiteral("#ff2e97")));
        f.type(QStringLiteral("99"));
        QCOMPARE(f.draft().backgroundColor1(), QColor(QStringLiteral("#003399")));
        QCOMPARE(f.item(QStringLiteral("profileColorPickerNow"))->property("color").value<QColor>(),
                 QColor(QStringLiteral("#003399")));
        QCOMPARE(f.item(QStringLiteral("profileColorPickerWas"))->property("color").value<QColor>(), original);

        // Enter keeps it: the picker closes and the colour joins Recent.
        f.key(Qt::Key_Return);
        QTRY_VERIFY(!f.isOpen(QStringLiteral("profileColorPicker")));
        QCOMPARE(f.profiles().recentColors().value(0).value<QColor>(), QColor(QStringLiteral("#003399")));
        // The whole session was one undo step.
        QVERIFY(f.profiles().canUndo());
        f.profiles().undo();
        QCOMPARE(f.draft().backgroundColor1(), original);
        QVERIFY(!f.profiles().canUndo());
        f.profiles().redo();
        QCOMPARE(f.draft().backgroundColor1(), QColor(QStringLiteral("#003399")));

        // Esc puts back "was" and closes; Recent is unchanged.
        f.click(QStringLiteral("profileBaseColourWell"));
        QTRY_VERIFY(f.isOpen(QStringLiteral("profileColorPicker")));
        QTRY_VERIFY(f.item(QStringLiteral("profileColorPickerRecent"))->property("count").toInt() >= 1);
        f.click(QStringLiteral("profileClassicSwatch_39ff14"));
        QCOMPARE(f.draft().backgroundColor1(), QColor(QStringLiteral("#39ff14")));
        f.key(Qt::Key_Escape);
        QTRY_VERIFY(!f.isOpen(QStringLiteral("profileColorPicker")));
        QCOMPARE(f.draft().backgroundColor1(), QColor(QStringLiteral("#003399")));
        QCOMPARE(f.profiles().recentColors().size(), 1);
        QVERIFY(f.profiles().editing());
    }

    void colorPickerReadabilityLine()
    {
        EditorFixture f; // Headliner: dark boxes
        QVERIFY(f.ready());
        f.openTab(QStringLiteral("text"));

        f.click(QStringLiteral("profileLinkColourWell"));
        QTRY_VERIFY(f.isOpen(QStringLiteral("profileColorPicker")));
        QQuickItem *line = f.item(QStringLiteral("profileColorPickerReadability"));
        QVERIFY(line && EditorFixture::isShown(line));

        // Too faint on the dark boxes: shown lighter, with the measured ratio.
        typeHex(f, QStringLiteral("222222"));
        const QVariantMap faint = f.profiles().contrastFor(int(Profile::InkRole::LinkInk), QColor(0x22, 0x22, 0x22));
        QVERIFY(!faint.value(QStringLiteral("passes")).toBool());
        QCOMPARE(f.text(QStringLiteral("profileColorPickerRatio")),
                 QString::number(faint.value(QStringLiteral("ratio")).toDouble(), 'f', 1) + QStringLiteral(" : 1 on your boxes"));
        QCOMPARE(f.text(QStringLiteral("profileColorPickerVerdict")), QStringLiteral("Too faint, so shown lighter"));

        // A strong colour is easy to read.
        typeHex(f, QStringLiteral("FFFFFF"));
        const QVariantMap strong = f.profiles().contrastFor(int(Profile::InkRole::LinkInk), QColor(Qt::white));
        QVERIFY(strong.value(QStringLiteral("passes")).toBool());
        QCOMPARE(f.text(QStringLiteral("profileColorPickerRatio")),
                 QString::number(strong.value(QStringLiteral("ratio")).toDouble(), 'f', 1) + QStringLiteral(" : 1 on your boxes"));
        QCOMPARE(f.text(QStringLiteral("profileColorPickerVerdict")), QStringLiteral("Easy to read"));
        f.key(Qt::Key_Return);
        QTRY_VERIFY(!f.isOpen(QStringLiteral("profileColorPicker")));

        // A strip text colour is measured on its strip.
        f.openTab(QStringLiteral("boxes"));
        f.click(QStringLiteral("profileStripTextWell"));
        QTRY_VERIFY(f.isOpen(QStringLiteral("profileColorPicker")));
        QVERIFY(f.text(QStringLiteral("profileColorPickerRatio")).endsWith(QStringLiteral(" : 1 on its strip")));
        f.key(Qt::Key_Escape);
        QTRY_VERIFY(!f.isOpen(QStringLiteral("profileColorPicker")));

        // A colour that is not text has no readability line.
        f.click(QStringLiteral("profileBoxColourWell"));
        QTRY_VERIFY(f.isOpen(QStringLiteral("profileColorPicker")));
        QVERIFY(!EditorFixture::isShown(f.item(QStringLiteral("profileColorPickerReadability"))));
        f.key(Qt::Key_Escape);
    }

    void colorWellBadgesFollowAdjustments()
    {
        EditorFixture f; // Headliner: dark boxes
        QVERIFY(f.ready());
        f.openTab(QStringLiteral("text"));
        const auto badge = [&f](const QString &well) {
            return f.itemIn(f.item(well), QStringLiteral("profileColorWellBadge"));
        };
        QVERIFY(!badge(QStringLiteral("profileLinkColourWell"))->property("warning").toBool());
        QVERIFY(!EditorFixture::isShown(f.item(QStringLiteral("profileReadabilityNotice"))));
        // Only text colours carry a badge.
        f.openTab(QStringLiteral("background"));
        QVERIFY(!badge(QStringLiteral("profileBaseColourWell"))->isVisible());
        f.openTab(QStringLiteral("text"));

        // A link colour too faint for the boxes: the renderer shows it
        // adjusted, the Links badge turns amber and the notice explains.
        f.draft().setLinkColor(QColor(0x2a, 0x2a, 0x2a));
        QVERIFY(f.draft().render()->inkAdjusted().value(QString::number(int(Profile::InkRole::LinkInk))).toBool());
        QTRY_VERIFY(badge(QStringLiteral("profileLinkColourWell"))->property("warning").toBool());
        QVERIFY(!badge(QStringLiteral("profileBodyColourWell"))->property("warning").toBool());
        QQuickItem *notice = f.item(QStringLiteral("profileReadabilityNotice"));
        QVERIFY(EditorFixture::isShown(notice));
        QVERIFY(notice->property("entries").toList().size() >= 1);

        // "Show me" points the preview at a box that shows it, for a moment.
        f.click(QStringLiteral("profileReadabilityShowMe"));
        QCOMPARE(f.frame()->property("editingTarget").toString(), QStringLiteral("contacting"));
        QTRY_COMPARE_WITH_TIMEOUT(f.frame()->property("editingTarget").toString(), QString(), 3000);

        // A readable colour: the check again, and no notice.
        f.draft().setLinkColor(QColor(0xf0, 0xf0, 0xf0));
        QTRY_VERIFY(!badge(QStringLiteral("profileLinkColourWell"))->property("warning").toBool());
        QTRY_VERIFY(!EditorFixture::isShown(f.item(QStringLiteral("profileReadabilityNotice"))));
    }

    // --- Design tabs ---------------------------------------------------------

    void patternPickerShowsNoneAndEveryMotif()
    {
        EditorFixture f(QSize(1024, 768), false); // Aero Sky: Bubbles
        QVERIFY(f.ready());
        f.openTab(QStringLiteral("background"));
        QQuickItem *grid = f.item(QStringLiteral("profilePatternGrid"));
        QCOMPARE(grid->property("count").toInt(), 18);
        QVERIFY(EditorFixture::isShown(f.item(QStringLiteral("profilePatternTile_none"))));
        for (int motif = int(Profile::Motif::Stars); motif <= int(Profile::Motif::LinenWeave); ++motif) {
            QQuickItem *tile = f.item(QStringLiteral("profilePatternTile_%1").arg(motif));
            QVERIFY2(tile && EditorFixture::isShown(tile), qPrintable(QString::number(motif)));
            // Painted live, in the page's own base and ink, at its motif.
            QQuickItem *backdrop = nullptr;
            for (QQuickItem *child : tile->childItems()) {
                if (child->inherits("OpenChat::ProfileBackdrop"))
                    backdrop = child;
            }
            QVERIFY(backdrop);
            QCOMPARE(backdrop->property("motif").toInt(), motif);
            QCOMPARE(backdrop->property("color1").value<QColor>(), f.draft().render()->color1());
            QCOMPARE(backdrop->property("motifInk").value<QColor>(), f.draft().render()->motifInk());
        }
        QCOMPARE(f.text(QStringLiteral("profilePatternName")), QStringLiteral("·  Bubbles"));

        f.click(QStringLiteral("profilePatternTile_4"));
        QCOMPARE(f.draft().backgroundKind(), int(Profile::BackgroundKind::PatternBackground));
        QCOMPARE(f.draft().motif(), int(Profile::Motif::Zebra));
        QCOMPARE(f.text(QStringLiteral("profilePatternName")), QStringLiteral("·  Zebra"));
        QCOMPARE(grid->property("currentIndex").toInt(), grid->property("model").toList().indexOf(4));

        // None keeps the colours as a gradient.
        f.click(QStringLiteral("profilePatternTile_none"));
        QCOMPARE(f.draft().backgroundKind(), int(Profile::BackgroundKind::GradientBackground));
        QCOMPARE(f.text(QStringLiteral("profilePatternName")), QStringLiteral("·  None"));
        QCOMPARE(grid->property("currentIndex").toInt(), 0);
        // The pattern's own knobs show only for a pattern.
        QVERIFY(!EditorFixture::isShown(f.item(QStringLiteral("profilePatternColourWell"))));

        // Keyboard: one Tab stop, arrows walk, Enter picks.
        grid->forceActiveFocus(Qt::TabFocusReason);
        f.key(Qt::Key_Right);
        f.key(Qt::Key_Right);
        f.key(Qt::Key_Return);
        QCOMPARE(f.draft().motif(), grid->property("model").toList().at(2).toInt());
        QCOMPARE(f.draft().backgroundKind(), int(Profile::BackgroundKind::PatternBackground));
        QVERIFY(EditorFixture::isShown(f.item(QStringLiteral("profilePatternColourWell"))));
    }

    void themesTilesTryOnAfter250msAndApplyOnClick()
    {
        EditorFixture f; // Headliner
        QVERIFY(f.ready());
        ProfileController &profiles = f.profiles();
        QCOMPARE(profiles.draft()->preset(), int(Profile::Preset::HeadlinerPreset));
        QQuickItem *chrome = f.item(QStringLiteral("profileThemeTile_chrome-y2k"));
        QVERIFY(chrome);

        // Resting on a tile: nothing before 250 ms, the try-on after.
        f.hover(chrome);
        QTest::qWait(150);
        QCOMPARE(profiles.tryOnPreset(), -1);
        QTRY_COMPARE_WITH_TIMEOUT(profiles.tryOnPreset(), int(Profile::Preset::ChromeY2KPreset), 1000);
        QTRY_COMPARE(f.frame()->property("shownPage").value<QObject *>(), profiles.tryOn());
        QVERIFY(EditorFixture::isShown(f.item(QStringLiteral("profileTryOnPill"))));
        QCOMPARE(f.text(QStringLiteral("profileTryOnPillText")), QStringLiteral("Trying on Chrome Y2K. Click to keep it."));
        // The draft itself does not change.
        QCOMPARE(profiles.draft()->preset(), int(Profile::Preset::HeadlinerPreset));
        QVERIFY(!profiles.canUndo());

        // Moving away puts the draft back.
        f.hover(f.item(QStringLiteral("profileEditorRail")));
        QTRY_COMPARE(profiles.tryOnPreset(), -1);
        QTRY_COMPARE(f.frame()->property("shownPage").value<QObject *>(), profiles.draft());
        QVERIFY(!EditorFixture::isShown(f.item(QStringLiteral("profileTryOnPill"))));

        // A click applies it: one undo step.
        f.click(f.item(QStringLiteral("profileThemeTile_linen")));
        QCOMPARE(profiles.draft()->preset(), int(Profile::Preset::LinenPreset));
        QCOMPARE(profiles.tryOnPreset(), -1);
        QVERIFY(profiles.canUndo());
        profiles.undo();
        QCOMPARE(profiles.draft()->preset(), int(Profile::Preset::HeadlinerPreset));

        // Keyboard: the grid is one Tab stop on the chosen tile; resting on a
        // tile tries it on, Enter applies it.
        f.hover(f.item(QStringLiteral("profileEditorRail")));
        QQuickItem *grid = f.item(QStringLiteral("profileThemesGrid"));
        grid->forceActiveFocus(Qt::TabFocusReason);
        QCOMPARE(grid->property("focusIndex").toInt(), 7); // Headliner
        f.key(Qt::Key_Down); // Chrome Y2K, under it
        QTRY_COMPARE(profiles.tryOnPreset(), int(Profile::Preset::ChromeY2KPreset));
        f.key(Qt::Key_Return);
        QCOMPARE(profiles.draft()->preset(), int(Profile::Preset::ChromeY2KPreset));
        QCOMPARE(profiles.tryOnPreset(), -1);
    }

    void presetTagsDefaultAndEdited()
    {
        EditorFixture f(QSize(1024, 768), false); // Aero Sky, unedited
        QVERIFY(f.ready());
        const auto tagOf = [&f](const QString &slug) {
            QQuickItem *tag = f.itemIn(f.item(QStringLiteral("profileThemeTile_") + slug), QStringLiteral("profileThemeTagText"));
            return tag && EditorFixture::isShown(tag) ? f.text(tag) : QString();
        };
        QCOMPARE(tagOf(QStringLiteral("aero-sky")), QStringLiteral("default"));
        QCOMPARE(tagOf(QStringLiteral("classic-06")), QString());

        // Any knob away from the preset tags the chosen tile "edited".
        f.draft().setBoxOpacity(70);
        QVERIFY(f.draft().styleEditedSincePreset());
        QTRY_COMPARE(tagOf(QStringLiteral("aero-sky")), QStringLiteral("edited"));
        QCOMPARE(tagOf(QStringLiteral("classic-06")), QString());

        // Another preset: chosen and clean; Aero Sky is "default" again.
        f.click(f.item(QStringLiteral("profileThemeTile_classic-06")));
        QCOMPARE(f.draft().preset(), int(Profile::Preset::Classic06Preset));
        QTRY_COMPARE(tagOf(QStringLiteral("aero-sky")), QStringLiteral("default"));
        QCOMPARE(tagOf(QStringLiteral("classic-06")), QString());
        f.draft().setBorderWidth(4);
        QTRY_COMPARE(tagOf(QStringLiteral("classic-06")), QStringLiteral("edited"));
    }

    void resetAndSurpriseAreUndoable()
    {
        EditorFixture f; // Headliner
        QVERIFY(f.ready());
        ProfileController &profiles = f.profiles();
        QQuickItem *reset = f.item(QStringLiteral("profileResetToPresetButton"));
        QVERIFY(reset && EditorFixture::isShown(reset));
        QVERIFY(!reset->isEnabled());

        const int opacity = f.draft().boxOpacity();
        f.draft().setBoxOpacity(opacity == 70 ? 72 : 70);
        QTRY_VERIFY(reset->isEnabled());
        f.click(reset);
        QCOMPARE(f.draft().boxOpacity(), opacity);
        QVERIFY(!f.draft().styleEditedSincePreset());
        profiles.undo();
        QVERIFY(f.draft().styleEditedSincePreset());
        profiles.redo();
        QCOMPARE(f.draft().boxOpacity(), opacity);

        // Surprise me: another preset (words untouched), one undo step.
        const QString headline = f.draft().headline();
        f.click(QStringLiteral("profileSurpriseButton"));
        QVERIFY(f.draft().preset() != int(Profile::Preset::HeadlinerPreset));
        QCOMPARE(f.draft().headline(), headline);
        profiles.undo();
        QCOMPARE(f.draft().preset(), int(Profile::Preset::HeadlinerPreset));
        QCOMPARE(f.draft().boxOpacity(), opacity);
    }

    void bodyFontSegmentsAreBodySafeOnly()
    {
        EditorFixture f;
        QVERIFY(f.ready());
        f.openTab(QStringLiteral("text"));
        QQuickItem *body = f.item(QStringLiteral("profileBodyFont"));
        const QStringList options = body->property("options").toStringList();
        QStringList bodySafe;
        for (const QVariant &choice : f.profiles().fontChoices()) {
            if (choice.toMap().value(QStringLiteral("bodySafe")).toBool())
                bodySafe.append(choice.toMap().value(QStringLiteral("name")).toString());
        }
        QCOMPARE(options, (QStringList{QStringLiteral("Standard"), QStringLiteral("Rounded"),
                                      QStringLiteral("Typewriter"), QStringLiteral("Serif")}));
        QCOMPARE(QSet<QString>(options.begin(), options.end()), QSet<QString>(bodySafe.begin(), bodySafe.end()));

        f.click(f.segment(QStringLiteral("profileBodyFont"), QStringLiteral("Serif")));
        QCOMPARE(f.draft().bodyFont(), int(Profile::Font::SerifFont));
        QCOMPARE(body->property("currentIndex").toInt(), 3);
        f.click(f.segment(QStringLiteral("profileBodyFont"), QStringLiteral("Rounded")));
        QCOMPARE(f.draft().bodyFont(), int(Profile::Font::RoundedFont));
        // Keyboard: ← moves the choice.
        body->forceActiveFocus(Qt::TabFocusReason);
        f.key(Qt::Key_Left);
        QCOMPARE(f.draft().bodyFont(), int(Profile::Font::InterfaceFont));
    }

    void headingListOmitsPixel()
    {
        EditorFixture f;
        QVERIFY(f.ready());
        f.openTab(QStringLiteral("text"));
        QQuickItem *list = f.item(QStringLiteral("profileHeadingList"));
        QCOMPARE(list->property("count").toInt(), 8);
        QCOMPARE(f.item(QStringLiteral("profileHeadingFont_%1").arg(int(Profile::Font::PixelFont))), nullptr);
        for (const QVariant &entry : f.profiles().fontChoices()) {
            const QVariantMap choice = entry.toMap();
            const int id = choice.value(QStringLiteral("id")).toInt();
            if (!choice.value(QStringLiteral("headingCapable")).toBool())
                continue;
            QQuickItem *row = f.item(QStringLiteral("profileHeadingFont_%1").arg(id));
            QVERIFY2(row, qPrintable(QString::number(id)));
            // Each row says the owner's heading in its own face.
            QQuickItem *sample = nullptr;
            for (QQuickItem *child : row->childItems()) {
                if (child->property("text").toString() == QStringLiteral("Daniel's Blurbs"))
                    sample = child;
            }
            QVERIFY2(sample, qPrintable(QString::number(id)));
            const QString family = choice.value(QStringLiteral("headingFamily")).toString();
            if (!family.isEmpty())
                QCOMPARE(sample->property("font").value<QFont>().family(), family);
        }
        f.click(f.item(QStringLiteral("profileHeadingFont_%1").arg(int(Profile::Font::ScriptFont))));
        QCOMPARE(f.draft().headingFont(), int(Profile::Font::ScriptFont));
    }

    void nameTabEffectsSizesAndFlourishes()
    {
        EditorFixture f;
        QVERIFY(f.ready());
        f.openTab(QStringLiteral("name"));
        // Nine faces, Pixel included (names may use it).
        QCOMPARE(f.item(QStringLiteral("profileNameFontGrid"))->property("count").toInt(), 9);
        QVERIFY(f.item(QStringLiteral("profileNameFont_%1").arg(int(Profile::Font::PixelFont))));
        f.click(f.item(QStringLiteral("profileNameFont_%1").arg(int(Profile::Font::PixelFont))));
        QCOMPARE(f.draft().nameFont(), int(Profile::Font::PixelFont));

        // Seven effects; the second colour is named for what the effect does.
        QCOMPARE(f.item(QStringLiteral("profileNameEffectGrid"))->property("count").toInt(), 7);
        QQuickItem *second = f.item(QStringLiteral("profileNameColour2Well"));
        f.click(f.item(QStringLiteral("profileNameEffect_1")));
        QCOMPARE(f.draft().nameEffect(), int(Profile::NameEffect::GlowName));
        QTRY_VERIFY(EditorFixture::isShown(second));
        QCOMPARE(second->property("label").toString(), QStringLiteral("Glow colour"));
        f.click(f.item(QStringLiteral("profileNameEffect_3")));
        QCOMPARE(second->property("label").toString(), QStringLiteral("Fades to"));
        f.click(f.item(QStringLiteral("profileNameEffect_0")));
        QCOMPARE(f.draft().nameEffect(), int(Profile::NameEffect::PlainName));
        QVERIFY(!EditorFixture::isShown(second));

        // Size, flourish and the falling sparkle.
        f.click(f.segment(QStringLiteral("profileNameSize"), QStringLiteral("Extra large")));
        QCOMPARE(f.draft().nameSize(), int(Profile::NameSize::ExtraLargeName));
        QCOMPARE(f.item(QStringLiteral("profileFlourishGrid"))->property("count").toInt(), 7);
        f.click(f.item(QStringLiteral("profileFlourish_1")));
        QCOMPARE(f.draft().nameFlourish(), int(Profile::Flourish::StarFlourish));
        f.click(f.segment(QStringLiteral("profileAmbient"), QStringLiteral("Snow")));
        QCOMPARE(f.draft().ambient(), int(Profile::Ambient::FallingSnow));

        // The close-up shows the name with its flourish, effect and colours.
        QQuickItem *closeUp = f.item(QStringLiteral("profileNameCloseUp"));
        QCOMPARE(closeUp->property("text").toString(), QStringLiteral("Daniel"));
        QCOMPARE(closeUp->property("flourish").toInt(), int(Profile::Flourish::StarFlourish));
        QCOMPARE(closeUp->property("effect").toInt(), f.draft().render()->nameEffect());
        QCOMPARE(closeUp->property("color").value<QColor>(), f.draft().render()->nameColor());
        QCOMPARE(closeUp->property("basePixelSize").toInt(), 36);
    }

    void layoutArrangerMovesHidesAndKeepsLockedRows()
    {
        EditorFixture f(QSize(1024, 900));
        QVERIFY(f.ready());
        f.openTab(QStringLiteral("layout"));
        ProfileController &profiles = f.profiles();
        const auto order = [&f](Profile::Column column) {
            QList<int> modules;
            for (const QVariant &entry : f.draft().moduleArrangement()) {
                if (entry.toMap().value(QStringLiteral("column")).toInt() == int(column))
                    modules.append(entry.toMap().value(QStringLiteral("module")).toInt());
            }
            return modules;
        };
        const auto visible = [&f](Profile::Module module) {
            for (const QVariant &entry : f.draft().moduleArrangement()) {
                if (entry.toMap().value(QStringLiteral("module")).toInt() == int(module))
                    return entry.toMap().value(QStringLiteral("visible")).toBool();
            }
            return false;
        };
        using M = Profile::Module;
        const QList<int> narrow{int(M::HandleModule), int(M::InterestsModule), int(M::DetailsModule)};
        QCOMPARE(order(Profile::Column::NarrowColumn), narrow);

        // The app-owned boxes are listed, locked.
        QStringList locked;
        for (QQuickItem *row : f.items(QStringLiteral("profileLayoutLockedRow"))) {
            if (EditorFixture::isShown(row))
                locked.append(row->property("label").toString() + QLatin1Char('|') + row->property("note").toString());
        }
        QCOMPARE(locked.size(), 3);
        QVERIFY(locked.contains(QStringLiteral("Name & photo|always first")));
        QVERIFY(locked.contains(QStringLiteral("Contacting Daniel|always shown")));
        QVERIFY(locked.contains(QStringLiteral("“In your contacts”|always first")));

        // The eye hides and shows a box.
        f.click(QStringLiteral("profileLayoutEye_%1").arg(int(M::DetailsModule)));
        QVERIFY(!visible(M::DetailsModule));
        f.click(QStringLiteral("profileLayoutEye_%1").arg(int(M::DetailsModule)));
        QVERIFY(visible(M::DetailsModule));

        // A row under the pointer offers ↑ ↓ ⇄.
        QQuickItem *interests = f.item(QStringLiteral("profileLayoutRow_%1").arg(int(M::InterestsModule)));
        f.hover(interests);
        QTRY_VERIFY(f.itemIn(interests, QStringLiteral("profileLayoutRowAction_up")));
        f.click(f.itemIn(interests, QStringLiteral("profileLayoutRowAction_up")));
        QCOMPARE(order(Profile::Column::NarrowColumn),
                 (QList<int>{int(M::InterestsModule), int(M::HandleModule), int(M::DetailsModule)}));
        QQuickItem *handle = f.item(QStringLiteral("profileLayoutRow_%1").arg(int(M::HandleModule)));
        f.hover(handle);
        QTRY_VERIFY(f.itemIn(handle, QStringLiteral("profileLayoutRowAction_swap")));
        f.click(f.itemIn(handle, QStringLiteral("profileLayoutRowAction_swap")));
        QVERIFY(order(Profile::Column::WideColumn).contains(int(M::HandleModule)));
        profiles.undo();
        profiles.undo();
        QCOMPARE(order(Profile::Column::NarrowColumn), narrow);

        // Keyboard: ↓ walks, Alt+↓ moves, Alt+→ goes to the right column, Space hides.
        QQuickItem *first = f.item(QStringLiteral("profileLayoutFirstList"));
        first->forceActiveFocus(Qt::TabFocusReason);
        QCOMPARE(first->property("focusIndex").toInt(), 0);
        f.key(Qt::Key_Down, Qt::AltModifier); // handle below interests
        QCOMPARE(order(Profile::Column::NarrowColumn),
                 (QList<int>{int(M::InterestsModule), int(M::HandleModule), int(M::DetailsModule)}));
        QCOMPARE(first->property("focusIndex").toInt(), 1); // the cursor stays on the handle
        f.key(Qt::Key_Right, Qt::AltModifier);
        QVERIFY(order(Profile::Column::WideColumn).contains(int(M::HandleModule)));
        QVERIFY(f.item(QStringLiteral("profileLayoutSecondList"))->hasActiveFocus());
        f.key(Qt::Key_Space);
        QVERIFY(!visible(M::HandleModule));
        f.key(Qt::Key_Left, Qt::AltModifier);
        QVERIFY(order(Profile::Column::NarrowColumn).contains(int(M::HandleModule)));

        // Dragging by the grip: the row follows the pointer, a line shows
        // where it lands, and the drop moves it as one undo step.
        const auto drag = [&f](QQuickItem *from, QQuickItem *onto, QPointF at) {
            QQuickItem *grip = f.itemIn(from, QStringLiteral("profileLayoutGrip"));
            f.settle(grip);
            const QPoint start = grip->mapToScene(QPointF(grip->width() / 2, grip->height() / 2)).toPoint();
            const QPoint target = onto->mapToScene(at).toPoint();
            QTest::mousePress(f.window(), Qt::LeftButton, Qt::NoModifier, start);
            for (int step = 1; step <= 6; ++step)
                QTest::mouseMove(f.window(), start + (target - start) * step / 6);
            return target;
        };
        const auto lineShown = [&f] {
            for (QQuickItem *line : f.items(QStringLiteral("profileLayoutInsertionLine"))) {
                if (EditorFixture::isShown(line))
                    return true;
            }
            return false;
        };
        const QList<int> wideBefore = order(Profile::Column::WideColumn);
        QCOMPARE(wideBefore.first(), int(M::SongModule)); // Headliner leads with the song
        QQuickItem *song = f.item(QStringLiteral("profileLayoutRow_%1").arg(int(M::SongModule)));
        QPoint dropAt = drag(f.item(QStringLiteral("profileLayoutRow_%1").arg(int(M::TopFriendsModule))), song, QPointF(20, 4));
        QVERIFY(EditorFixture::isShown(f.item(QStringLiteral("profileLayoutDragRow"))));
        QTRY_VERIFY(lineShown());
        QCOMPARE(order(Profile::Column::WideColumn), wideBefore); // nothing moves before the drop
        QTest::mouseRelease(f.window(), Qt::LeftButton, Qt::NoModifier, dropAt);
        QVERIFY(!EditorFixture::isShown(f.item(QStringLiteral("profileLayoutDragRow"))));
        QVERIFY(!lineShown());
        QCOMPARE(order(Profile::Column::WideColumn),
                 (QList<int>{int(M::TopFriendsModule), int(M::SongModule), int(M::BlurbsModule)}));
        profiles.undo();
        QCOMPARE(order(Profile::Column::WideColumn), wideBefore);

        // A drop at the end of the other column moves it across.
        QQuickItem *lastWide = f.item(QStringLiteral("profileLayoutRow_%1").arg(wideBefore.last()));
        dropAt = drag(f.item(QStringLiteral("profileLayoutRow_%1").arg(int(M::DetailsModule))), lastWide,
                      QPointF(20, lastWide->height() - 4));
        QTRY_VERIFY(lineShown());
        QTest::mouseRelease(f.window(), Qt::LeftButton, Qt::NoModifier, dropAt);
        QCOMPARE(order(Profile::Column::WideColumn), wideBefore + QList<int>{int(M::DetailsModule)});
        QVERIFY(!order(Profile::Column::NarrowColumn).contains(int(M::DetailsModule)));
        profiles.undo();
        QCOMPARE(order(Profile::Column::WideColumn), wideBefore);

        // The layout cards.
        f.click(f.item(QStringLiteral("profileLayoutCard_1")));
        QCOMPARE(f.draft().layout(), int(Profile::Layout::FlippedLayout));
        f.click(f.item(QStringLiteral("profileLayoutCard_2")));
        QCOMPARE(f.draft().layout(), int(Profile::Layout::SingleLayout));
    }

    void topFriendsSlotsAddRemoveReorderAndPrivacyNote()
    {
        EditorFixture f(QSize(1024, 900), false); // no friends yet
        QVERIFY(f.ready());
        f.openTab(QStringLiteral("friends"));
        ProfileController &profiles = f.profiles();
        QCOMPARE(f.text(QStringLiteral("profileFriendsPrivacyNote")), QStringLiteral("Your contacts will see who you put here."));
        QCOMPARE(f.item(QStringLiteral("profileFriendSlots"))->property("count").toInt(), 8);
        QCOMPARE(f.draft().topFriends().size(), 0);

        // Only accepted contacts are offered; the row under the pointer adds.
        QCOMPARE(f.item(QStringLiteral("profileFriendCandidates"))->property("count").toInt(),
                 profiles.topFriendCandidates().size());
        QQuickItem *jessica = f.item(QStringLiteral("profileFriendCandidate_jessica"));
        f.hover(jessica);
        QTRY_VERIFY(EditorFixture::isShown(f.itemIn(jessica, QStringLiteral("profileFriendAddButton"))));
        QCOMPARE(f.itemIn(jessica, QStringLiteral("profileFriendAddButton"))->property("label").toString(),
                 QStringLiteral("Add as #1"));
        f.click(f.itemIn(jessica, QStringLiteral("profileFriendAddButton")));
        QCOMPARE(f.draft().topFriends().size(), 1);
        QCOMPARE(f.draft().topFriends().value(0).toMap().value(QStringLiteral("name")).toString(), QStringLiteral("Jessica"));
        QTRY_COMPARE(f.text(f.itemIn(f.item(QStringLiteral("profileFriendCandidate_jessica")),
                                     QStringLiteral("profileFriendCandidatePlace"))),
                     QStringLiteral("#1"));
        QQuickItem *michael = f.item(QStringLiteral("profileFriendCandidate_michael"));
        f.hover(michael);
        QTRY_VERIFY(EditorFixture::isShown(f.itemIn(michael, QStringLiteral("profileFriendAddButton"))));
        QCOMPARE(f.itemIn(michael, QStringLiteral("profileFriendAddButton"))->property("label").toString(),
                 QStringLiteral("Add as #2"));
        f.click(f.itemIn(michael, QStringLiteral("profileFriendAddButton")));
        QCOMPARE(f.draft().topFriends().size(), 2);

        const auto names = [&f] {
            QStringList list;
            for (const QVariant &tile : f.draft().topFriends())
                list.append(tile.toMap().value(QStringLiteral("name")).toString());
            return list;
        };
        // Alt+→ on a slot moves it later; Delete removes it.
        QQuickItem *slotGrid = f.item(QStringLiteral("profileFriendSlots"));
        slotGrid->forceActiveFocus(Qt::TabFocusReason);
        QCOMPARE(slotGrid->property("focusIndex").toInt(), 0);
        f.key(Qt::Key_Right, Qt::AltModifier);
        QCOMPARE(names(), (QStringList{QStringLiteral("Michael"), QStringLiteral("Jessica")}));
        QCOMPARE(slotGrid->property("focusIndex").toInt(), 1);

        // Dragging a picture onto another slot moves it there.
        QQuickItem *slot0 = f.item(QStringLiteral("profileFriendSlot_0"));
        QQuickItem *slot1 = f.item(QStringLiteral("profileFriendSlot_1"));
        const QPoint from = slot0->mapToScene(QPointF(20, 20)).toPoint();
        const QPoint to = slot1->mapToScene(QPointF(20, 20)).toPoint();
        QTest::mousePress(f.window(), Qt::LeftButton, Qt::NoModifier, from);
        for (int step = 1; step <= 5; ++step)
            QTest::mouseMove(f.window(), from + (to - from) * step / 5);
        QTest::mouseRelease(f.window(), Qt::LeftButton, Qt::NoModifier, to);
        QCOMPARE(names(), (QStringList{QStringLiteral("Jessica"), QStringLiteral("Michael")}));
        profiles.undo();
        QCOMPARE(names(), (QStringList{QStringLiteral("Michael"), QStringLiteral("Jessica")}));

        // Removal: a slot's context menu, or Delete on a focused slot.
        f.click(f.item(QStringLiteral("profileFriendSlot_1")), QPointF(20, 20), Qt::RightButton);
        QTRY_VERIFY(f.isOpen(QStringLiteral("profileFriendSlotMenu")));
        QVERIFY(f.editor()->property("popupOpen").toBool());
        f.click(f.visibleItem(QStringLiteral("profileFriendRemove")));
        QTRY_VERIFY(!f.isOpen(QStringLiteral("profileFriendSlotMenu")));
        QCOMPARE(names(), (QStringList{QStringLiteral("Michael")}));
        slotGrid->forceActiveFocus(Qt::TabFocusReason);
        slotGrid->setProperty("focusIndex", 0);
        f.key(Qt::Key_Delete);
        QCOMPARE(names(), QStringList());

        // The next free slot asks for someone: it focuses the search.
        f.click(f.item(QStringLiteral("profileFriendSlot_0")));
        QTRY_VERIFY(f.focusIn(QStringLiteral("profileFriendSearch")));
        f.type(QStringLiteral("ryan"));
        QTRY_COMPARE(f.item(QStringLiteral("profileFriendCandidates"))->property("count").toInt(), 1);
        f.key(Qt::Key_Return);
        QCOMPARE(names(), (QStringList{QStringLiteral("Ryan")}));
    }

    void songTabWaveformWindowListenAndMeter()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = writeSong(dir, QStringLiteral("tune.wav"), 52.0, 3.0);
        QVERIFY(!path.isEmpty());
        SongPlayer::setOutputFactoryForTesting(
            [](int channels, QString &) { return std::make_unique<SilentOutput>(channels); });

        EditorFixture f(QSize(1024, 768), false); // no song yet
        QVERIFY(f.ready());
        f.openTab(QStringLiteral("song"));
        ProfileController &profiles = f.profiles();
        QCOMPARE(f.item(QStringLiteral("profileChooseSongButton"))->property("label").toString(),
                 QStringLiteral("Choose a song file…"));

        // The file dialog's choice is imported: analysed, then cut.
        QObject *dialog = f.object(QStringLiteral("profileSongFileDialog"));
        QVERIFY(dialog);
        dialog->setProperty("selectedFile", QUrl::fromLocalFile(path));
        QMetaObject::invokeMethod(dialog, "accepted");
        QVERIFY(profiles.songImporting());
        // Listen waits for the cut: "Preparing…", disabled.
        QQuickItem *preparing = f.visibleItem(QStringLiteral("profileSongListenButton"));
        QTRY_VERIFY(preparing);
        QCOMPARE(preparing->property("label").toString(), QStringLiteral("Preparing…"));
        QVERIFY(!preparing->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(!profiles.songImporting() && f.draft().hasSong(), 20'000);
        QCOMPARE(f.text(QStringLiteral("profileSongFileName")), QStringLiteral("tune.wav"));
        QVERIFY(f.text(QStringLiteral("profileSongFileFormat")).startsWith(QStringLiteral("WAV · 0:52")));
        QCOMPARE(f.draft().songTitle(), QStringLiteral("Paper Planes"));

        // The waveform: 45 s of 52, the window's times over it.
        QQuickItem *wave = f.visibleItem(QStringLiteral("profileSongWaveform"));
        QVERIFY(wave);
        QCOMPARE(wave->property("durationMs").toReal(), qreal(profiles.songSource().value(QStringLiteral("durationMs")).toLongLong()));
        QCOMPARE(wave->property("windowMs").toLongLong(), profiles.songWindowMs());
        const auto clock = [](qint64 ms) {
            const qint64 seconds = (ms + 500) / 1000;
            return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
        };
        QCOMPARE(f.text(QStringLiteral("profileSongWindowText")),
                 clock(profiles.songWindowStartMs()) + QStringLiteral(" – ")
                     + clock(profiles.songWindowStartMs() + profiles.songWindowMs()));

        // Dragging the window moves it; it is cut again once it rests.
        const qint64 before = profiles.songWindowStartMs();
        f.scrollTo(wave);
        const QPoint grab = wave->mapToScene(QPointF(wave->property("windowX").toReal() + 10, wave->height() / 2)).toPoint();
        QTest::mousePress(f.window(), Qt::LeftButton, Qt::NoModifier, grab);
        QTest::mouseMove(f.window(), grab + QPoint(-40, 0));
        QTest::mouseRelease(f.window(), Qt::LeftButton, Qt::NoModifier, grab + QPoint(-40, 0));
        QVERIFY(profiles.songWindowStartMs() < before);
        QTRY_VERIFY_WITH_TIMEOUT(!profiles.songImporting(), 20'000);
        QCOMPARE(f.text(QStringLiteral("profileSongWindowText")),
                 clock(profiles.songWindowStartMs()) + QStringLiteral(" – ")
                     + clock(profiles.songWindowStartMs() + profiles.songWindowMs()));

        // Listen plays the draft's song on the page's own player.
        auto *player = qobject_cast<SongPlayer *>(f.object(QStringLiteral("profileSongPlayer")));
        QVERIFY(player);
        QTRY_COMPARE(player->songKey(), f.draft().songKey());
        QQuickItem *listen = f.visibleItem(QStringLiteral("profileSongListenButton"));
        QTRY_VERIFY(listen->isEnabled());
        QVERIFY(!player->playing());
        f.click(listen);
        QTRY_VERIFY(player->playing());
        QCOMPARE(listen->property("label").toString(), QStringLiteral("Pause"));
        f.click(listen);
        QTRY_VERIFY(!player->playing());

        // The meter: what the clip takes of the 224 KB.
        QCOMPARE(f.text(QStringLiteral("profileSongClipSize")),
                 QStringLiteral("%1 KB of 224 KB").arg(qRound(profiles.songClipBytes() / 1024.0)));
        QVERIFY(profiles.songClipBytes() > 0);

        // Remove song: the page has none, and the tab offers a file again.
        f.click(QStringLiteral("profileRemoveSong"));
        QVERIFY(!f.draft().hasSong());
        QCOMPARE(f.draft().songTitle(), QString());
        QCOMPARE(f.item(QStringLiteral("profileChooseSongButton"))->property("label").toString(),
                 QStringLiteral("Choose a song file…"));
        QVERIFY(!EditorFixture::isShown(f.item(QStringLiteral("profileSongWaveform"))));
    }

    void backgroundTabImportProgressAndModes()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = writePicture(dir, QStringLiteral("stage.png"), QSize(1200, 800));
        QVERIFY(!path.isEmpty());
        EditorFixture f(QSize(1024, 900), false);
        QVERIFY(f.ready());
        f.openTab(QStringLiteral("background"));
        ProfileController &profiles = f.profiles();
        QVERIFY(!f.draft().hasBackgroundImage());
        QCOMPARE(f.item(QStringLiteral("profileChoosePictureButton"))->property("label").toString(),
                 QStringLiteral("Choose picture…"));
        // A solid colour has nothing to fade to.
        f.click(f.segment(QStringLiteral("profileBackgroundStyle"), QStringLiteral("Solid")));
        QCOMPARE(f.draft().backgroundKind(), int(Profile::BackgroundKind::SolidBackground));
        QVERIFY(!f.item(QStringLiteral("profileFadeColourWell"))->isEnabled());
        f.click(f.segment(QStringLiteral("profileBackgroundStyle"), QStringLiteral("Gradient")));
        QVERIFY(f.item(QStringLiteral("profileFadeColourWell"))->isEnabled());
        // Picture with no picture yet asks for one (the file dialog) first.
        QObject *pictureDialog = f.object(QStringLiteral("profileBackgroundFileDialog"));
        QSignalSpy asked(pictureDialog, SIGNAL(visibleChanged()));
        f.click(f.segment(QStringLiteral("profileBackgroundStyle"), QStringLiteral("Picture")));
        QCOMPARE(f.draft().backgroundKind(), int(Profile::BackgroundKind::GradientBackground));
        QTRY_VERIFY(pictureDialog->property("visible").toBool());
        f.closeDialog(pictureDialog);

        QObject *dialog = f.object(QStringLiteral("profileBackgroundFileDialog"));
        QVERIFY(dialog);
        dialog->setProperty("selectedFile", QUrl::fromLocalFile(path));
        QMetaObject::invokeMethod(dialog, "accepted");
        // While it is re-encoded: the progress bar, and no Choose button.
        QVERIFY(profiles.backgroundImporting());
        QTRY_VERIFY(EditorFixture::isShown(f.item(QStringLiteral("profilePictureProgress"))));
        QVERIFY(!EditorFixture::isShown(f.item(QStringLiteral("profileChoosePictureButton"))));
        QTRY_VERIFY_WITH_TIMEOUT(!profiles.backgroundImporting(), 20'000);
        QVERIFY(f.draft().hasBackgroundImage());
        QCOMPARE(f.draft().backgroundKind(), int(Profile::BackgroundKind::ImageBackground));
        QVERIFY(!EditorFixture::isShown(f.item(QStringLiteral("profilePictureProgress"))));
        QQuickItem *thumbnail = f.item(QStringLiteral("profilePictureThumbnail"));
        QTRY_VERIFY(EditorFixture::isShown(thumbnail));
        QCOMPARE(thumbnail->property("imageKey").toString(), f.draft().backgroundImageKey());
        QCOMPARE(f.item(QStringLiteral("profileBackgroundStyle"))->property("currentIndex").toInt(),
                 int(Profile::BackgroundKind::ImageBackground));

        // Tile · Fill · Fit · Center, and whether it scrolls with the page.
        QVERIFY(EditorFixture::isShown(f.item(QStringLiteral("profilePictureMode"))));
        f.click(f.segment(QStringLiteral("profilePictureMode"), QStringLiteral("Fit")));
        QCOMPARE(f.draft().imageMode(), int(Profile::ImageMode::FitImage));
        f.click(f.segment(QStringLiteral("profilePictureMode"), QStringLiteral("Tile")));
        QCOMPARE(f.draft().imageMode(), int(Profile::ImageMode::TileImage));
        QVERIFY(f.draft().imageFixed());
        f.click(QStringLiteral("profilePictureFixed"));
        QVERIFY(!f.draft().imageFixed());

        // Another style keeps the picture, and Picture brings it back.
        f.click(f.segment(QStringLiteral("profileBackgroundStyle"), QStringLiteral("Gradient")));
        QCOMPARE(f.draft().backgroundKind(), int(Profile::BackgroundKind::GradientBackground));
        QVERIFY(f.draft().hasBackgroundImage());
        QVERIFY(!EditorFixture::isShown(f.item(QStringLiteral("profilePictureMode"))));
        f.click(f.segment(QStringLiteral("profileBackgroundStyle"), QStringLiteral("Picture")));
        QCOMPARE(f.draft().backgroundKind(), int(Profile::BackgroundKind::ImageBackground));

        // Remove picture.
        f.click(QStringLiteral("profileRemovePicture"));
        QVERIFY(!f.draft().hasBackgroundImage());
        QCOMPARE(f.draft().backgroundKind(), int(Profile::BackgroundKind::SolidBackground));
        QVERIFY(!EditorFixture::isShown(f.item(QStringLiteral("profilePictureThumbnail"))));
    }

    void boxesTabBordersCornersAndStrips()
    {
        EditorFixture f(QSize(1024, 900));
        QVERIFY(f.ready());
        f.openTab(QStringLiteral("boxes"));
        ProfileController &profiles = f.profiles();

        // Double needs 3 px or more: the hint says so below that.
        QQuickItem *hint = f.item(QStringLiteral("profileDoubleBorderHint"));
        f.click(f.segment(QStringLiteral("profileBorderWidth"), QStringLiteral("1")));
        f.click(f.segment(QStringLiteral("profileBorderStyle"), QStringLiteral("Double")));
        QCOMPARE(f.draft().borderStyle(), int(Profile::BorderStyle::DoubleBorder));
        QTRY_VERIFY(EditorFixture::isShown(hint));
        f.click(f.segment(QStringLiteral("profileBorderWidth"), QStringLiteral("3")));
        QCOMPARE(f.draft().borderWidth(), 3);
        QTRY_VERIFY(!EditorFixture::isShown(hint));

        // Corners, the neon edge and the table style.
        f.click(f.item(QStringLiteral("profileCorner_10")));
        QCOMPARE(f.draft().boxRadius(), int(Profile::BoxRadius::RoundCorners));
        f.click(f.item(QStringLiteral("profileCorner_0")));
        QCOMPARE(f.draft().boxRadius(), int(Profile::BoxRadius::SquareCorners));
        const bool glow = f.draft().boxGlow();
        f.click(QStringLiteral("profileNeonEdge"));
        QCOMPARE(f.draft().boxGlow(), !glow);
        f.click(f.segment(QStringLiteral("profileTableStyle"), QStringLiteral("Lines")));
        QCOMPARE(f.draft().tableStyle(), int(Profile::TableStyle::LineTable));

        // Strip styles, drawn in the strip's own colour.
        f.click(f.item(QStringLiteral("profileStripTile_%1").arg(int(Profile::HeaderStyle::NoHeader))));
        QCOMPARE(f.draft().headerStyle(), int(Profile::HeaderStyle::NoHeader));
        f.click(f.item(QStringLiteral("profileStripTile_%1").arg(int(Profile::HeaderStyle::FlatHeader))));
        QCOMPARE(f.draft().headerStyle(), int(Profile::HeaderStyle::FlatHeader));

        // A different strip for the right column brings its three wells.
        QQuickItem *altFill = f.item(QStringLiteral("profileAltStripColourWell"));
        if (f.draft().altHeader())
            f.click(QStringLiteral("profileAltHeader"));
        QVERIFY(!EditorFixture::isShown(altFill));
        f.click(QStringLiteral("profileAltHeader"));
        QVERIFY(f.draft().altHeader());
        QTRY_VERIFY(EditorFixture::isShown(altFill));
        QVERIFY(EditorFixture::isShown(f.item(QStringLiteral("profileAltStripTextWell"))));
        QVERIFY(EditorFixture::isShown(f.item(QStringLiteral("profileAltBorderColourWell"))));
        QCOMPARE(f.item(QStringLiteral("profileAltStripTextWell"))->property("inkRole").toInt(),
                 int(Profile::InkRole::AltHeaderTextInk));

        // See-through runs 0–40%, i.e. opacity 100–60; a drag is one step.
        QQuickItem *slider = f.item(QStringLiteral("profileSeeThrough"));
        f.settle(slider);
        const QPoint left = slider->mapToScene(QPointF(9, slider->height() / 2)).toPoint();
        const QPoint right = slider->mapToScene(QPointF(slider->width() - 9, slider->height() / 2)).toPoint();
        while (profiles.canUndo())
            profiles.undo();
        const int opacity = f.draft().boxOpacity();
        QTest::mousePress(f.window(), Qt::LeftButton, Qt::NoModifier, left);
        QCOMPARE(f.draft().boxOpacity(), 100);
        for (int step = 1; step <= 5; ++step)
            QTest::mouseMove(f.window(), left + (right - left) * step / 5);
        QTest::mouseRelease(f.window(), Qt::LeftButton, Qt::NoModifier, right);
        QCOMPARE(f.draft().boxOpacity(), 60);
        QCOMPARE(f.text(QStringLiteral("profileSeeThroughValue")), QStringLiteral("40%"));
        QTRY_VERIFY_WITH_TIMEOUT(profiles.canUndo(), 2000);
        QTest::qWait(500); // the drag's step closes once the knob rests
        profiles.undo();
        QCOMPARE(f.draft().boxOpacity(), opacity);
        QVERIFY(!profiles.canUndo());
    }

    void aboutTabPictureMoodDetailsAndNameHint()
    {
        EditorFixture f(QSize(1024, 900));
        QVERIFY(f.ready());
        f.openTab(QStringLiteral("about"));

        // Change picture… is the page's own picture dialog.
        QObject *dialog = f.object(QStringLiteral("localAvatarFileDialog"));
        QVERIFY(dialog);
        f.click(QStringLiteral("profileChangePictureButton"));
        QTRY_VERIFY(dialog->property("visible").toBool());
        f.closeDialog(dialog);

        // The name is page-only, and says so.
        QQuickItem *name = f.item(QStringLiteral("profileField_displayName"));
        bool hinted = false;
        for (QQuickItem *child : name->parentItem()->childItems()) {
            if (child->property("text").toString().startsWith(QStringLiteral("Shown on your profile page")))
                hinted = EditorFixture::isShown(child);
        }
        QVERIFY(hinted);

        // Mood: the painted face and the word.
        QQuickItem *mood = f.item(QStringLiteral("profileMoodButton"));
        QCOMPARE(mood->property("text").toString(), QStringLiteral("rockin'"));

        // Details: the fold counts what is filled, Here for picks several.
        QQuickItem *details = f.item(QStringLiteral("profileDetailsFold"));
        QCOMPARE(details->property("note").toString(), QStringLiteral("%1 of 6 filled").arg(f.draft().filledDetailCount()));
        QVERIFY(!details->property("expanded").toBool());
        f.click(details, QPointF(40, 17));
        QTRY_VERIFY(details->property("expanded").toBool());
        f.draft().setHereFor(0);
        f.click(QStringLiteral("profileHereForButton"));
        QTRY_VERIFY(f.isOpen(QStringLiteral("profileHereForMenu")));
        f.click(f.visibleItem(QStringLiteral("profileHereFor_%1").arg(int(Profile::HereForMusic))));
        QTRY_VERIFY(!f.isOpen(QStringLiteral("profileHereForMenu")));
        f.click(QStringLiteral("profileHereForButton"));
        QTRY_VERIFY(f.isOpen(QStringLiteral("profileHereForMenu")));
        f.click(f.visibleItem(QStringLiteral("profileHereFor_%1").arg(int(Profile::HereForFriends))));
        QCOMPARE(f.draft().hereFor(), int(Profile::HereForMusic | Profile::HereForFriends));
        QTRY_COMPARE(f.item(QStringLiteral("profileHereForButton"))->property("text").toString(),
                     QStringLiteral("Friends, Music"));
        // Zodiac.
        f.click(QStringLiteral("profileZodiacButton"));
        QTRY_VERIFY(f.isOpen(QStringLiteral("profileZodiacMenu")));
        f.click(f.visibleItem(QStringLiteral("profileZodiac_%1").arg(int(Profile::Zodiac::Aries))));
        QCOMPARE(f.draft().zodiac(), int(Profile::Zodiac::Aries));
        QTRY_COMPARE(f.item(QStringLiteral("profileZodiacButton"))->property("text").toString(), QStringLiteral("Aries"));
        // A detail field edits its own field.
        f.focusField(QStringLiteral("profileField_hometown"));
        f.type(QStringLiteral("Olympia"));
        QCOMPARE(f.draft().hometown(), QStringLiteral("Olympia"));
        QCOMPARE(f.frame()->property("editingTarget").toString(), QStringLiteral("details"));
    }

    // --- Save, discard, leaving ----------------------------------------------

    void saveIsDisabledUntilDirtyAndPublishes()
    {
        EditorFixture f;
        QVERIFY(f.ready());
        ProfileController &profiles = f.profiles();
        QQuickItem *save = f.item(QStringLiteral("profileSaveButton"));
        QVERIFY(!save->isEnabled());
        QVERIFY(!EditorFixture::isShown(f.item(QStringLiteral("profileUnsavedPill"))));

        f.draft().setHeadline(QStringLiteral("New EP out now"));
        QVERIFY(profiles.draftDirty());
        QTRY_VERIFY(save->isEnabled());
        QVERIFY(EditorFixture::isShown(f.item(QStringLiteral("profileUnsavedPill"))));

        QSignalSpy published(&profiles, &ProfileController::published);
        const qint64 revision = profiles.publishedRevision();
        f.click(save);
        QCOMPARE(published.count(), 1);
        QVERIFY(profiles.publishedRevision() > revision);
        QVERIFY(!profiles.editing());
        QCOMPARE(profiles.view()->headline(), QStringLiteral("New EP out now"));
        // The editor goes back to the page.
        QTRY_COMPARE(f.editor(), nullptr);
    }

    void saveWaitsForEncoding()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = writePicture(dir, QStringLiteral("big.png"), QSize(1600, 1000));
        EditorFixture f;
        QVERIFY(f.ready());
        ProfileController &profiles = f.profiles();
        QSignalSpy published(&profiles, &ProfileController::published);
        f.draft().setHeadline(QStringLiteral("With a picture"));
        profiles.importBackground(QUrl::fromLocalFile(path));
        QVERIFY(profiles.backgroundImporting());

        QQuickItem *save = f.item(QStringLiteral("profileSaveButton"));
        f.click(save);
        // "Saving…" inside the button until the picture is in, then it saves.
        QVERIFY(profiles.publishPending());
        QCOMPARE(save->property("label").toString(), QStringLiteral("Saving…"));
        QVERIFY(!save->isEnabled());
        QVERIFY(profiles.editing());
        QCOMPARE(published.count(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(published.count(), 1, 20'000);
        QVERIFY(!profiles.editing());
        QVERIFY(profiles.view()->hasBackgroundImage());
    }

    void discardPopoverRestoresPublished()
    {
        EditorFixture f;
        QVERIFY(f.ready());
        ProfileController &profiles = f.profiles();
        QQuickItem *discard = f.item(QStringLiteral("profileDiscardButton"));
        QVERIFY(!discard->isEnabled()); // nothing to discard
        const QString published = f.draft().headline();
        f.draft().setHeadline(QStringLiteral("Draft words"));
        QTRY_VERIFY(discard->isEnabled());

        // Keep editing: nothing changes.
        f.click(discard);
        QTRY_VERIFY(f.isOpen(QStringLiteral("profileDiscardPopover")));
        QVERIFY(f.editor()->property("popupOpen").toBool());
        QVERIFY(f.focusIn(QStringLiteral("profileDiscardKeepButton")));
        f.click(QStringLiteral("profileDiscardKeepButton"));
        QTRY_VERIFY(!f.isOpen(QStringLiteral("profileDiscardPopover")));
        QCOMPARE(f.draft().headline(), QStringLiteral("Draft words"));

        // Discard: back to the page as last saved, still in the editor.
        f.click(discard);
        QTRY_VERIFY(f.isOpen(QStringLiteral("profileDiscardPopover")));
        f.click(QStringLiteral("profileDiscardConfirmButton"));
        QTRY_VERIFY(!f.isOpen(QStringLiteral("profileDiscardPopover")));
        QCOMPARE(f.draft().headline(), published);
        QVERIFY(!profiles.draftDirty());
        QVERIFY(profiles.editing());
    }

    void leaveDialogOnBackEscapeAndWindowClose()
    {
        EditorFixture f;
        QVERIFY(f.ready());
        ProfileController &profiles = f.profiles();

        // No changes: Back leaves at once.
        f.click(QStringLiteral("profileEditorBackButton"));
        QVERIFY(!profiles.editing());
        QTRY_COMPARE(f.editor(), nullptr);

        QVERIFY(profiles.beginEditing());
        QVERIFY(f.waitForEditor());
        f.draft().setHeadline(QStringLiteral("Unsaved words"));

        // Back with changes asks. Keep editing has focus; Enter means it.
        f.click(QStringLiteral("profileEditorBackButton"));
        QTRY_VERIFY(f.isOpen(QStringLiteral("profileLeaveDialog")));
        QVERIFY(f.focusIn(QStringLiteral("profileLeaveKeepButton")));
        f.key(Qt::Key_Return);
        QTRY_VERIFY(!f.isOpen(QStringLiteral("profileLeaveDialog")));
        QVERIFY(profiles.editing());
        QCOMPARE(f.draft().headline(), QStringLiteral("Unsaved words"));

        // Esc asks too; Esc in the dialog means Keep editing.
        f.editor()->forceActiveFocus();
        f.key(Qt::Key_Escape);
        QTRY_VERIFY(f.isOpen(QStringLiteral("profileLeaveDialog")));
        f.key(Qt::Key_Escape);
        QTRY_VERIFY(!f.isOpen(QStringLiteral("profileLeaveDialog")));
        QVERIFY(profiles.editing());

        // Enter means Keep editing even on Discard.
        f.key(Qt::Key_Escape);
        QTRY_VERIFY(f.isOpen(QStringLiteral("profileLeaveDialog")));
        f.item(QStringLiteral("profileLeaveDiscardButton"))->forceActiveFocus(Qt::TabFocusReason);
        f.key(Qt::Key_Return);
        QTRY_VERIFY(!f.isOpen(QStringLiteral("profileLeaveDialog")));
        QVERIFY(profiles.editing());
        QCOMPARE(f.draft().headline(), QStringLiteral("Unsaved words"));

        // Closing the window asks; Discard lets it close with the page as saved.
        QMetaObject::invokeMethod(f.window(), "closeWindow");
        QTRY_VERIFY(f.isOpen(QStringLiteral("profileLeaveDialog")));
        QCOMPARE(f.window()->property("closeOutcome").toString(), QString());
        f.click(QStringLiteral("profileLeaveDiscardButton"));
        QTRY_VERIFY(!f.isOpen(QStringLiteral("profileLeaveDialog")));
        QCOMPARE(f.window()->property("closeOutcome").toString(), QStringLiteral("closed"));
        QVERIFY(!profiles.draftDirty());

        // The Back chip's history menu leaves through the dialog too.
        {
            Profile::Page jessica = *Reference::seededPage(QStringLiteral("jessica"));
            jessica.topFriends.prepend({Reference::mockAccountFor(Reference::selfId()).bytes(), QStringLiteral("Daniel")});
            profiles.setMockPage(QStringLiteral("jessica"), jessica);
            profiles.endEditing();
            QVERIFY(profiles.openContact(QStringLiteral("jessica")));
            QVERIFY(profiles.openTopFriend(0)); // yourself, from her Friend Space
            QVERIFY(profiles.isOwnProfile());
            QCOMPARE(profiles.depth(), 2);
            QVERIFY(profiles.beginEditing());
            QVERIFY(f.waitForEditor());
            f.draft().setHeadline(QStringLiteral("Unsaved words"));
            f.click(f.item(QStringLiteral("profileEditorBackButton")), QPointF(10, 10), Qt::RightButton);
            QTRY_VERIFY(f.isOpen(QStringLiteral("profileEditorHistoryMenu")));
            QVERIFY(f.editor()->property("popupOpen").toBool());
            QQuickItem *entry = nullptr;
            QTRY_VERIFY((entry = f.visibleItem(QStringLiteral("profileEditorHistory_0"))));
            f.click(entry);
            QTRY_VERIFY(f.isOpen(QStringLiteral("profileLeaveDialog")));
            QCOMPARE(profiles.depth(), 2);
            f.click(QStringLiteral("profileLeaveDiscardButton"));
            QCOMPARE(profiles.depth(), 1);
            QVERIFY(!profiles.editing());
            QCOMPARE(profiles.personId(), QStringLiteral("jessica"));
            QTRY_COMPARE(f.editor(), nullptr);
            profiles.openOwn();
            QVERIFY(profiles.beginEditing());
            QVERIFY(f.waitForEditor());
        }

        // Save saves, then leaves.
        f.draft().setHeadline(QStringLiteral("Saved words"));
        QSignalSpy published(&profiles, &ProfileController::published);
        f.editor()->forceActiveFocus();
        f.key(Qt::Key_Escape);
        QTRY_VERIFY(f.isOpen(QStringLiteral("profileLeaveDialog")));
        f.click(QStringLiteral("profileLeaveSaveButton"));
        QCOMPARE(published.count(), 1);
        QVERIFY(!profiles.editing());
        QCOMPARE(profiles.view()->headline(), QStringLiteral("Saved words"));
    }

    void survivingDraftNotice()
    {
        EditorFixture f;
        QVERIFY(f.ready());
        ProfileController &profiles = f.profiles();
        const QString published = f.draft().headline();
        QVERIFY(!EditorFixture::isShown(f.item(QStringLiteral("profileSurvivingDraftNotice"))));

        f.draft().setHeadline(QStringLiteral("Left for later"));
        profiles.endEditing(); // the draft is kept
        QTRY_COMPARE(f.editor(), nullptr);
        QVERIFY(profiles.beginEditing());
        QVERIFY(f.waitForEditor());
        QVERIFY(profiles.survivingDraftAtMs() > 0);
        QQuickItem *notice = f.item(QStringLiteral("profileSurvivingDraftNotice"));
        QVERIFY(EditorFixture::isShown(notice));
        const QString time = QLocale().toString(QDateTime::fromMSecsSinceEpoch(profiles.survivingDraftAtMs()).time(),
                                                QLocale::ShortFormat);
        QCOMPARE(f.text(QStringLiteral("profileSurvivingDraftText")),
                 QStringLiteral("You have unsaved changes from ") + time + QLatin1Char('.'));
        QCOMPARE(f.draft().headline(), QStringLiteral("Left for later"));

        // Continue: the notice goes, the changes stay.
        f.click(QStringLiteral("profileContinueDraftButton"));
        QTRY_VERIFY(!EditorFixture::isShown(notice));
        QCOMPARE(f.draft().headline(), QStringLiteral("Left for later"));

        // Start over: back to the page as saved.
        profiles.endEditing();
        QTRY_COMPARE(f.editor(), nullptr);
        QVERIFY(profiles.beginEditing());
        QVERIFY(f.waitForEditor());
        QTRY_VERIFY(EditorFixture::isShown(f.item(QStringLiteral("profileSurvivingDraftNotice"))));
        f.click(QStringLiteral("profileStartOverButton"));
        QTRY_VERIFY(!EditorFixture::isShown(f.item(QStringLiteral("profileSurvivingDraftNotice"))));
        QCOMPARE(f.draft().headline(), published);
        QVERIFY(!profiles.draftDirty());
    }

    // --- History, preview, keyboard ------------------------------------------

    void undoRedoShortcutsAndChips()
    {
        EditorFixture f;
        QVERIFY(f.ready());
        ProfileController &profiles = f.profiles();
        QQuickItem *undo = f.item(QStringLiteral("profileUndoButton"));
        QQuickItem *redo = f.item(QStringLiteral("profileRedoButton"));
        QVERIFY(!undo->isEnabled());
        QVERIFY(!redo->isEnabled());
        QCOMPARE(undo->opacity(), 0.55);

        f.openTab(QStringLiteral("boxes"));
        const int width = f.draft().borderWidth();
        f.click(f.segment(QStringLiteral("profileBorderWidth"), QStringLiteral("4")));
        QCOMPARE(f.draft().borderWidth(), 4);
        QTRY_VERIFY(undo->isEnabled());

        f.item(QStringLiteral("profileEditorRail"))->forceActiveFocus();
        f.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(f.draft().borderWidth(), width);
        QTRY_VERIFY(redo->isEnabled());
        f.key(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(f.draft().borderWidth(), 4);
        f.key(Qt::Key_Z, Qt::ControlModifier);
        f.key(Qt::Key_Y, Qt::ControlModifier);
        QCOMPARE(f.draft().borderWidth(), 4);
        f.click(undo);
        QCOMPARE(f.draft().borderWidth(), width);
        f.click(redo);
        QCOMPARE(f.draft().borderWidth(), 4);

        // Inside a text field Ctrl+Z is the field's own; the field's focus
        // period lands as one step when focus leaves.
        f.openTab(QStringLiteral("about"));
        const QString headline = f.draft().headline();
        f.focusField(QStringLiteral("profileField_headline"));
        f.key(Qt::Key_End);
        f.type(QStringLiteral("!!"));
        QCOMPARE(f.draft().headline(), headline + QStringLiteral("!!"));
        f.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(f.draft().borderWidth(), 4); // the history did not move
        QVERIFY(f.draft().headline().size() < headline.size() + 2);
        f.type(QStringLiteral("??"));
        const QString typed = f.draft().headline();
        QVERIFY(typed.endsWith(QStringLiteral("??")));
        f.item(QStringLiteral("profileEditorRail"))->forceActiveFocus(); // focus leaves
        f.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(f.draft().headline(), headline);
        QCOMPARE(f.draft().borderWidth(), 4);
        f.key(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(f.draft().headline(), typed);
    }

    void previewToggleHidesRailAndPanel()
    {
        EditorFixture f;
        QVERIFY(f.ready());
        QQuickItem *toggle = f.item(QStringLiteral("profilePreviewToggle"));
        QQuickItem *rail = f.item(QStringLiteral("profileEditorRail"));
        QCOMPARE(f.frame()->x(), 376.0);

        f.click(toggle);
        QVERIFY(f.editor()->property("previewOnly").toBool());
        QVERIFY(toggle->property("checked").toBool());
        // The preview takes the width at once; the rail and panel slide away.
        QCOMPARE(f.frame()->x(), 0.0);
        QCOMPARE(f.frame()->width(), qreal(f.window()->width()));
        QTRY_VERIFY(!rail->isVisible());
        QVERIFY(!f.item(QStringLiteral("profileEditorPanel"))->parentItem()->isVisible());
        // Save, Discard and Undo stay in the bar.
        QVERIFY(EditorFixture::isShown(f.item(QStringLiteral("profileSaveButton"))));
        QVERIFY(EditorFixture::isShown(f.item(QStringLiteral("profileUndoButton"))));

        f.click(toggle);
        QVERIFY(!f.editor()->property("previewOnly").toBool());
        QCOMPARE(f.frame()->x(), 376.0);
        QTRY_VERIFY(EditorFixture::isShown(rail));
        QTRY_COMPARE(rail->x(), 0.0);

        // Asking to edit a box from the full-width preview brings the panel back.
        f.click(toggle);
        QVERIFY(f.editor()->property("previewOnly").toBool());
        QMetaObject::invokeMethod(f.frame(), "editRequested", Q_ARG(QString, QStringLiteral("song")));
        QVERIFY(!f.editor()->property("previewOnly").toBool());
        QCOMPARE(f.profiles().lastTab(), int(EditorTab::SongTab));
    }

    void previewCaptionFollowsWidth()
    {
        const QString two = QStringLiteral("Live preview: what your contacts will see after you save");
        {
            EditorFixture f(QSize(1024, 768)); // a 648 px preview
            QVERIFY(f.ready());
            QCOMPARE(f.text(QStringLiteral("profilePreviewCaptionText")), two);
            QVERIFY(EditorFixture::isShown(f.item(QStringLiteral("profilePreviewHint"))));
            QCOMPARE(f.text(QStringLiteral("profilePreviewHint")), QStringLiteral("Click any box to edit it"));
        }
        {
            EditorFixture f(QSize(860, 680)); // 484 px
            QVERIFY(f.ready());
            QCOMPARE(f.frame()->width(), 484.0);
            QCOMPARE(f.text(QStringLiteral("profilePreviewCaptionText")),
                     QStringLiteral("Live preview, one column here: contacts with a wider window see two"));
            QVERIFY(!EditorFixture::isShown(f.item(QStringLiteral("profilePreviewHint"))));
            // Preview: the full width holds two columns.
            f.click(QStringLiteral("profilePreviewToggle"));
            QCOMPARE(f.text(QStringLiteral("profilePreviewCaptionText")), two);
            QVERIFY(EditorFixture::isShown(f.item(QStringLiteral("profilePreviewHint"))));
        }
        {
            EditorFixture f(QSize(720, 560)); // 344 px
            QVERIFY(f.ready());
            QCOMPARE(f.text(QStringLiteral("profilePreviewCaptionText")),
                     QStringLiteral("One column here; wider windows show two"));
            // A page laid out in one column is one column for everyone.
            f.draft().setLayout(int(Profile::Layout::SingleLayout));
            QCOMPARE(f.text(QStringLiteral("profilePreviewCaptionText")), two);
        }
    }

    void clickingAModuleOpensItsTabAndField()
    {
        EditorFixture f(QSize(1024, 900));
        QVERIFY(f.ready());
        struct Row {
            const char *target;
            EditorTab tab;
            const char *field; // what gets focus ("" = somewhere in the tab)
        };
        const Row rows[] = {
            {"name", EditorTab::NameTab, "profileNameFontGrid"},
            {"photo", EditorTab::AboutTab, "profileChangePictureButton"},
            {"headline", EditorTab::AboutTab, "profileField_headline"},
            {"info", EditorTab::AboutTab, "profileField_infoLine1"},
            {"mood", EditorTab::AboutTab, "profileMoodButton"},
            {"aboutMe", EditorTab::AboutTab, "profileField_aboutMe"},
            {"meet", EditorTab::AboutTab, "profileField_meet"},
            {"interests", EditorTab::AboutTab, "profileField_interestGeneral"},
            {"details", EditorTab::AboutTab, "profileHereForButton"},
            {"song", EditorTab::SongTab, "profileChooseSongButton"},
            {"friends", EditorTab::FriendsTab, "profileFriendSlots"},
            {"strip", EditorTab::BoxesTab, "profileStripGrid"},
            {"backdrop", EditorTab::BackgroundTab, "profileBackgroundStyle"},
        };
        for (const Row &row : rows) {
            f.profiles().setLastTab(int(EditorTab::LayoutTab));
            QMetaObject::invokeMethod(f.frame(), "editRequested", Q_ARG(QString, QString::fromLatin1(row.target)));
            QCOMPARE(f.profiles().lastTab(), int(row.tab));
            QTRY_VERIFY2(f.focusIn(QString::fromLatin1(row.field)), row.target);
        }
        // A focused About me field marks its box in the preview ("Editing").
        QMetaObject::invokeMethod(f.frame(), "editRequested", Q_ARG(QString, QStringLiteral("headline")));
        QTRY_COMPARE(f.frame()->property("editingTarget").toString(), QStringLiteral("headline"));
        QMetaObject::invokeMethod(f.frame(), "editRequested", Q_ARG(QString, QStringLiteral("interests")));
        QTRY_COMPARE(f.frame()->property("editingTarget").toString(), QStringLiteral("interests"));
        QVERIFY(f.item(QStringLiteral("profileInterestsFold"))->property("expanded").toBool());

        // The page kit's view gets the draft in preview mode and reports
        // clicks on its boxes through editRequested.
        QQuickItem *view = f.frame()->property("view").value<QQuickItem *>();
        QVERIFY(view);
        if (view->property("page").isValid()) {
            QCOMPARE(view->property("page").value<QObject *>(), f.profiles().draft());
            QCOMPARE(view->property("mode").toString(), QStringLiteral("preview"));
            QCOMPARE(view->property("editingTarget").toString(), QStringLiteral("interests"));
            QMetaObject::invokeMethod(view, "editRequested", Q_ARG(QString, QStringLiteral("song")));
            QCOMPARE(f.profiles().lastTab(), int(EditorTab::SongTab));
        } else {
            qInfo("The page kit's ProfilePageView has not landed: its bindings are checked once it has.");
        }
    }

    void appOwnedBoxesAreInert()
    {
        EditorFixture f;
        QVERIFY(f.ready());
        f.openTab(QStringLiteral("text"));
        QQuickItem *before = f.focused();
        for (const char *target : {"contacting", "banner", "handle", "status"}) {
            QMetaObject::invokeMethod(f.frame(), "editRequested", Q_ARG(QString, QString::fromLatin1(target)));
            QCOMPARE(f.profiles().lastTab(), int(EditorTab::TextTab));
            QCOMPARE(f.focused(), before);
            QVERIFY(!f.profiles().draftDirty());
        }
        // Clicking the page's own Contacting box in the preview does nothing.
        if (QQuickItem *contacting = f.visibleItem(QStringLiteral("profileContactingBox"))) {
            f.click(contacting);
            QTest::qWait(50);
            QCOMPARE(f.profiles().lastTab(), int(EditorTab::TextTab));
            QVERIFY(f.profiles().editing());
            QVERIFY(!f.profiles().draftDirty());
        } else {
            qInfo("The page kit's Contacting box has not landed: clicking it is checked once it has.");
        }
    }

    void escapeWithAMenuOpenClosesOnlyTheMenu()
    {
        EditorFixture f;
        QVERIFY(f.ready());
        f.openTab(QStringLiteral("about"));
        f.draft().setHeadline(QStringLiteral("Unsaved"));

        f.click(QStringLiteral("profileMoodButton"));
        QTRY_VERIFY(f.isOpen(QStringLiteral("profileMoodMenu")));
        QVERIFY(f.editor()->property("popupOpen").toBool());
        f.key(Qt::Key_Escape);
        QTRY_VERIFY(!f.isOpen(QStringLiteral("profileMoodMenu")));
        QVERIFY(!f.isOpen(QStringLiteral("profileLeaveDialog")));
        QVERIFY(f.profiles().editing());

        // A mood from the menu.
        f.click(QStringLiteral("profileMoodButton"));
        QTRY_VERIFY(f.isOpen(QStringLiteral("profileMoodMenu")));
        const int mood = int(Profile::Mood::MoodBouncy);
        f.click(f.visibleItem(QStringLiteral("profileMood_%1").arg(mood)));
        QTRY_VERIFY(!f.isOpen(QStringLiteral("profileMoodMenu")));
        QCOMPARE(f.draft().mood(), mood);

        // The colour picker: Esc closes it (putting back "was"), nothing else.
        f.openTab(QStringLiteral("background"));
        const QColor base = f.draft().backgroundColor1();
        f.click(QStringLiteral("profileBaseColourWell"));
        QTRY_VERIFY(f.isOpen(QStringLiteral("profileColorPicker")));
        f.click(QStringLiteral("profileClassicSwatch_ffd400"));
        f.key(Qt::Key_Escape);
        QTRY_VERIFY(!f.isOpen(QStringLiteral("profileColorPicker")));
        QCOMPARE(f.draft().backgroundColor1(), base);
        QVERIFY(!f.isOpen(QStringLiteral("profileLeaveDialog")));

        // Esc in a text field puts back what it held when it took focus
        // (one Esc), and only then goes on to the page.
        f.openTab(QStringLiteral("about"));
        f.focusField(QStringLiteral("profileField_headline"));
        f.key(Qt::Key_End);
        f.type(QStringLiteral(" and more"));
        QCOMPARE(f.draft().headline(), QStringLiteral("Unsaved and more"));
        f.key(Qt::Key_Escape);
        QCOMPARE(f.draft().headline(), QStringLiteral("Unsaved"));
        QCOMPARE(f.text(f.input(QStringLiteral("profileField_headline"))), QStringLiteral("Unsaved"));
        QVERIFY(!f.isOpen(QStringLiteral("profileLeaveDialog")));
        f.key(Qt::Key_Escape);
        QTRY_VERIFY(f.isOpen(QStringLiteral("profileLeaveDialog")));
        f.key(Qt::Key_Escape);
        QTRY_VERIFY(!f.isOpen(QStringLiteral("profileLeaveDialog")));

        // With nothing open, Esc asks about the unsaved changes.
        f.editor()->forceActiveFocus();
        f.key(Qt::Key_Escape);
        QTRY_VERIFY(f.isOpen(QStringLiteral("profileLeaveDialog")));
        f.key(Qt::Key_Escape);
        QTRY_VERIFY(!f.isOpen(QStringLiteral("profileLeaveDialog")));
    }

    void editorFitsTheMinimumWindow()
    {
        EditorFixture f(QSize(720, 560));
        QVERIFY(f.ready());
        f.draft().setHeadline(QStringLiteral("Unsaved"));
        QTest::qWait(20);
        // All nine tabs fit: clamp(floor((railH − 12 − 44) / 9), 44, 52).
        const qreal railHeight = f.item(QStringLiteral("profileEditorRail"))->height();
        QCOMPARE(railHeight, 560.0 - 48.0);
        for (const char *name : {"themes", "background", "boxes", "text", "name", "about", "friends", "song", "layout"}) {
            QQuickItem *tab = f.item(QStringLiteral("profileEditorTab_") + QString::fromLatin1(name));
            QVERIFY2(tab->mapToScene(QPointF(0, tab->height())).y() <= 560, name);
            QCOMPARE(tab->height(), std::clamp(std::floor((railHeight - 12 - 44) / 9), 44.0, 52.0));
        }
        // With a CallStrip under the bar too, the tabs shrink and still fit.
        f.editor()->setHeight(560 - 48 - 40);
        for (const char *name : {"themes", "layout"}) {
            QQuickItem *tab = f.item(QStringLiteral("profileEditorTab_") + QString::fromLatin1(name));
            QTRY_COMPARE(tab->height(), std::floor((560.0 - 48 - 40 - 12 - 44) / 9));
            // (the rail's columns lay out on the next polish)
            QTRY_VERIFY2(tab->mapToItem(f.editor(), QPointF(0, tab->height())).y() <= f.editor()->height(), name);
        }
        f.editor()->setHeight(560 - 48);
        // The panel keeps its 300 px; the preview gets the rest.
        QCOMPARE(f.item(QStringLiteral("profileEditorPanel"))->width(), 300.0);
        QCOMPARE(f.frame()->width(), 344.0);
        // The bar's two sides do not meet, and Save is inside the window.
        QQuickItem *save = f.item(QStringLiteral("profileSaveButton"));
        QVERIFY(save->mapToScene(QPointF(save->width(), 0)).x() <= 720 - 12 + 0.5);
        QQuickItem *pill = f.item(QStringLiteral("profileUnsavedPill"));
        QQuickItem *undo = f.item(QStringLiteral("profileUndoButton"));
        QVERIFY(EditorFixture::isShown(pill));
        QVERIFY(pill->mapToScene(QPointF(pill->width(), 0)).x() + 8 <= undo->mapToScene(QPointF(0, 0)).x());
    }

    void compactBarBelow900()
    {
        {
            EditorFixture f(QSize(860, 680));
            QVERIFY(f.ready());
            f.draft().setHeadline(QStringLiteral("Unsaved"));
            QQuickItem *pill = f.item(QStringLiteral("profileUnsavedPill"));
            QTRY_VERIFY(EditorFixture::isShown(pill));
            QVERIFY(f.bar()->property("compact").toBool());
            // The pill is its dot; the words move to its tooltip.
            QCOMPARE(pill->width(), 22.0);
            QVERIFY(!EditorFixture::isShown(f.item(QStringLiteral("profileUnsavedPillText"))));
            QObject *tip = f.object(QStringLiteral("profileUnsavedPillTip"));
            QVERIFY(tip);
            QCOMPARE(tip->property("title").toString(), QStringLiteral("Unsaved changes"));
            QCOMPARE(tip->property("text").toString(), QStringLiteral("Only you can see these changes until you save."));
            f.hover(pill);
            QTRY_VERIFY(tip->property("visible").toBool());
            // Preview drops its label.
            QQuickItem *preview = f.item(QStringLiteral("profilePreviewToggle"));
            QCOMPARE(preview->width(), 32.0);
            QCOMPARE(preview->property("label").toString(), QString());
        }
        EditorFixture wide(QSize(1024, 768));
        QVERIFY(wide.ready());
        wide.draft().setHeadline(QStringLiteral("Unsaved"));
        QTRY_VERIFY(EditorFixture::isShown(wide.item(QStringLiteral("profileUnsavedPillText"))));
        QVERIFY(!wide.bar()->property("compact").toBool());
        QVERIFY(wide.item(QStringLiteral("profileUnsavedPill"))->width() > 22.0);
        QCOMPARE(wide.text(QStringLiteral("profileUnsavedPillText")), QStringLiteral("Unsaved changes"));
        QCOMPARE(wide.item(QStringLiteral("profilePreviewToggle"))->property("label").toString(), QStringLiteral("Preview"));
    }

    void tabOrderF6AndCtrlDigits()
    {
        EditorFixture f;
        QVERIFY(f.ready());
        const auto region = [&f] {
            QQuickItem *at = f.focused();
            if (EditorFixture::isInside(at, f.item(QStringLiteral("profileEditorRail"))))
                return QStringLiteral("rail");
            if (EditorFixture::isInside(at, f.item(QStringLiteral("profileEditorPanel"))))
                return QStringLiteral("panel");
            if (EditorFixture::isInside(at, f.frame()))
                return QStringLiteral("preview");
            if (EditorFixture::isInside(at, f.bar()))
                return QStringLiteral("bar");
            return QString();
        };
        // F6 cycles rail → panel → preview → top bar → rail.
        f.key(Qt::Key_F6);
        QCOMPARE(region(), QStringLiteral("rail"));
        f.key(Qt::Key_F6);
        QCOMPARE(region(), QStringLiteral("panel"));
        f.key(Qt::Key_F6);
        QCOMPARE(region(), QStringLiteral("preview"));
        f.key(Qt::Key_F6);
        QCOMPARE(region(), QStringLiteral("bar"));
        QVERIFY(f.focusIn(QStringLiteral("profileEditorBackButton")));
        f.key(Qt::Key_F6);
        QCOMPARE(region(), QStringLiteral("rail"));

        // Tab from the rail goes into the panel's first control.
        f.key(Qt::Key_Tab);
        QCOMPARE(region(), QStringLiteral("panel"));
        QVERIFY(f.focusIn(QStringLiteral("profileThemesGrid")));

        // Ctrl+1…9 jump to a tab.
        f.key(Qt::Key_3, Qt::ControlModifier);
        QCOMPARE(f.profiles().lastTab(), int(EditorTab::BoxesTab));
        QTRY_VERIFY(f.tabItem(QStringLiteral("boxes")));
        QCOMPARE(region(), QStringLiteral("rail"));
        f.key(Qt::Key_9, Qt::ControlModifier);
        QCOMPARE(f.profiles().lastTab(), int(EditorTab::LayoutTab));
        f.key(Qt::Key_1, Qt::ControlModifier);
        QCOMPARE(f.profiles().lastTab(), int(EditorTab::ThemesTab));

        // None of them fires while a popup is open.
        f.openTab(QStringLiteral("background"));
        f.click(QStringLiteral("profileBaseColourWell"));
        QTRY_VERIFY(f.isOpen(QStringLiteral("profileColorPicker")));
        f.key(Qt::Key_5, Qt::ControlModifier);
        QCOMPARE(f.profiles().lastTab(), int(EditorTab::BackgroundTab));
        f.key(Qt::Key_Escape);
        QTRY_VERIFY(!f.isOpen(QStringLiteral("profileColorPicker")));
        f.key(Qt::Key_5, Qt::ControlModifier);
        QCOMPARE(f.profiles().lastTab(), int(EditorTab::NameTab));
    }

    // --- Captures ------------------------------------------------------------

    // OPENCHAT_PROFILE_CAPTURES: the mockups' editor scenes
    // (final/final-editor-*.png), light and dark, each tab at 1024×768, and the
    // editor at the default and minimum windows.
    void captures()
    {
        if (qEnvironmentVariableIsEmpty("OPENCHAT_PROFILE_CAPTURES"))
            QSKIP("Set OPENCHAT_PROFILE_CAPTURES=<dir> to save the editor's states.");
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        for (const bool dark : {false, true}) {
            AppearanceSettings().setDarkMode(dark);
            const QString mode = dark ? QStringLiteral("-dark") : QStringLiteral("-light");
            {
                // Themes: an edited Headliner, Chrome Y2K being tried on.
                EditorFixture f;
                QVERIFY(f.ready());
                f.draft().setBoxOpacity(84);
                f.hover(f.item(QStringLiteral("profileThemeTile_chrome-y2k")));
                QTRY_COMPARE(f.profiles().tryOnPreset(), int(Profile::Preset::ChromeY2KPreset));
                QTest::qWait(200);
                f.capture(QStringLiteral("editor-themes") + mode);
                f.hover(f.item(QStringLiteral("profileEditorRail")));
                QTRY_COMPARE(f.profiles().tryOnPreset(), -1);

                // Boxes and the leave dialog over it.
                f.profiles().applyPreset(int(Profile::Preset::NeonZebraPreset));
                f.openTab(QStringLiteral("boxes"));
                f.capture(QStringLiteral("editor-boxes") + mode);
                f.editor()->forceActiveFocus();
                f.key(Qt::Key_Escape);
                QTRY_VERIFY(f.isOpen(QStringLiteral("profileLeaveDialog")));
                QTest::qWait(150);
                f.capture(QStringLiteral("editor-leave") + mode);
                f.key(Qt::Key_Escape);
                QTRY_VERIFY(!f.isOpen(QStringLiteral("profileLeaveDialog")));

                // Text: Linen with a link colour too faint, the picker on it.
                f.profiles().applyPreset(int(Profile::Preset::LinenPreset));
                f.draft().setLabelColor(QColor(QStringLiteral("#85552a")));
                f.draft().setLinkColor(QColor(QStringLiteral("#dfa46a")));
                f.openTab(QStringLiteral("text"));
                f.click(QStringLiteral("profileLinkColourWell"));
                QTRY_VERIFY(f.isOpen(QStringLiteral("profileColorPicker")));
                f.profiles().rememberColor(QColor(QStringLiteral("#85552a")));
                f.profiles().rememberColor(QColor(QStringLiteral("#6b8e23")));
                f.profiles().rememberColor(QColor(QStringLiteral("#1f6fa3")));
                QTest::qWait(150);
                f.capture(QStringLiteral("editor-text") + mode);
                f.key(Qt::Key_Escape);
                QTRY_VERIFY(!f.isOpen(QStringLiteral("profileColorPicker")));

                // Name & FX: Scene Queen with stars and sparkles.
                f.profiles().applyPreset(int(Profile::Preset::SceneQueenPreset));
                f.draft().setNameFlourish(int(Profile::Flourish::StarFlourish));
                f.draft().setNameSize(int(Profile::NameSize::LargeName));
                f.draft().setAmbient(int(Profile::Ambient::FloatingSparkles));
                f.openTab(QStringLiteral("name"));
                f.capture(QStringLiteral("editor-name") + mode);

                // About me with its About me field in use.
                f.profiles().undo();
                f.profiles().undo();
                f.profiles().undo();
                f.profiles().undo();
                f.openTab(QStringLiteral("about"));
                f.focusField(QStringLiteral("profileField_aboutMe"));
                f.key(Qt::Key_End);
                f.capture(QStringLiteral("editor-about") + mode);
                f.item(QStringLiteral("profileEditorRail"))->forceActiveFocus();

                // Top Friends with the pointer on a contact.
                f.profiles().applyPreset(int(Profile::Preset::Classic06Preset));
                f.profiles().removeTopFriend(5);
                f.openTab(QStringLiteral("friends"));
                f.hover(f.item(QStringLiteral("profileFriendCandidate_tom")));
                f.capture(QStringLiteral("editor-friends") + mode);

                // Layout: Midnight Emo.
                f.profiles().applyPreset(int(Profile::Preset::MidnightEmoPreset));
                f.openTab(QStringLiteral("layout"));
                f.hover(f.item(QStringLiteral("profileLayoutRow_%1").arg(int(Profile::Module::TopFriendsModule))));
                f.capture(QStringLiteral("editor-layout") + mode);
            }
            {
                // Song: a freshly imported file.
                const QString song = writeSong(dir, QStringLiteral("such_great_heights.wav"), 70.0, 3.0);
                EditorFixture f;
                QVERIFY(f.ready());
                f.profiles().importSong(QUrl::fromLocalFile(song));
                QTRY_VERIFY_WITH_TIMEOUT(!f.profiles().songImporting() && f.profiles().songPeaks().size() > 0, 20'000);
                f.draft().setSongTitle(QStringLiteral("Such Great Heights"));
                f.draft().setSongArtist(QStringLiteral("The Postal Service"));
                f.openTab(QStringLiteral("song"));
                f.capture(QStringLiteral("editor-song") + mode);
                f.openTab(QStringLiteral("background"));
                f.capture(QStringLiteral("editor-background") + mode);
            }
            {
                // The default window: Glitter Girl's background, a one-column preview.
                EditorFixture f(QSize(860, 680));
                QVERIFY(f.ready());
                f.profiles().applyPreset(int(Profile::Preset::GlitterGirlPreset));
                f.openTab(QStringLiteral("background"));
                f.capture(QStringLiteral("editor-narrow") + mode);
            }
            {
                // The minimum window: compact bar, 44 px tabs.
                EditorFixture f(QSize(720, 560), false);
                QVERIFY(f.ready());
                f.draft().setBoxOpacity(84);
                f.profiles().setLastTab(int(EditorTab::ThemesTab));
                f.capture(QStringLiteral("editor-min") + mode);
            }
        }
        AppearanceSettings().setDarkMode(false);
    }
};

OPENCHAT_PROFILE_QML_TEST_MAIN(ProfileEditorTest, "qml-profile-editor")

#include "tst_profileeditor.moc"
