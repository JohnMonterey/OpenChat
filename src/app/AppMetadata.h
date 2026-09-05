#pragma once

#include <QStringView>

namespace OpenChat::AppMetadata {

inline constexpr auto name = QStringView(u"OpenChat");
// The basename of the installed .desktop file (deploy/openchat.desktop). The
// freedesktop desktops use it as the Wayland app id and as the key a
// notification daemon looks the application's name and icon up under.
inline constexpr auto desktopEntry = QStringView(u"openchat");
// The Application User Model ID Windows attributes toasts to. Must match the
// Start Menu shortcut the notification backend registers.
inline constexpr auto appUserModelId = QStringView(u"OpenChat.Desktop.Client");
inline constexpr int defaultWidth = 860;
inline constexpr int defaultHeight = 680;
inline constexpr int minimumWidth = 720;
inline constexpr int minimumHeight = 560;

} // namespace OpenChat::AppMetadata
