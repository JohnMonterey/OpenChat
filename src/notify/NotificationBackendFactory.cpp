#include "notify/NotificationBackend.h"

#include "notify/NullNotifier.h"

#include <QtGlobal>

#if defined(Q_OS_MACOS)
#    include "notify/MacNotifier.h"
#elif defined(Q_OS_WIN)
#    include "notify/WindowsNotifier.h"
#elif defined(Q_OS_UNIX)
#    include "notify/FreedesktopNotifier.h"
#endif

namespace OpenChat {

// Which desktop service to talk to is a compile-time question — the three
// implementations have nothing in common and only one of them will link — but
// whether that service is actually reachable is a run-time one, which is why
// every backend answers isAvailable() for itself rather than being chosen away
// here. On the freedesktop desktops the same backend serves Wayland and X11:
// notifications travel over the session bus, not the display connection.
std::unique_ptr<NotificationBackend> makeNotificationBackend(const NotificationAppInfo &appInfo,
                                                             QObject *parent)
{
#if defined(Q_OS_MACOS)
    return std::make_unique<MacNotifier>(appInfo, parent);
#elif defined(Q_OS_WIN)
    return std::make_unique<WindowsNotifier>(appInfo, parent);
#elif defined(Q_OS_UNIX)
    return std::make_unique<FreedesktopNotifier>(appInfo, parent);
#else
    Q_UNUSED(appInfo);
    return std::make_unique<NullNotifier>(parent);
#endif
}

} // namespace OpenChat
