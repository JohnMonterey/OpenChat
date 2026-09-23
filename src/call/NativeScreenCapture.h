#pragma once

#include "call/CallScreenSession.h"

#include <QString>
#include <QVector>

#include <functional>
#include <memory>

namespace OpenChat {

struct ScreenShareSource;

// A desktop capture written against the operating system's own API.
//
// Qt Multimedia captures screens and windows only through its FFmpeg backend,
// and the builds this application actually ships on do not have it: the
// Windows package carries the Media Foundation backend alone, and Homebrew's
// macOS Qt is configured with FFmpeg switched off. On those, QScreenCapture
// accepts start() and then never produces a frame or an error. So Windows and
// macOS capture here instead — DXGI Desktop Duplication and
// Windows.Graphics.Capture on one, ScreenCaptureKit on the other — and
// QtScreenCapture keeps Qt's own path for Linux, where the FFmpeg backend is
// what the distributions ship.
//
// The contract is the Qt path's: frames are pulled by QtScreenCapture's pacing
// timer, never pushed, so nothing queues behind a busy encoder, and a frame is
// handed to the sink while it is mapped and is not retained by anybody else.
class NativeScreenCapture
{
public:
    enum class Pull {
        // A frame was handed to the sink.
        Delivered,
        // The capture is alive and nothing has changed since the last frame.
        Unchanged,
        // The capture is over; the failure says why.
        Failed,
    };

    struct Failure final {
        QString message;
        // True when the answer will not change on this machine (the operating
        // system cannot capture at all), rather than this source or this
        // attempt having failed.
        bool permanent = false;
    };

    using FrameSink = std::function<void(const ScreenFrameView &)>;

    virtual ~NativeScreenCapture() = default;

    // Which API and which source, for diagnostics and crash reports. Never a
    // window title: that is the user's content.
    [[nodiscard]] virtual QString describe() const = 0;
    [[nodiscard]] virtual bool start(Failure &failure) = 0;
    // Hands the newest frame to `sink` if there is one. With `redeliver`, the
    // last frame is handed over again even when nothing changed: the encoder
    // only heartbeats and answers resend requests when it is given a frame, and
    // these APIs, unlike Qt's X11 grabber, go quiet while the screen is still.
    [[nodiscard]] virtual Pull pull(bool redeliver, const FrameSink &sink, Failure &failure) = 0;
};

// Whether the operating system lets this process see other applications'
// pixels. Only macOS ever says no; it asks the user once per application.
enum class ScreenCaptureAccess { Granted, Denied };

namespace NativeScreenCapturePlatform {

// True when a native path is compiled in, this OS version supports it, and
// OPENCHAT_SCREEN_CAPTURE=qt has not asked for Qt's path instead.
[[nodiscard]] bool isAvailable();
// The API by name, e.g. "Windows Desktop Duplication". Empty when unavailable.
[[nodiscard]] QString name();
// Every screen, then every window, this path can capture right now.
[[nodiscard]] QVector<ScreenShareSource> sources();
[[nodiscard]] std::unique_ptr<NativeScreenCapture> create(const ScreenShareSource &source);

// Whether sharing is permitted at all, regardless of which path captures.
// With `request`, the operating system is asked to prompt the user if it has
// not done so before.
[[nodiscard]] ScreenCaptureAccess access(bool request);
// What to tell the user when access is denied, including how to grant it.
[[nodiscard]] QString accessDeniedMessage();
// Opens the system settings page where access is granted. False where there is
// no such page.
bool openAccessSettings();

} // namespace NativeScreenCapturePlatform

} // namespace OpenChat
