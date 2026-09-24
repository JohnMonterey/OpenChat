// The owner's editor in QML (ARCH §9.8): tests/qml/ProfileEditorHarness.qml
// hosts ProfileEditorBar and ProfileEditor as the profile page does, over the
// reference mock's ChatController, and every test drives them the way a
// person would (mouse, keys, focus) and checks the draft, the controller and
// what is on screen. Every QML warning fails the test.
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
#include "profile/ProfileRenderPolicy.h"
#include "profile/SongImport.h"
#include "profile/SongPlayer.h"

#include <QDir>
#include <QFile>
#include <QImage>
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

// The editor over the reference mock, in the harness window.
class EditorFixture final
{
public:
    // Opens the viewer's own profile (Daniel's reference page when `daniel`,
    // else the default page), starts editing and shows the editor at `size`.
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

    [[nodiscard]] bool ready()
    {
        // Once the rail and panel have slid in.
        return m_editing && m_window && QTest::qWaitForWindowExposed(m_window.get())
               && QTest::qWaitForWindowActive(m_window.get()) && editor() != nullptr
               && QTest::qWaitFor([this] { return editor()->property("slide").toReal() == 0.0; });
    }

    [[nodiscard]] ChatController &chat() { return m_chat; }
    [[nodiscard]] ProfileController &profiles() { return *m_chat.profiles(); }
    [[nodiscard]] ProfilePageObject &draft() { return *m_chat.profiles()->draft(); }
    [[nodiscard]] QQuickWindow *window() const { return m_window.get(); }
    [[nodiscard]] QQuickItem *editor() const { return item("profileEditor"); }
    [[nodiscard]] QQuickItem *bar() const { return item("profileEditorBar"); }

    // Items are looked up in the visual tree (Repeater delegates have no
    // QObject parent, and popups live in the window's overlay).
    [[nodiscard]] QQuickItem *item(const QString &name) const
    {
        const QList<QQuickItem *> all = items(name);
        return all.isEmpty() ? nullptr : all.first();
    }
    [[nodiscard]] QList<QQuickItem *> items(const QString &name) const
    {
        QList<QQuickItem *> found;
        if (m_window)
            collect(m_window->contentItem(), name, found);
        return found;
    }
    // Non-visual objects (popups, dialogs, the song player).
    [[nodiscard]] QObject *object(const QString &name) const
    {
        return m_window ? m_window->findChild<QObject *>(name) : nullptr;
    }
    // A shown item called `name` (hidden copies, e.g. in closed folds, are skipped).
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

    // Clicks the middle of `item` (or `at`, in its coordinates).
    void click(QQuickItem *target, QPointF at = QPointF(-1, -1), Qt::MouseButton button = Qt::LeftButton,
               Qt::KeyboardModifiers modifiers = Qt::NoModifier)
    {
        QVERIFY(target);
        if (at.x() < 0)
            at = QPointF(target->width() / 2, target->height() / 2);
        QTest::mouseClick(m_window.get(), button, modifiers, target->mapToScene(at).toPoint());
    }
    void hover(QQuickItem *target, QPointF at = QPointF(-1, -1))
    {
        QVERIFY(target);
        if (at.x() < 0)
            at = QPointF(target->width() / 2, target->height() / 2);
        QTest::mouseMove(m_window.get(), target->mapToScene(at).toPoint());
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
    // Opens editor tab `name` through the rail, as a click would.
    void openTab(const QString &name)
    {
        QQuickItem *tab = item(QStringLiteral("profileEditorTab_") + name);
        QVERIFY2(tab, qPrintable(name));
        click(tab);
        QTRY_VERIFY(item(QStringLiteral("profile") + tabObjectName(name) + QStringLiteral("Tab")) != nullptr);
    }
    [[nodiscard]] static QString tabObjectName(const QString &name)
    {
        static const QHash<QString, QString> names{
            {QStringLiteral("themes"), QStringLiteral("Themes")}, {QStringLiteral("background"), QStringLiteral("Background")},
            {QStringLiteral("boxes"), QStringLiteral("Boxes")},   {QStringLiteral("text"), QStringLiteral("Text")},
            {QStringLiteral("name"), QStringLiteral("Name")},     {QStringLiteral("about"), QStringLiteral("About")},
            {QStringLiteral("friends"), QStringLiteral("Friends")}, {QStringLiteral("song"), QStringLiteral("Song")},
            {QStringLiteral("layout"), QStringLiteral("Layout")}};
        return names.value(name);
    }

    // OPENCHAT_PROFILE_CAPTURES: saves the window as `name`.png.
    void capture(const QString &name) const
    {
        const QString dir = qEnvironmentVariable("OPENCHAT_PROFILE_CAPTURES");
        if (dir.isEmpty() || !m_window)
            return;
        QDir().mkpath(dir);
        QTest::qWait(50);
        const QImage image = m_window->grabWindow();
        QVERIFY2(image.save(QDir(dir).filePath(name + QStringLiteral(".png"))), qPrintable(name));
    }

private:
    ChatController m_chat;
    bool m_editing = false;
    QQmlEngine m_engine;
    std::unique_ptr<QQuickWindow> m_window;
};

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

    void editorOpensOnlyForOwnProfile()
    {
        EditorFixture fixture;
        QVERIFY(fixture.ready());
        ProfileController &profiles = fixture.profiles();

        // A contact's page has no editor: beginEditing refuses and nothing loads.
        profiles.closeAll();
        QTRY_VERIFY(fixture.editor() == nullptr);
        QVERIFY(profiles.openContact(QStringLiteral("michael")));
        QVERIFY(!profiles.beginEditing());
        QVERIFY(!profiles.editing());
        QTest::qWait(20);
        QCOMPARE(fixture.editor(), nullptr);
        QCOMPARE(fixture.bar(), nullptr);

        // Your own page does: the bar, the rail, the panel and the preview.
        profiles.openOwn();
        QVERIFY(profiles.beginEditing());
        QTRY_VERIFY(fixture.editor() != nullptr);
        QVERIFY(fixture.bar() != nullptr);
        QVERIFY(fixture.item(QStringLiteral("profileEditorRail")));
        QVERIFY(fixture.item(QStringLiteral("profileEditorPanel")));
        QVERIFY(fixture.item(QStringLiteral("profilePreviewFrame")));
        // The editor found the page's inputs by itself.
        QCOMPARE(fixture.editor()->property("profiles").value<QObject *>(), &profiles);
        QVERIFY(fixture.editor()->property("songPlayer").value<QObject *>() != nullptr);
        QVERIFY(fixture.editor()->property("avatarFileDialog").value<QObject *>() != nullptr);
        QCOMPARE(fixture.bar()->property("editor").value<QQuickItem *>(), fixture.editor());
    }

    void railHasNineTabsAndRemembersTheLast()
    {
        const QStringList names{QStringLiteral("themes"), QStringLiteral("background"), QStringLiteral("boxes"),
                                QStringLiteral("text"),   QStringLiteral("name"),       QStringLiteral("about"),
                                QStringLiteral("friends"), QStringLiteral("song"),      QStringLiteral("layout")};
        {
            EditorFixture fixture;
            QVERIFY(fixture.ready());
            // Themes the first time.
            QCOMPARE(fixture.profiles().lastTab(), int(Profile::EditorTab::ThemesTab));
            QVERIFY(fixture.item(QStringLiteral("profileThemesTab")));
            for (const QString &name : names) {
                QQuickItem *tab = fixture.item(QStringLiteral("profileEditorTab_") + name);
                QVERIFY2(tab, qPrintable(name));
                QVERIFY2(EditorFixture::isShown(tab), qPrintable(name));
                // Every tab fits the rail.
                QVERIFY(tab->mapToScene(QPointF(0, tab->height())).y() <= fixture.window()->height());
            }
            fixture.openTab(QStringLiteral("song"));
            QCOMPARE(fixture.profiles().lastTab(), int(Profile::EditorTab::SongTab));
            QVERIFY(fixture.item(QStringLiteral("profileSongTab")));
            QCOMPARE(fixture.item(QStringLiteral("profileThemesTab")), nullptr);
        }
        // The next visit opens on the tab used last.
        EditorFixture again;
        QVERIFY(again.ready());
        QCOMPARE(again.profiles().lastTab(), int(Profile::EditorTab::SongTab));
        QVERIFY(again.item(QStringLiteral("profileSongTab")));

        // The rail is one Tab stop; ↑/↓ move between tabs.
        QQuickItem *rail = again.item(QStringLiteral("profileEditorRail"));
        rail->forceActiveFocus();
        again.key(Qt::Key_Down);
        QCOMPARE(again.profiles().lastTab(), int(Profile::EditorTab::LayoutTab));
        again.key(Qt::Key_Up);
        again.key(Qt::Key_Up);
        QCOMPARE(again.profiles().lastTab(), int(Profile::EditorTab::FriendsTab));
        QTRY_VERIFY(again.item(QStringLiteral("profileFriendsTab")));
    }

    // OPENCHAT_PROFILE_CAPTURES: every tab at the mockups' 1024×768, light and
    // dark, and the editor at the default and minimum windows.
    void captures()
    {
        if (qEnvironmentVariableIsEmpty("OPENCHAT_PROFILE_CAPTURES"))
            QSKIP("Set OPENCHAT_PROFILE_CAPTURES=<dir> to save the editor's states.");
        const QStringList tabs{QStringLiteral("themes"), QStringLiteral("background"), QStringLiteral("boxes"),
                               QStringLiteral("text"),   QStringLiteral("name"),       QStringLiteral("about"),
                               QStringLiteral("friends"), QStringLiteral("song"),      QStringLiteral("layout")};
        for (const bool dark : {false, true}) {
            AppearanceSettings().setDarkMode(dark);
            const QString mode = dark ? QStringLiteral("-dark") : QStringLiteral("-light");
            EditorFixture fixture;
            QVERIFY(fixture.ready());
            for (const QString &tab : tabs) {
                fixture.openTab(tab);
                fixture.capture(QStringLiteral("editor-") + tab + mode);
            }
        }
        AppearanceSettings().setDarkMode(false);
    }
};

OPENCHAT_PROFILE_QML_TEST_MAIN(ProfileEditorTest, "qml-profile-editor")

#include "tst_profileeditor.moc"
