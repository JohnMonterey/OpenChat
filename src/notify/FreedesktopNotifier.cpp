#include "notify/FreedesktopNotifier.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDebug>
#include <QImage>
#include <QTimer>
#include <QVariantMap>

#include <algorithm>
#include <cstring>
#include <utility>

namespace OpenChat {

namespace {

const QString service = QStringLiteral("org.freedesktop.Notifications");
const QString path = QStringLiteral("/org/freedesktop/Notifications");
const QString interface = QStringLiteral("org.freedesktop.Notifications");
// The action key the specification reserves for "the user clicked the body".
const QString defaultAction = QStringLiteral("default");
// How long a notification waits for the daemon to describe itself before going
// out anyway. This is a local round trip that normally completes in single
// digits of milliseconds; the wait only ever matters at startup.
constexpr int capabilityWaitMs = 1500;

QString categoryHint(NotificationCategory category)
{
    switch (category) {
    case NotificationCategory::Message:
        return QStringLiteral("im.received");
    case NotificationCategory::ContactRequest:
        return QStringLiteral("im");
    case NotificationCategory::IncomingCall:
        return QStringLiteral("call.incoming");
    }
    return QStringLiteral("im");
}

QDBusMessage callMessage(const QString &member)
{
    return QDBusMessage::createMethodCall(service, path, interface, member);
}

} // namespace

QDBusArgument &operator<<(QDBusArgument &argument, const FreedesktopImageData &image)
{
    argument.beginStructure();
    argument << image.width << image.height << image.rowStride << image.hasAlpha
             << image.bitsPerSample << image.channels << image.pixels;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, FreedesktopImageData &image)
{
    argument.beginStructure();
    argument >> image.width >> image.height >> image.rowStride >> image.hasAlpha
        >> image.bitsPerSample >> image.channels >> image.pixels;
    argument.endStructure();
    return argument;
}

FreedesktopImageData FreedesktopNotifier::toImageData(const QImage &image)
{
    FreedesktopImageData data;
    if (image.isNull())
        return data;

    // The hint is straight, non-premultiplied RGBA at 8 bits a sample; Qt's
    // painting formats are premultiplied, so the conversion is required rather
    // than cosmetic.
    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    if (rgba.isNull())
        return data;

    data.width = rgba.width();
    data.height = rgba.height();
    data.channels = 4;
    data.bitsPerSample = 8;
    data.hasAlpha = true;
    data.rowStride = data.width * data.channels;

    // Repack tightly: QImage rows are padded to a 4-byte boundary, and some
    // daemons ignore the declared stride.
    data.pixels.resize(static_cast<qsizetype>(data.rowStride) * data.height);
    for (int y = 0; y < data.height; ++y) {
        std::memcpy(data.pixels.data() + static_cast<qsizetype>(y) * data.rowStride,
                    rgba.constScanLine(y), static_cast<size_t>(data.rowStride));
    }
    return data;
}

FreedesktopNotifier::FreedesktopNotifier(NotificationAppInfo appInfo, QObject *parent)
    : NotificationBackend(parent), m_appInfo(std::move(appInfo))
{
    qDBusRegisterMetaType<FreedesktopImageData>();

    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return;

    bus.connect(service, path, interface, QStringLiteral("ActionInvoked"), this,
                SLOT(onActionInvoked(quint32, QString)));
    bus.connect(service, path, interface, QStringLiteral("NotificationClosed"), this,
                SLOT(onNotificationClosed(quint32, quint32)));

    // The daemon is allowed to be absent now and present later; a desktop that
    // restarts its notification service must not silence the application.
    m_watcher = new QDBusServiceWatcher(service, bus,
                                        QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(m_watcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
            [this](const QString &, const QString &, const QString &) {
                m_idsByKey.clear();
                m_keysById.clear();
                refreshAvailability();
            });

    refreshAvailability();
}

FreedesktopNotifier::~FreedesktopNotifier() = default;

void FreedesktopNotifier::refreshAvailability()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    QDBusConnectionInterface *iface = bus.interface();
    if (iface == nullptr) {
        m_serviceAvailable = false;
        return;
    }
    // Registered now, or startable on demand: both mean a notification will be
    // delivered rather than dropped.
    m_serviceAvailable = iface->isServiceRegistered(service).value()
        || iface->activatableServiceNames().value().contains(service);
    if (m_serviceAvailable) {
        requestCapabilities();
    } else {
        m_capabilities.clear();
        m_capabilitiesKnown = false;
    }
}

void FreedesktopNotifier::requestCapabilities()
{
    m_capabilitiesKnown = false;
    // Asked asynchronously: a blocking call here would stall application
    // startup behind an unresponsive daemon.
    QDBusPendingCall call =
        QDBusConnection::sessionBus().asyncCall(callMessage(QStringLiteral("GetCapabilities")));
    auto *watcher = new QDBusPendingCallWatcher(call, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this](QDBusPendingCallWatcher *finished) {
                finished->deleteLater();
                const QDBusPendingReply<QStringList> reply = *finished;
                markCapabilities(reply.isError() ? QStringList() : reply.value());
            });
    // A daemon that never answers must not swallow notifications: give up
    // waiting after a moment and post with the conservative subset.
    QTimer::singleShot(capabilityWaitMs, this, [this] {
        if (!m_capabilitiesKnown)
            markCapabilities(QStringList());
    });
}

void FreedesktopNotifier::markCapabilities(const QStringList &capabilities)
{
    if (m_capabilitiesKnown)
        return;
    m_capabilities = capabilities;
    m_capabilitiesKnown = true;
    flushDeferred();
}

void FreedesktopNotifier::flushDeferred()
{
    const QList<Notification> waiting = std::exchange(m_deferred, {});
    for (const Notification &notification : waiting)
        sendNotify(notification);
}

bool FreedesktopNotifier::supports(const char *capability) const
{
    return m_capabilities.contains(QLatin1StringView(capability));
}

bool FreedesktopNotifier::isAvailable() const
{
    return m_serviceAvailable && QDBusConnection::sessionBus().isConnected();
}

QString FreedesktopNotifier::name() const
{
    return QStringLiteral("freedesktop");
}

void FreedesktopNotifier::show(const Notification &notification)
{
    if (!isAvailable())
        return;
    if (!m_capabilitiesKnown) {
        // Held for the round trip that says whether this daemon can be clicked
        // and whether its body is markup. A second message for the same chat
        // replaces the one waiting, exactly as it would once posted.
        const auto same = std::find_if(m_deferred.begin(), m_deferred.end(),
                                       [&notification](const Notification &pending) {
                                           return pending.key == notification.key;
                                       });
        if (same != m_deferred.end())
            *same = notification;
        else
            m_deferred.append(notification);
        return;
    }
    sendNotify(notification);
}

void FreedesktopNotifier::sendNotify(const Notification &notification)
{
    if (m_pending.contains(notification.key)) {
        // The first post for this chat has not been given an id yet, so there
        // is nothing to name as replaces_id. Sending now would leave two
        // notifications for one conversation stacked on the desktop; instead
        // this one waits for the id and then replaces the notification it
        // belongs to. Only the newest waiting message is kept.
        m_queuedWhilePending.insert(notification.key, notification);
        return;
    }

    QVariantMap hints;
    hints.insert(QStringLiteral("desktop-entry"), m_appInfo.desktopEntry);
    hints.insert(QStringLiteral("category"), categoryHint(notification.category));
    hints.insert(QStringLiteral("urgency"), QVariant::fromValue(uchar(1))); // normal
    if (!notification.image.isNull()) {
        const FreedesktopImageData image = toImageData(notification.image);
        if (!image.pixels.isEmpty()) {
            // "image-data" is the 1.2 spelling; "image_data" is 1.1. Sending
            // both costs nothing and covers older daemons still in use.
            hints.insert(QStringLiteral("image-data"), QVariant::fromValue(image));
            hints.insert(QStringLiteral("image_data"), QVariant::fromValue(image));
        }
    }

    QStringList actions;
    if (supports("actions")) {
        // "default" is invoked by clicking the notification body itself, which
        // is how the user asks for the conversation to be opened.
        actions << defaultAction << QStringLiteral("Open");
    }

    // Only the body is parsed as markup, and only where the daemon says so.
    const QString body =
        supports("body-markup") ? notification.body.toHtmlEscaped() : notification.body;

    QDBusMessage message = callMessage(QStringLiteral("Notify"));
    message << m_appInfo.applicationName                    // app_name
            << m_idsByKey.value(notification.key, 0)        // replaces_id
            << m_appInfo.iconName                           // app_icon
            << notification.title                           // summary
            << body                                         // body
            << actions
            << hints
            << -1; // expire_timeout: let the desktop decide

    m_pending.insert(notification.key);
    auto *watcher =
        new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
    const QString key = notification.key;
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, key](QDBusPendingCallWatcher *finished) {
                finished->deleteLater();
                const QDBusPendingReply<quint32> reply = *finished;
                m_pending.remove(key);
                if (reply.isError()) {
                    m_withdrawnWhilePending.remove(key);
                    m_queuedWhilePending.remove(key);
                    return;
                }
                onNotifyReplied(key, reply.value());
                flushQueued(key);
            });
}

void FreedesktopNotifier::flushQueued(const QString &key)
{
    const auto queued = m_queuedWhilePending.constFind(key);
    if (queued == m_queuedWhilePending.cend())
        return;
    const Notification notification = queued.value();
    m_queuedWhilePending.erase(queued);
    sendNotify(notification);
}

void FreedesktopNotifier::onNotifyReplied(const QString &key, quint32 id)
{
    if (id == 0)
        return;
    // A withdraw that arrived before the daemon answered is honoured now.
    if (m_withdrawnWhilePending.remove(key)) {
        QDBusMessage close = callMessage(QStringLiteral("CloseNotification"));
        close << id;
        QDBusConnection::sessionBus().asyncCall(close);
        return;
    }
    // A replaced notification keeps its id; a new one takes a new id and the old
    // mapping for this key goes away.
    if (const quint32 previous = m_idsByKey.value(key, 0); previous != 0 && previous != id)
        m_keysById.remove(previous);
    m_idsByKey.insert(key, id);
    m_keysById.insert(id, key);
}

void FreedesktopNotifier::withdraw(const QString &key)
{
    // One that never went out is withdrawn by never sending it.
    m_deferred.removeIf(
        [&key](const Notification &pending) { return pending.key == key; });
    m_queuedWhilePending.remove(key);
    if (m_pending.contains(key))
        m_withdrawnWhilePending.insert(key);
    const quint32 id = m_idsByKey.value(key, 0);
    if (id == 0)
        return;
    forget(id);
    if (!isAvailable())
        return;
    QDBusMessage close = callMessage(QStringLiteral("CloseNotification"));
    close << id;
    QDBusConnection::sessionBus().asyncCall(close);
}

void FreedesktopNotifier::withdrawAll()
{
    m_deferred.clear();
    m_queuedWhilePending.clear();
    const QList<QString> keys = m_idsByKey.keys();
    for (const QString &key : keys)
        withdraw(key);
    m_withdrawnWhilePending.unite(m_pending);
}

void FreedesktopNotifier::forget(quint32 id)
{
    const QString key = m_keysById.take(id);
    if (!key.isEmpty() && m_idsByKey.value(key, 0) == id)
        m_idsByKey.remove(key);
}

void FreedesktopNotifier::onActionInvoked(quint32 id, const QString &actionKey)
{
    const auto it = m_keysById.constFind(id);
    if (it == m_keysById.cend())
        return; // someone else's notification on the same bus
    if (actionKey != defaultAction && actionKey != QStringLiteral("Open"))
        return;
    const QString key = it.value();
    forget(id);
    emit activated(key);
}

void FreedesktopNotifier::onNotificationClosed(quint32 id, quint32 reason)
{
    Q_UNUSED(reason);
    forget(id);
}

} // namespace OpenChat
