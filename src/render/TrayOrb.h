#pragma once

#include <QIcon>
#include <QImage>
#include <QStringView>

// The notification-area icon during a call: a glossy Aero orb whose colour
// says what this user's microphone is doing. Dark green in the call, bright
// green while talking, red when muted and dark grey when deafened.
namespace OpenChat::TrayOrb {

// One orb `side` pixels square. `state` is "call", "talking", "muted" or
// "deafened"; anything else draws nothing and returns a null image.
[[nodiscard]] QImage render(QStringView state, int side);

// The orb drawn at every size a notification area asks for (16 px at 100 %
// up to 64 px), so no tray has to scale one. Null for an unknown state.
[[nodiscard]] QIcon icon(QStringView state);

} // namespace OpenChat::TrayOrb
