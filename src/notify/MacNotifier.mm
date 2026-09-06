#include "notify/MacNotifier.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QImage>
#include <QMetaObject>
#include <QPointer>
#include <QStandardPaths>
#include <QUuid>

#import <Foundation/Foundation.h>
#import <UserNotifications/UserNotifications.h>

#include <utility>

namespace {

// Notification identifiers must survive a round trip through Cocoa, so a
// conversation key is reduced to a stable, plain-ASCII identifier.
NSString *identifierFor(const QString &key)
{
    const QByteArray digest =
        QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256).toHex().left(32);
    return [NSString stringWithUTF8String:digest.constData()];
}

NSString *toNSString(const QString &text)
{
    return [NSString stringWithUTF8String:text.toUtf8().constData()];
}

} // namespace

// The delegate that receives taps. It holds a guarded pointer back to the
// backend so a notification tapped after the backend is gone is simply dropped.
@interface OpenChatNotificationDelegate : NSObject <UNUserNotificationCenterDelegate>
@property(nonatomic, assign) OpenChat::MacNotifier *backend;
@end

@implementation OpenChatNotificationDelegate

// Shown even while OpenChat is frontmost: the service has already decided the
// message is not one the user is currently reading.
- (void)userNotificationCenter:(UNUserNotificationCenter *)center
       willPresentNotification:(UNNotification *)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions))completionHandler
{
    (void)center;
    (void)notification;
    completionHandler(UNNotificationPresentationOptionBanner | UNNotificationPresentationOptionSound
                      | UNNotificationPresentationOptionList);
}

- (void)userNotificationCenter:(UNUserNotificationCenter *)center
    didReceiveNotificationResponse:(UNNotificationResponse *)response
             withCompletionHandler:(void (^)(void))completionHandler
{
    (void)center;
    if ([response.actionIdentifier isEqualToString:UNNotificationDefaultActionIdentifier]) {
        NSString *key = response.notification.request.content.userInfo[@"conversationKey"];
        if (key != nil && self.backend != nullptr) {
            const QString conversationKey = QString::fromUtf8([key UTF8String]);
            OpenChat::MacNotifier *backend = self.backend;
            // Hop to the Qt event loop: the delegate runs on Cocoa's queue.
            QMetaObject::invokeMethod(
                backend, [backend, conversationKey] { backend->reportActivated(conversationKey); },
                Qt::QueuedConnection);
        }
    }
    completionHandler();
}

@end

namespace OpenChat {

struct MacNotifier::Private {
    NotificationAppInfo appInfo;
    UNUserNotificationCenter *center = nil;
    OpenChatNotificationDelegate *delegate = nil;
    bool authorized = false;
    // Identifiers still on screen, so withdrawAll knows what to take back.
    QHash<QString, QString> identifiersByKey;
    // Where attachment images are staged before the framework claims them.
    QString attachmentDir;
};

MacNotifier::MacNotifier(NotificationAppInfo appInfo, QObject *parent)
    : NotificationBackend(parent), d(std::make_unique<Private>())
{
    d->appInfo = std::move(appInfo);

    // UserNotifications requires a LaunchServices-backed application bundle.
    // The standalone development executable has an embedded Info.plist, so it
    // reports a bundle identifier even though its main bundle is the build
    // directory. Asking for the notification centre in that state raises an
    // NSInternalInconsistencyException instead of returning nil.
    NSBundle *mainBundle = [NSBundle mainBundle];
    if (mainBundle.bundleIdentifier == nil
        || ![mainBundle.bundleURL.pathExtension.lowercaseString isEqualToString:@"app"])
        return;

    @try {
        d->center = [UNUserNotificationCenter currentNotificationCenter];
    } @catch (NSException *exception) {
        (void)exception;
        d->center = nil;
    }
    if (d->center == nil)
        return;

    d->delegate = [[OpenChatNotificationDelegate alloc] init];
    d->delegate.backend = this;
    d->center.delegate = d->delegate;

    d->attachmentDir =
        QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
            .filePath(QStringLiteral("OpenChat-notifications"));
    QDir().mkpath(d->attachmentDir);

    QPointer<MacNotifier> guard(this);
    [d->center requestAuthorizationWithOptions:(UNAuthorizationOptionAlert
                                                | UNAuthorizationOptionSound)
                             completionHandler:^(BOOL granted, NSError *error) {
                               (void)error;
                               const bool wasGranted = granted == YES;
                               if (qApp == nullptr)
                                   return;
                               // Hop to the Qt event loop; the guard covers a
                               // backend destroyed while the prompt was up.
                               QMetaObject::invokeMethod(
                                   qApp,
                                   [guard, wasGranted] {
                                       if (guard)
                                           guard->d->authorized = wasGranted;
                                   },
                                   Qt::QueuedConnection);
                             }];
}

MacNotifier::~MacNotifier()
{
    if (d->center != nil && d->delegate != nil) {
        if (d->center.delegate == d->delegate)
            d->center.delegate = nil;
        d->delegate.backend = nullptr;
    }
    if (!d->attachmentDir.isEmpty())
        QDir(d->attachmentDir).removeRecursively();
}

bool MacNotifier::isAvailable() const
{
    // Posts made before the authorization callback lands are still worth
    // attempting: the framework queues them and the user may have already
    // granted permission in an earlier run.
    return d->center != nil;
}

QString MacNotifier::name() const
{
    return QStringLiteral("macos");
}

void MacNotifier::show(const Notification &notification)
{
    if (!isAvailable())
        return;

    UNMutableNotificationContent *content = [[UNMutableNotificationContent alloc] init];
    content.title = toNSString(notification.title);
    content.body = toNSString(notification.body);
    content.sound = [UNNotificationSound defaultSound];
    content.userInfo = @{@"conversationKey" : toNSString(notification.key)};
    // Groups every message from one contact into a single stack, matching the
    // "replace, do not pile up" behaviour of the other backends.
    content.threadIdentifier = toNSString(notification.key);

    // The picture: written to its own file because accepting an attachment
    // moves the file into the notification store.
    if (!notification.image.isNull() && !d->attachmentDir.isEmpty()) {
        const QString file =
            QDir(d->attachmentDir)
                .filePath(QUuid::createUuid().toString(QUuid::WithoutBraces)
                          + QStringLiteral(".png"));
        if (notification.image.save(file, "PNG")) {
            NSURL *url = [NSURL fileURLWithPath:toNSString(file)];
            NSError *attachmentError = nil;
            UNNotificationAttachment *attachment =
                [UNNotificationAttachment attachmentWithIdentifier:@"avatar"
                                                               URL:url
                                                           options:nil
                                                             error:&attachmentError];
            if (attachment != nil && attachmentError == nil)
                content.attachments = @[ attachment ];
            else
                QFile::remove(file);
        }
    }

    NSString *identifier = identifierFor(notification.key);
    // Re-using the identifier replaces the notification already on screen.
    UNNotificationRequest *request = [UNNotificationRequest requestWithIdentifier:identifier
                                                                         content:content
                                                                         trigger:nil];
    [d->center addNotificationRequest:request withCompletionHandler:nil];
    d->identifiersByKey.insert(notification.key, QString::fromUtf8([identifier UTF8String]));
}

void MacNotifier::withdraw(const QString &key)
{
    if (!isAvailable())
        return;
    const auto it = d->identifiersByKey.constFind(key);
    if (it == d->identifiersByKey.cend())
        return;
    NSArray<NSString *> *identifiers = @[ toNSString(it.value()) ];
    [d->center removeDeliveredNotificationsWithIdentifiers:identifiers];
    [d->center removePendingNotificationRequestsWithIdentifiers:identifiers];
    d->identifiersByKey.erase(it);
}

void MacNotifier::withdrawAll()
{
    if (!isAvailable())
        return;
    [d->center removeAllDeliveredNotifications];
    [d->center removeAllPendingNotificationRequests];
    d->identifiersByKey.clear();
}

void MacNotifier::reportActivated(const QString &key)
{
    d->identifiersByKey.remove(key);
    emit activated(key);
}

} // namespace OpenChat
