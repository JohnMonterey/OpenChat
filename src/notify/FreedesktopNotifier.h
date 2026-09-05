#pragma once

#include "notify/NotificationBackend.h"

#include <QByteArray>
#include <QDBusArgument>
#include <QHash>
#include <QList>
#include <QMetaType>
#include <QSet>
#include <QStringList>

QT_BEGIN_NAMESPACE
class QDBusServiceWatcher;
QT_END_NAMESPACE

namespace OpenChat {

// The picture hint of the freedesktop notification specification, transferred
// as the D-Bus structure (iiibiiay): width, height, row stride, whether there
// is an alpha channel, bits per sample, channels, and the pixels themselves.
struct FreedesktopImageData final {
    int width = 0;
    int height = 0;
    int rowStride = 0;
    bool hasAlpha = false;
    int bitsPerSample = 8;
    int channels = 4;
    QByteArray pixels;
};

QDBusArgument &operator<<(QDBusArgument &argument, const FreedesktopImageData &image);
const QDBusArgument &operator>>(const QDBusArgument &argument, FreedesktopImageData &image);

// Desktop notifications on Linux and the other freedesktop desktops.
//
// This talks to org.freedesktop.Notifications on the session bus, which is a
// D-Bus protocol rather than a display protocol: the same code reaches
// GNOME Shell, Plasma, mako, dunst and the rest identically whether the session
// is Wayland or X11, and needs no window-system connection of its own. The
// sender's picture travels in the standard "image-data" hint, which is what
// makes the desktop draw the avatar beside the name and the message.
//
// The daemon may be absent at startup and appear later (or restart), so service
// ownership is watched rather than sampled once.
class FreedesktopNotifier final : public NotificationBackend
{
    Q_OBJECT

public:
    explicit FreedesktopNotifier(NotificationAppInfo appInfo, QObject *parent = nullptr);
    ~FreedesktopNotifier() override;

    [[nodiscard]] bool isAvailable() const override;
    [[nodiscard]] QString name() const override;
    void show(const Notification &notification) override;
    void withdraw(const QString &key) override;
    void withdrawAll() override;

    // Converts an image to the specification's hint form, repacking the rows
    // tightly and dropping Qt's premultiplied alpha. Exposed for testing.
    [[nodiscard]] static FreedesktopImageData toImageData(const QImage &image);

private slots:
    // Connected to the daemon's D-Bus signals by name, so these must stay slots
    // with exactly these signatures.
    void onActionInvoked(quint32 id, const QString &actionKey);
    void onNotificationClosed(quint32 id, quint32 reason);

private:
    void refreshAvailability();
    void requestCapabilities();
    // Records the daemon's capabilities (an empty list when it did not answer)
    // and releases everything that was waiting on them.
    void markCapabilities(const QStringList &capabilities);
    void sendNotify(const Notification &notification);
    void flushDeferred();
    // Sends the message that arrived while this key's id was still unknown.
    void flushQueued(const QString &key);
    void onNotifyReplied(const QString &key, quint32 id);
    void forget(quint32 id);
    [[nodiscard]] bool supports(const char *capability) const;

    NotificationAppInfo m_appInfo;
    bool m_serviceAvailable = false;
    // The daemon's answer to GetCapabilities, and whether it has answered.
    // Whether the body is markup and whether a click can be acted on are both
    // capability questions, so a notification posted before the answer arrives
    // waits for it rather than going out wrong: without this the first message
    // of a session would be the one the user cannot click.
    QStringList m_capabilities;
    bool m_capabilitiesKnown = false;
    QList<Notification> m_deferred;
    QDBusServiceWatcher *m_watcher = nullptr;

    // The live notifications, both ways round, so a click can be routed back to
    // a conversation and a conversation can replace or close its own.
    QHash<QString, quint32> m_idsByKey;
    QHash<quint32, QString> m_keysById;
    // Keys whose Notify call has not yet returned an id. Until it does, the
    // notification has no id to replace, so a second message for that chat
    // waits here rather than going out as a second notification beside the
    // first; a withdraw arriving in the same gap is honoured once the id lands.
    QSet<QString> m_pending;
    QSet<QString> m_withdrawnWhilePending;
    QHash<QString, Notification> m_queuedWhilePending;
};

} // namespace OpenChat

Q_DECLARE_METATYPE(OpenChat::FreedesktopImageData)
