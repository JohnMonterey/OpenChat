// The freedesktop backend against a notification daemon of our own.
//
// tst_notifications covers the policy without a desktop. What it cannot cover
// is the half of the Linux backend that only exists on the bus: the arguments
// the daemon actually receives, the id bookkeeping that lets one conversation
// replace and close its own notification, and the click that comes back as a
// signal. So this test starts a private session bus, owns
// org.freedesktop.Notifications on it, and talks to the real backend across it.
//
// It skips itself where dbus-daemon is not installed or a private bus will not
// start, so it is a bonus where the environment allows it rather than a new
// requirement on the build.

#include "notify/FreedesktopNotifier.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMetaType>
#include <QGuiApplication>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QVariantMap>

#include <memory>

using OpenChat::FreedesktopImageData;
using OpenChat::FreedesktopNotifier;
using OpenChat::Notification;
using OpenChat::NotificationAppInfo;

namespace {

const QString notificationsService = QStringLiteral("org.freedesktop.Notifications");
const QString notificationsPath = QStringLiteral("/org/freedesktop/Notifications");

} // namespace

// Stands in for the desktop's notification daemon: records what it was asked to
// show and can invoke an action the way a user's click would.
class FakeNotificationDaemon final : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Notifications")

public:
    struct Shown final {
        QString appName;
        uint replacesId = 0;
        QString summary;
        QString body;
        QStringList actions;
        QVariantMap hints;
        uint id = 0;
    };

    QList<Shown> shown;
    QList<uint> closed;
    QStringList capabilities{QStringLiteral("body"), QStringLiteral("body-markup"),
                             QStringLiteral("actions")};

    // Delivers a click on a notification, as the daemon does.
    void invokeAction(uint id, const QString &action)
    {
        emit ActionInvoked(id, action);
    }

public slots:
    QStringList GetCapabilities() { return capabilities; }

    uint Notify(const QString &appName, uint replacesId, const QString &appIcon,
                const QString &summary, const QString &body, const QStringList &actions,
                const QVariantMap &hints, int timeout)
    {
        Q_UNUSED(appIcon);
        Q_UNUSED(timeout);
        // A daemon reuses the id it was asked to replace, and mints a new one
        // otherwise; the backend's bookkeeping depends on both.
        const uint id = replacesId != 0 ? replacesId : ++m_nextId;
        shown.append(Shown{appName, replacesId, summary, body, actions, hints, id});
        return id;
    }

    void CloseNotification(uint id)
    {
        closed.append(id);
        emit NotificationClosed(id, 3); // 3: closed by a call to CloseNotification
    }

signals:
    void ActionInvoked(uint id, const QString &action);
    void NotificationClosed(uint id, uint reason);

private:
    uint m_nextId = 100;
};

class TestFreedesktopNotifier final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void sendsTheSenderMessageAndPictureToTheDaemon();
    void replacesTheNotificationOfTheSameConversation();
    void separateConversationsGetSeparateNotifications();
    void reportsTheConversationBehindAClick();
    void withdrawClosesTheNotification();
    void escapesMarkupOnlyWhereTheDaemonParsesIt();

private:
    [[nodiscard]] Notification message(const QString &key, const QString &title,
                                       const QString &body) const;
    // Runs the event loop until the daemon has recorded `count` notifications.
    bool waitForShown(int count);

    QDBusConnection bus() { return QDBusConnection::sessionBus(); }

    std::unique_ptr<FakeNotificationDaemon> m_daemon;
    std::unique_ptr<FreedesktopNotifier> m_notifier;
};

void TestFreedesktopNotifier::initTestCase()
{
    if (!QDBusConnection::sessionBus().isConnected())
        QSKIP("No session bus available.");
    // main() points this process at a private bus; if the name is already taken
    // the test would be talking to the real desktop, which it must never do.
    QVERIFY2(QDBusConnection::sessionBus().interface() != nullptr, "no bus interface");
}

void TestFreedesktopNotifier::init()
{
    m_daemon = std::make_unique<FakeNotificationDaemon>();
    QVERIFY2(bus().registerObject(notificationsPath, m_daemon.get(),
                                  QDBusConnection::ExportAllSlots
                                      | QDBusConnection::ExportAllSignals),
             "could not export the fake daemon");
    QVERIFY2(bus().registerService(notificationsService),
             "could not own org.freedesktop.Notifications on the private bus");

    NotificationAppInfo appInfo;
    appInfo.applicationName = QStringLiteral("OpenChat");
    appInfo.desktopEntry = QStringLiteral("openchat");
    appInfo.iconName = QStringLiteral("openchat");
    m_notifier = std::make_unique<FreedesktopNotifier>(appInfo);
    QVERIFY(m_notifier->isAvailable());
}

void TestFreedesktopNotifier::cleanup()
{
    m_notifier.reset();
    bus().unregisterService(notificationsService);
    bus().unregisterObject(notificationsPath);
    m_daemon.reset();
}

Notification TestFreedesktopNotifier::message(const QString &key, const QString &title,
                                              const QString &body) const
{
    Notification notification;
    notification.key = key;
    notification.title = title;
    notification.body = body;
    notification.image = QImage(8, 8, QImage::Format_ARGB32_Premultiplied);
    notification.image.fill(Qt::red);
    return notification;
}

bool TestFreedesktopNotifier::waitForShown(int count)
{
    return QTest::qWaitFor([this, count] { return m_daemon->shown.size() >= count; }, 5000);
}

void TestFreedesktopNotifier::sendsTheSenderMessageAndPictureToTheDaemon()
{
    m_notifier->show(message(QStringLiteral("michael"), QStringLiteral("Michael"),
                             QStringLiteral("Are we still on for tonight?")));
    QVERIFY(waitForShown(1));

    const auto &shown = m_daemon->shown.at(0);
    QCOMPARE(shown.appName, QStringLiteral("OpenChat"));
    QCOMPARE(shown.summary, QStringLiteral("Michael"));
    QCOMPARE(shown.body, QStringLiteral("Are we still on for tonight?"));
    QCOMPARE(shown.replacesId, 0u);
    // The daemon says it supports actions, so the notification is clickable.
    QVERIFY(shown.actions.contains(QStringLiteral("default")));
    QCOMPARE(shown.hints.value(QStringLiteral("desktop-entry")).toString(),
             QStringLiteral("openchat"));
    QCOMPARE(shown.hints.value(QStringLiteral("category")).toString(),
             QStringLiteral("im.received"));

    // The picture arrives as the specification's image hint, not as a file path.
    QVERIFY(shown.hints.contains(QStringLiteral("image-data")));
    FreedesktopImageData image;
    shown.hints.value(QStringLiteral("image-data")).value<QDBusArgument>() >> image;
    QCOMPARE(image.width, 8);
    QCOMPARE(image.height, 8);
    QCOMPARE(image.channels, 4);
    QCOMPARE(image.pixels.size(), qsizetype(8 * 8 * 4));
}

void TestFreedesktopNotifier::replacesTheNotificationOfTheSameConversation()
{
    m_notifier->show(message(QStringLiteral("michael"), QStringLiteral("Michael"),
                             QStringLiteral("first")));
    QVERIFY(waitForShown(1));
    const uint firstId = m_daemon->shown.at(0).id;

    m_notifier->show(message(QStringLiteral("michael"), QStringLiteral("Michael"),
                             QStringLiteral("second")));
    QVERIFY(waitForShown(2));

    // The second message replaces the first rather than stacking beside it.
    QCOMPARE(m_daemon->shown.at(1).replacesId, firstId);
    QCOMPARE(m_daemon->shown.at(1).body, QStringLiteral("second"));
}

void TestFreedesktopNotifier::separateConversationsGetSeparateNotifications()
{
    m_notifier->show(message(QStringLiteral("michael"), QStringLiteral("Michael"),
                             QStringLiteral("hello")));
    QVERIFY(waitForShown(1));
    m_notifier->show(message(QStringLiteral("jessica"), QStringLiteral("Jessica"),
                             QStringLiteral("hello")));
    QVERIFY(waitForShown(2));

    QCOMPARE(m_daemon->shown.at(1).replacesId, 0u);
    QVERIFY(m_daemon->shown.at(0).id != m_daemon->shown.at(1).id);
}

void TestFreedesktopNotifier::reportsTheConversationBehindAClick()
{
    QSignalSpy activated(m_notifier.get(), &FreedesktopNotifier::activated);
    m_notifier->show(message(QStringLiteral("michael"), QStringLiteral("Michael"),
                             QStringLiteral("hello")));
    m_notifier->show(message(QStringLiteral("jessica"), QStringLiteral("Jessica"),
                             QStringLiteral("hello")));
    QVERIFY(waitForShown(2));

    // Click the second one: the backend must name that conversation, not the
    // most recent notification or the first.
    //
    // The daemon has recorded the notification, but the backend learns its id
    // from the reply to its own call, which may still be in flight. Rather than
    // sleep, click until it lands: a click on an id the backend does not know
    // is ignored, and once it does know, the first one through wins.
    const uint secondId = m_daemon->shown.at(1).id;
    QVERIFY(QTest::qWaitFor(
        [this, secondId, &activated] {
            if (activated.isEmpty())
                m_daemon->invokeAction(secondId, QStringLiteral("default"));
            return !activated.isEmpty();
        },
        5000));
    QCOMPARE(activated.size(), 1);
    QCOMPARE(activated.at(0).at(0).toString(), QStringLiteral("jessica"));
}

void TestFreedesktopNotifier::withdrawClosesTheNotification()
{
    m_notifier->show(message(QStringLiteral("michael"), QStringLiteral("Michael"),
                             QStringLiteral("hello")));
    QVERIFY(waitForShown(1));
    const uint id = m_daemon->shown.at(0).id;

    m_notifier->withdraw(QStringLiteral("michael"));
    QVERIFY(QTest::qWaitFor([this] { return !m_daemon->closed.isEmpty(); }, 5000));
    QCOMPARE(m_daemon->closed.at(0), id);

    // The notification is gone, so the next message for that chat is a new one.
    m_notifier->show(message(QStringLiteral("michael"), QStringLiteral("Michael"),
                             QStringLiteral("again")));
    QVERIFY(waitForShown(2));
    QCOMPARE(m_daemon->shown.at(1).replacesId, 0u);
}

void TestFreedesktopNotifier::escapesMarkupOnlyWhereTheDaemonParsesIt()
{
    m_notifier->show(message(QStringLiteral("michael"), QStringLiteral("Michael"),
                             QStringLiteral("5 < 6 & <b>bold</b>")));
    QVERIFY(waitForShown(1));
    // This daemon declared body-markup, so a message containing angle brackets
    // must arrive escaped rather than as markup the sender chose.
    QCOMPARE(m_daemon->shown.at(0).body,
             QStringLiteral("5 &lt; 6 &amp; &lt;b&gt;bold&lt;/b&gt;"));

    // A daemon without body-markup gets the message as written.
    m_notifier.reset();
    m_daemon->capabilities = QStringList{QStringLiteral("body")};
    m_daemon->shown.clear();
    NotificationAppInfo appInfo;
    m_notifier = std::make_unique<FreedesktopNotifier>(appInfo);
    m_notifier->show(message(QStringLiteral("michael"), QStringLiteral("Michael"),
                             QStringLiteral("5 < 6")));
    QVERIFY(waitForShown(1));
    QCOMPARE(m_daemon->shown.at(0).body, QStringLiteral("5 < 6"));
    // and no click action, because this daemon cannot deliver one.
    QVERIFY(m_daemon->shown.at(0).actions.isEmpty());
}

// A private session bus, so the test never touches the user's real desktop.
// Started before the application object because QDBusConnection::sessionBus()
// resolves the address once, on first use.
int main(int argc, char *argv[])
{
    const QString daemon = QStandardPaths::findExecutable(QStringLiteral("dbus-daemon"));
    QTemporaryDir configDir;
    QProcess busProcess;
    if (daemon.isEmpty() || !configDir.isValid()) {
        qWarning("dbus-daemon is not available; skipping the freedesktop bus test.");
        return 0;
    }

    const QString configPath = configDir.filePath(QStringLiteral("session.conf"));
    {
        QFile config(configPath);
        if (!config.open(QIODevice::WriteOnly))
            return 0;
        // The upstream session policy, minus the service directories: this bus
        // must not autostart anything from the user's real desktop. Both the
        // send and the receive rule are needed, or a client cannot even reach
        // the bus driver to say hello.
        config.write(R"(<!DOCTYPE busconfig PUBLIC
 "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <listen>unix:tmpdir=/tmp</listen>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
)");
    }

    busProcess.start(daemon, {QStringLiteral("--config-file=") + configPath,
                              QStringLiteral("--print-address"), QStringLiteral("--nofork")});
    if (!busProcess.waitForStarted(5000) || !busProcess.waitForReadyRead(5000)) {
        qWarning("Could not start a private session bus; skipping.");
        return 0;
    }
    const QByteArray address = busProcess.readLine().trimmed();
    if (address.isEmpty()) {
        qWarning("The private session bus printed no address; skipping.");
        busProcess.kill();
        busProcess.waitForFinished(2000);
        return 0;
    }
    qputenv("DBUS_SESSION_BUS_ADDRESS", address);

    // QGuiApplication rather than QCoreApplication: the notifications carry
    // images, and QImage conversion wants the GUI stack initialised.
    QGuiApplication application(argc, argv);
    TestFreedesktopNotifier testCase;
    const int result = QTest::qExec(&testCase, argc, argv);

    QDBusConnection::disconnectFromBus(QDBusConnection::sessionBus().name());
    busProcess.kill();
    busProcess.waitForFinished(2000);
    return result;
}

#include "tst_freedesktopnotifier.moc"
