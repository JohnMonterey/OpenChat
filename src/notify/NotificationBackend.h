#pragma once

#include "notify/Notification.h"

#include <QObject>
#include <QString>

#include <memory>

namespace OpenChat {

// The seam between the notification policy and one desktop's notification
// service. Every platform gets one implementation; NotificationService owns
// exactly one and never knows which.
//
// Implementations must be safe to construct on a desktop that has no working
// notification service: report false from isAvailable() and drop show() calls
// rather than failing. Nothing about a chat client should depend on the
// notification daemon being up.
class NotificationBackend : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;
    ~NotificationBackend() override = default;

    // Whether notifications posted here can actually reach the user. Checked on
    // every post, because a service can come and go while the app runs (a
    // restarted notification daemon, a revoked macOS authorization).
    [[nodiscard]] virtual bool isAvailable() const = 0;

    // Which desktop service this talks to, for diagnostics only
    // ("freedesktop", "macos", "windows-toast", "windows-tray", "null").
    [[nodiscard]] virtual QString name() const = 0;

    // Posts a notification, replacing any still-visible one with the same key.
    virtual void show(const Notification &notification) = 0;

    // Takes back a still-visible notification, used when the user opens the
    // conversation it was about. A key with nothing showing is not an error.
    virtual void withdraw(const QString &key) = 0;

    // Takes back everything this backend still has on screen. Called on
    // shutdown so a closed application leaves no notifications behind.
    virtual void withdrawAll() = 0;

signals:
    // The user clicked the notification for `key`. The application brings
    // itself forward and opens that conversation.
    void activated(const QString &key);
};

// Builds the backend for the platform this binary was compiled for. Never
// returns null: where no service exists, or where this build has no support for
// one, it returns a backend that reports itself unavailable and drops posts.
[[nodiscard]] std::unique_ptr<NotificationBackend> makeNotificationBackend(
    const NotificationAppInfo &appInfo, QObject *parent = nullptr);

} // namespace OpenChat
