#pragma once

#include "notify/NotificationBackend.h"

namespace OpenChat {

// The backend for a desktop this build cannot notify on: an unsupported
// platform, or a supported one whose notification service is missing. It
// accepts and drops everything, so the rest of the application needs no
// "notifications might not exist" branch of its own.
class NullNotifier final : public NotificationBackend
{
    Q_OBJECT

public:
    using NotificationBackend::NotificationBackend;

    [[nodiscard]] bool isAvailable() const override { return false; }
    [[nodiscard]] QString name() const override { return QStringLiteral("null"); }
    void show(const Notification &) override {}
    void withdraw(const QString &) override {}
    void withdrawAll() override {}
};

} // namespace OpenChat
