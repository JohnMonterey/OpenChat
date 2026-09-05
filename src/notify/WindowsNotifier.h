#pragma once

#include "notify/NotificationBackend.h"

#include <memory>

namespace OpenChat {

// Desktop notifications on Windows.
//
// The preferred route is a Windows toast, which draws the sender's picture as
// the toast's app-logo override beside the name and the message — the same
// arrangement as the other platforms. Toasts from an unpackaged desktop
// application must be attributed to an Application User Model ID that a Start
// Menu shortcut declares, so the backend registers one for OpenChat on first
// use; without it Windows silently discards the toast.
//
// Where this build has no C++/WinRT available (a MinGW toolchain, say), the
// backend falls back to a shell notification-area balloon carrying the picture
// as its icon. Windows 10 and 11 render those as toasts too, so the user still
// gets a notification, just a plainer one.
//
// The header stays free of Windows headers so it can be included anywhere;
// everything platform specific lives behind the private implementation.
class WindowsNotifier final : public NotificationBackend
{
    Q_OBJECT

public:
    explicit WindowsNotifier(NotificationAppInfo appInfo, QObject *parent = nullptr);
    ~WindowsNotifier() override;

    [[nodiscard]] bool isAvailable() const override;
    [[nodiscard]] QString name() const override;
    void show(const Notification &notification) override;
    void withdraw(const QString &key) override;
    void withdrawAll() override;

    // Called from the window procedure when the user clicks a balloon. Public
    // because that callback is a free function; not part of the interface.
    void reportActivated(const QString &key);

private:
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace OpenChat
