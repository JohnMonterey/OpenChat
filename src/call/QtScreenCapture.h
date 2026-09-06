#pragma once

#include "call/CallScreenSession.h"

#include <QCapturableWindow>
#include <QImage>
#include <QMediaCaptureSession>
#include <QObject>
#include <QPointer>
#include <QScreen>
#include <QSize>
#include <QString>
#include <QTimer>
#include <QVector>
#include <QVideoFrame>
#include <QVideoSink>

#include <functional>
#include <memory>

QT_BEGIN_NAMESPACE
class QScreenCapture;
class QWindowCapture;
QT_END_NAMESPACE

namespace OpenChat {

// One thing the user can choose to share.
struct ScreenShareSource final {
    enum class Kind { Screen, Window };

    Kind kind = Kind::Screen;
    // Stable enough to re-select across an enumeration; not persisted.
    QString id;
    QString name;
    QSize size;
    // Exactly one of these is set, according to `kind`. The screen is held
    // weakly: a monitor can be unplugged while its row is still on screen.
    QPointer<QScreen> screen;
    QCapturableWindow window;

    [[nodiscard]] bool isValid() const;
};

// The desktop capture half of screen sharing.
//
// Frames are taken from the platform's own capture path — the compositor's, not
// a screenshot loop — and are handed on WITHOUT being copied: the frame is
// mapped read only, the encoder reads the tiles it needs straight out of that
// mapping, and it is unmapped before this returns. Nothing is retained after
// the callback, so the pipeline never holds a desktop-sized buffer of its own.
//
// The send rate is this class's decision, not the display's. A 240 Hz monitor
// offers frames at 240 Hz; a timer running at the encoder's target rate pulls
// the newest one and everything else is simply never looked at. Frames are
// pulled rather than pushed for the same reason the camera pulls: nothing can
// queue up behind a busy encoder, so what goes out is always the newest picture
// rather than the oldest stale one.
class QtScreenCapture final : public QObject
{
    Q_OBJECT

public:
    explicit QtScreenCapture(QObject *parent = nullptr);
    ~QtScreenCapture() override;

    QtScreenCapture(const QtScreenCapture &) = delete;
    QtScreenCapture &operator=(const QtScreenCapture &) = delete;

    // Everything shareable right now: every connected screen, then every
    // capturable window. Cheap enough to call each time the picker opens, which
    // is also the only way a closed window leaves the list.
    [[nodiscard]] static QVector<ScreenShareSource> availableSources();

    // Borrowed, valid only for the duration of the call. Invoked on the thread
    // this object lives on, synchronously from the pacing timer.
    std::function<void(const ScreenFrameView &)> onFrame;

    void start(const ScreenShareSource &source);
    void stop();
    [[nodiscard]] bool isActive() const noexcept { return m_requested; }
    [[nodiscard]] const ScreenShareSource &source() const noexcept { return m_source; }

    // The rate the encoder wants. Applied to the pacing timer immediately, so a
    // share whose viewer closed their window drops to a trickle at once.
    void setTargetFps(int fps);
    [[nodiscard]] int targetFps() const noexcept { return m_targetFps; }

signals:
    // Capture could not start or could not continue. The caller is expected to
    // put the UI back and release everything; nothing here retries by itself.
    // `permanent` marks the answer that will not change on this machine — the
    // platform has no capture at all — so the button can say so once instead of
    // failing again on every press.
    void failed(const QString &message, bool permanent);

private:
    void pullFrame();
    void teardown();
    void fail(const QString &message, bool permanent = false);

    bool m_requested = false;
    // Bumped on every start/stop so a callback from a previous attempt that is
    // still in flight recognises itself as stale and does nothing.
    quint64 m_generation = 0;
    int m_targetFps = 30;
    ScreenShareSource m_source;

    QMediaCaptureSession m_session;
    QVideoSink m_sink;
    std::unique_ptr<QScreenCapture> m_screenCapture;
    std::unique_ptr<QWindowCapture> m_windowCapture;
    QTimer m_timer;
    // Fires when the capture is up but no frame has arrived for long enough
    // that the source is gone rather than merely still.
    QTimer m_watchdog;
    // The most recent frame's presentation time, so an unchanged frame is not
    // re-hashed. Deliberately not the frame itself: holding one would keep a
    // whole desktop's worth of the compositor's buffer alive between pulls,
    // for the sake of an equality test a timestamp answers just as well.
    qint64 m_lastFrameTime = -1;
    // Only used for capture formats that are not directly readable as a 32-bit
    // QImage; the common desktop formats never touch it.
    QImage m_conversion;
};

} // namespace OpenChat
