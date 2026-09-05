# Desktop notifications

An inbound message raises a desktop notification carrying the sender's picture,
their name and what they wrote — the same arrangement on every platform:

```
┌──────────┐
│          │  Michael
│ sender   │
│ picture  │    Are we still on for tonight?
└──────────┘
```

Clicking it brings the window forward, leaves whatever section was open, and
opens that conversation.

## Shape

`ChatController` emits `messageNotificationRequested` for every inbound message,
carrying the chat id, the sender's display name, the message body and the
sender's avatar key. `NotificationService` decides whether that is worth
interrupting the user for and what it should say; one `NotificationBackend`
talks to the desktop. Only the backend is platform specific, so every judgement
call is made once and tested once, in `tests/tst_notifications.cpp`.

The picture is drawn by `AvatarPainter`, the same code the sidebar and the call
screen draw with, so a notification shows exactly the picture the interface
shows — a received JPEG, the bundled artwork, or the procedural placeholder —
rendered off-screen at 96 px with the interface's corner rounding.

## What is announced

A message is not announced while its conversation is already open in a focused
window. It **is** announced when that conversation is open behind another window:
whether the user is looking at it is a question about window focus, which the
controller cannot see and `main.cpp` feeds in from the window's `activeChanged`.
Opening a conversation withdraws the notification that was about it. Our own
outgoing messages are never announced.

A second message from the same contact replaces the first rather than stacking,
so one talkative contact cannot bury the desktop. Messages from more than four
distinct chats within five seconds collapse into a single summary that replaces
itself; clicking it opens the most recent of them.

Titles are cut at 64 characters and bodies at 220, on a word boundary where one
is near. In the session states that withhold message plaintext from the
interface itself (Locked, Quarantined, DeviceChanged) the controller sends an
empty body and the notification says a message arrived without repeating it: a
notification daemon is a different trust domain from the encrypted store the
message came out of, and the threat model lists plaintext notifications as a
protected asset.

## Per platform

**Linux and the other freedesktop desktops** (`FreedesktopNotifier`) talk to
`org.freedesktop.Notifications` on the session bus. That is a D-Bus protocol
rather than a display protocol, so one implementation reaches GNOME Shell,
Plasma, mako and dunst identically, and **Wayland and X11 need no separate
code**. The picture travels in the standard `image-data` hint as straight
non-premultiplied RGBA with tightly packed rows. The daemon is asked what it
supports before the first notification goes out, because whether a click can be
delivered and whether the body is parsed as markup are both capability
questions; a message posted before that answer arrives waits for it, and a
daemon that never answers is given up on after 1.5 seconds. Bodies are HTML
escaped only where the daemon declares `body-markup`. The daemon may be absent
at startup, appear later, or restart, and ownership of the name is watched
rather than sampled once.

Install `deploy/openchat.desktop` (the `install` target does) so the desktop can
attribute notifications to OpenChat and put its icon on them; without it they
still appear, under the plain application name.

**macOS** (`MacNotifier`) uses `UNUserNotificationCenter`. The picture is a
notification attachment, which is what Notification Centre draws beside the
title and body. The framework needs the process to have a bundle identity; this
build links its `Info.plist` into the executable, so a plain binary has one.
Authorization is requested on first use, and a denied or unavailable centre
means posts are dropped rather than anything failing.

**Windows** (`WindowsNotifier`) posts a toast whose `appLogoOverride` image is
the sender's picture, circle-cropped. An unpackaged desktop application only
owns the Application User Model ID a toast is attributed to if a Start Menu
shortcut declares it, so one is registered on first use; without it Windows
discards the toast silently. Toolchains without C++/WinRT fall back to a shell
notification-area balloon carrying the picture as its icon, which Windows 10 and
11 render as a toast as well. The build detects which it has and links to match.

## Checking it

`tests/tst_notifications.cpp` covers the policy against a recording backend and
needs no desktop. `tests/tst_freedesktopnotifier.cpp` starts a private session
bus, owns `org.freedesktop.Notifications` on it, and drives the real Linux
backend across it: the arguments the daemon receives, replacing and closing by
id, and the click coming back. It skips itself where `dbus-daemon` is absent.

What no test can do is confirm a real desktop draws it, so:

```
openchat-notify-check                        # one notification, default text
openchat-notify-check --avatar ~/face.jpg    # with a real picture
openchat-notify-check --burst 7              # burst collapsing
```

It prints which backend was chosen, whether the desktop accepted the post, and
whether the notification was clicked. Run it on each platform.
