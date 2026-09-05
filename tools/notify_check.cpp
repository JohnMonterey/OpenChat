// Posts a desktop notification through the same code the application uses and
// reports what the desktop did with it.
//
// The notification policy is covered by tst_notifications, which runs anywhere;
// what it cannot check is whether a real notification service accepts the post
// and draws the picture. That needs a desktop session, so it lives here as a
// tool to be run by hand — the same arrangement as openchat-call-check.
//
//   openchat-notify-check                       one notification, default text
//   openchat-notify-check --name Michael --message "Are we still on?"
//   openchat-notify-check --avatar ~/face.jpg   use a real picture
//   openchat-notify-check --burst 7             check burst collapsing
//
// It waits for a click (or --timeout seconds) and prints which backend was
// chosen, whether the desktop accepted the post, and whether the notification
// was clicked. Run it on each platform to confirm notifications work there.

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QFile>
#include <QGuiApplication>
#include <QTextStream>
#include <QTimer>

#include "app/AppMetadata.h"
#include "notify/NotificationBackend.h"
#include "notify/NotificationService.h"
#include "render/AvatarStore.h"

namespace {

QTextStream &out()
{
    static QTextStream stream(stdout);
    return stream;
}

} // namespace

int main(int argc, char *argv[])
{
    QGuiApplication application(argc, argv);
    QCoreApplication::setApplicationName(OpenChat::AppMetadata::name.toString());
    QGuiApplication::setDesktopFileName(OpenChat::AppMetadata::desktopEntry.toString());

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Post a desktop notification the way OpenChat does."));
    parser.addHelpOption();
    const QCommandLineOption nameOption(QStringLiteral("name"),
                                        QStringLiteral("Sender's display name."),
                                        QStringLiteral("name"), QStringLiteral("Michael"));
    const QCommandLineOption messageOption(
        QStringLiteral("message"), QStringLiteral("Message body."), QStringLiteral("text"),
        QStringLiteral("Are we still on for tonight?"));
    const QCommandLineOption avatarOption(
        QStringLiteral("avatar"),
        QStringLiteral("Picture file to show; defaults to the built-in artwork."),
        QStringLiteral("path"));
    const QCommandLineOption burstOption(
        QStringLiteral("burst"), QStringLiteral("Post <count> notifications from distinct chats."),
        QStringLiteral("count"), QStringLiteral("1"));
    const QCommandLineOption timeoutOption(
        QStringLiteral("timeout"), QStringLiteral("Seconds to wait for a click."),
        QStringLiteral("seconds"), QStringLiteral("30"));
    parser.addOption(nameOption);
    parser.addOption(messageOption);
    parser.addOption(avatarOption);
    parser.addOption(burstOption);
    parser.addOption(timeoutOption);
    parser.process(application);

    OpenChat::NotificationAppInfo appInfo;
    appInfo.applicationName = OpenChat::AppMetadata::name.toString();
    appInfo.desktopEntry = OpenChat::AppMetadata::desktopEntry.toString();
    appInfo.appUserModelId = OpenChat::AppMetadata::appUserModelId.toString();
    appInfo.iconName = OpenChat::AppMetadata::desktopEntry.toString();

    OpenChat::NotificationService service(OpenChat::makeNotificationBackend(appInfo));
    out() << "backend:   " << service.backendName() << Qt::endl;
    out() << "available: " << (service.isAvailable() ? "yes" : "no") << Qt::endl;
    if (!service.isAvailable()) {
        out() << "No notification service is reachable from this session." << Qt::endl;
        return 1;
    }

    // A supplied picture goes through the same store the received ones do, so
    // what is drawn here is what a contact's picture would look like.
    QString avatarKey = QStringLiteral("michael");
    if (parser.isSet(avatarOption)) {
        QFile file(parser.value(avatarOption));
        if (!file.open(QIODevice::ReadOnly)) {
            out() << "Cannot read " << parser.value(avatarOption) << Qt::endl;
            return 1;
        }
        const QString key = OpenChat::AvatarStore::instance().registerJpeg(file.readAll());
        if (key.isEmpty()) {
            out() << "Not a usable picture: " << parser.value(avatarOption) << Qt::endl;
            return 1;
        }
        avatarKey = key;
    }

    QObject::connect(&service, &OpenChat::NotificationService::conversationActivated,
                     &application, [](const QString &key) {
                         out() << "clicked:   " << key << Qt::endl;
                         QCoreApplication::exit(0);
                     });

    const int count = qMax(1, parser.value(burstOption).toInt());
    for (int i = 0; i < count; ++i) {
        const QString key = count == 1 ? QStringLiteral("check-contact")
                                       : QStringLiteral("check-contact-%1").arg(i);
        const QString name =
            count == 1 ? parser.value(nameOption)
                       : QStringLiteral("%1 %2").arg(parser.value(nameOption)).arg(i);
        service.postMessage(key, name, parser.value(messageOption), avatarKey);
    }
    out() << "posted:    " << count << Qt::endl;
    out() << "Waiting for a click; the notification is withdrawn on exit." << Qt::endl;

    QTimer::singleShot(qMax(1, parser.value(timeoutOption).toInt()) * 1000, &application, [] {
        out() << "clicked:   no (timed out)" << Qt::endl;
        QCoreApplication::exit(0);
    });
    return application.exec();
}
