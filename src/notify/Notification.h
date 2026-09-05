#pragma once

#include <QImage>
#include <QString>

namespace OpenChat {

// What a notification is about. Backends map this to the platform's own idea of
// a notification kind, which is what lets a desktop group, sort or silence them
// sensibly.
enum class NotificationCategory {
    Message,
    ContactRequest,
    IncomingCall,
};

// One desktop notification, already reduced to what every platform can show:
// a title, a body and a square picture.
//
// The interface the request asks for is the same everywhere — the sender's
// picture on the left, their name above their message:
//
//     ┌──────────┐
//     │          │  Michael
//     │ sender   │
//     │ picture  │    Are we still on for tonight?
//     └──────────┘
//
// so this struct carries exactly those three things and nothing platform
// specific. `key` is both the routing identity (which conversation to open when
// the notification is clicked) and the coalescing identity: a second
// notification with the same key replaces the first rather than stacking, so a
// talkative contact cannot bury the rest of the desktop.
struct Notification final {
    QString key;
    QString title;
    QString body;
    QImage image;
    NotificationCategory category = NotificationCategory::Message;
};

// How the application identifies itself to the desktop's notification service.
// Each platform needs a different one of these to attribute a notification to
// OpenChat (and to show its icon next to one), so all four are carried
// together and the backend picks what it needs.
struct NotificationAppInfo final {
    // Human-readable name, shown by every platform.
    QString applicationName = QStringLiteral("OpenChat");
    // Freedesktop: the basename of the installed .desktop file, which is how
    // GNOME and KDE find the application's icon and name.
    QString desktopEntry = QStringLiteral("openchat");
    // Windows: the Application User Model ID the toast is attributed to. Must
    // match the Start Menu shortcut the backend registers.
    QString appUserModelId = QStringLiteral("OpenChat.Desktop.Client");
    // Freedesktop: an icon name or path used when a notification carries no
    // picture of its own.
    QString iconName = QStringLiteral("openchat");
};

} // namespace OpenChat
