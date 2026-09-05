#pragma once

#include "notify/NotificationBackend.h"

#include <memory>

namespace OpenChat {

// Desktop notifications on macOS, through UNUserNotificationCenter.
//
// The sender's picture is delivered as a notification attachment, which is what
// Notification Centre draws to the left of the title and body, giving the same
// arrangement as the other platforms. The image is written to a temporary file
// per notification because the framework takes ownership of an attachment's
// file when the request is accepted.
//
// The framework requires the process to have a bundle identity. This build
// links its Info.plist into the executable, so a plain binary has one; if the
// centre is nevertheless unavailable (an unsigned build, or a user who has
// denied authorization) the backend reports itself unavailable and drops posts
// rather than failing.
//
// The header stays free of Objective-C so it can be included from ordinary C++
// translation units; everything Cocoa lives behind the private implementation.
class MacNotifier final : public NotificationBackend
{
    Q_OBJECT

public:
    explicit MacNotifier(NotificationAppInfo appInfo, QObject *parent = nullptr);
    ~MacNotifier() override;

    [[nodiscard]] bool isAvailable() const override;
    [[nodiscard]] QString name() const override;
    void show(const Notification &notification) override;
    void withdraw(const QString &key) override;
    void withdrawAll() override;

    // Called by the notification-centre delegate when the user taps a
    // notification. Public because the delegate is an Objective-C class that
    // cannot be a friend; not part of the backend interface.
    void reportActivated(const QString &key);

private:
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace OpenChat
