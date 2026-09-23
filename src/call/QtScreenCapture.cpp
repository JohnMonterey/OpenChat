#include "call/QtScreenCapture.h"

#include "call/NativeScreenCapture.h"
#include "call/VideoFrameCopy.h"
#include "diagnostics/BlackBox.h"

#include <QDateTime>
#include <QGuiApplication>
#include <QPainter>
#include <QScopeGuard>
#include <QScreenCapture>
#include <QVideoFrameFormat>
#include <QWindowCapture>

#include <algorithm>
#include <atomic>
#include <exception>
#include <new>

namespace OpenChat {

namespace {

// Long enough that a genuinely still desktop is never mistaken for a dead one
// (some backends stop producing frames when nothing changes), short enough that
// a closed window or an unplugged monitor is noticed while the user still
// remembers doing it.
constexpr int firstFrameTimeoutMs = 8000;

// How often a still screen's last frame is handed to the encoder again. The
// encoder heartbeats once a second and answers a viewer's resend request only
// when it is given a frame, so this bounds how long a repair waits on a
// motionless desktop, at the cost of one hash of an unchanged frame.
constexpr qint64 redeliverIntervalMs = 200;

constexpr const char *area = "screen share";

std::atomic<bool> g_crashOnNextFrame{false};

// For the crash-reporting self test: a genuine invalid write, from inside the
// capture path, so the report shows exactly what a real one would.
[[noreturn]] void crashForTesting()
{
    BlackBox::record(area, "crash self-test: writing through a null pointer on purpose");
    volatile int *nowhere = nullptr;
    *nowhere = 0x0badf00d;
    std::abort();
}

[[nodiscard]] QString describeSource(const ScreenShareSource &source)
{
    // What a report may say about the source: its kind and size, never its
    // title — a window title is the user's content.
    if (source.kind == ScreenShareSource::Kind::Screen) {
        return QStringLiteral("screen %1 (%2x%3)")
            .arg(source.native ? source.nativeName : source.id)
            .arg(source.size.width())
            .arg(source.size.height());
    }
    return QStringLiteral("a window");
}

[[nodiscard]] bool isDirectlyReadable(QImage::Format format) noexcept
{
    switch (format) {
    case QImage::Format_RGB32:
    case QImage::Format_ARGB32:
    case QImage::Format_ARGB32_Premultiplied:
    case QImage::Format_RGBX8888:
    case QImage::Format_RGBA8888:
    case QImage::Format_RGBA8888_Premultiplied:
        return true;
    default:
        return false;
    }
}

} // namespace

bool ScreenShareSource::isValid() const
{
    if (native)
        return kind == Kind::Screen ? (nativeId != 0 || !nativeName.isEmpty()) : nativeId != 0;
    return kind == Kind::Screen ? !screen.isNull() : window.isValid();
}

QString QtScreenCapture::backendName()
{
    if (NativeScreenCapturePlatform::isAvailable())
        return NativeScreenCapturePlatform::name();
    return QStringLiteral("Qt Multimedia");
}

void QtScreenCapture::crashOnNextFrameForTesting()
{
    g_crashOnNextFrame.store(true);
}

QVector<ScreenShareSource> QtScreenCapture::availableSources()
{
    if (NativeScreenCapturePlatform::isAvailable()) {
        BlackBox::Activity activity(area, "listing screens and windows (native)");
        return NativeScreenCapturePlatform::sources();
    }
    BlackBox::Activity activity(area, "listing screens and windows (Qt Multimedia)");
    QVector<ScreenShareSource> sources;
    const QList<QScreen *> screens = QGuiApplication::screens();
    sources.reserve(screens.size() + 8);
    for (QScreen *screen : screens) {
        if (screen == nullptr)
            continue;
        ScreenShareSource source;
        source.kind = ScreenShareSource::Kind::Screen;
        source.screen = screen;
        source.id = QStringLiteral("screen:") + screen->name();
        source.size = screen->geometry().size() * screen->devicePixelRatio();
        const QString label = screen->name().isEmpty() ? QStringLiteral("Display")
                                                       : screen->name();
        source.name = screens.size() > 1
            ? QStringLiteral("%1 (%2×%3)").arg(label).arg(source.size.width())
                  .arg(source.size.height())
            : QStringLiteral("Entire screen (%1×%2)")
                  .arg(source.size.width()).arg(source.size.height());
        sources.append(source);
    }
    const QList<QCapturableWindow> windows = QWindowCapture::capturableWindows();
    for (const QCapturableWindow &window : windows) {
        if (!window.isValid() || window.description().isEmpty())
            continue;
        ScreenShareSource source;
        source.kind = ScreenShareSource::Kind::Window;
        source.window = window;
        source.name = window.description();
        source.id = QStringLiteral("window:") + window.description();
        sources.append(source);
    }
    return sources;
}

QtScreenCapture::QtScreenCapture(QObject *parent)
    : QObject(parent)
{
    m_session.setVideoSink(&m_sink);
    connect(&m_timer, &QTimer::timeout, this, &QtScreenCapture::pullFrame);
    m_watchdog.setSingleShot(true);
    connect(&m_watchdog, &QTimer::timeout, this, [this] {
        if (m_framesDelivered == 0) {
            // Started without complaint and then produced nothing at all: the
            // signature of a capture API that is present but not working here.
            // Named, so a tester's report says which one.
            fail(QStringLiteral("Screen capture started but never produced a picture (%1). "
                                "Try sharing a different screen or window.")
                     .arg(m_native ? m_native->describe() : QStringLiteral("Qt Multimedia")));
            return;
        }
        fail(QStringLiteral("The screen capture stopped producing frames. "
                            "The window or display may have gone away."));
    });
    setTargetFps(m_targetFps);

    // A monitor can be unplugged mid-share. The capture object would keep a
    // dangling screen, so the share is ended deliberately instead. Guarded
    // because this class is reachable from a console-only process, which has
    // no screens to lose.
    if (qGuiApp == nullptr)
        return;
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, [this](QScreen *screen) {
        if (!m_requested || m_source.kind != ScreenShareSource::Kind::Screen)
            return;
        // A native screen Qt never matched is watched by its own API, which
        // reports the output going away; any other monitor leaving is not ours.
        if (m_source.native && !m_screenMatched)
            return;
        if (m_source.screen.isNull() || m_source.screen == screen)
            fail(QStringLiteral("The display being shared was disconnected."));
    });
}

QtScreenCapture::~QtScreenCapture()
{
    stop();
}

void QtScreenCapture::setTargetFps(int fps)
{
    m_targetFps = std::clamp(fps, 1, 120);
    // Sampled a little above the encoder's rate so its own pacing gate, not the
    // timer's granularity, is what decides which frames go out.
    const int intervalMs = std::max(4, 1000 / (m_targetFps + m_targetFps / 4 + 1));
    if (m_timer.interval() != intervalMs)
        m_timer.setInterval(intervalMs);
}

void QtScreenCapture::start(const ScreenShareSource &source)
{
    stop();
    if (!source.isValid()) {
        fail(QStringLiteral("That screen or window is no longer available."));
        return;
    }
    m_requested = true;
    const quint64 generation = ++m_generation;
    m_source = source;
    m_startedMs = QDateTime::currentMSecsSinceEpoch();
    m_framesDelivered = 0;
    m_lastDeliveryMs = 0;
    m_nativeHadFrame = false;
    m_screenMatched = !source.screen.isNull();

    if (source.native) {
        BlackBox::Activity activity(area, "starting the native capture");
        m_native = NativeScreenCapturePlatform::create(source);
        if (!m_native) {
            fail(QStringLiteral("Screen sharing is not available on this system."), true);
            return;
        }
        const QString described = m_native->describe();
        BlackBox::setContext(area, described);
        BlackBox::record(area, QStringLiteral("starting: ") + described);
        NativeScreenCapture::Failure failure;
        bool started = false;
        // Nothing thrown in a capture API may reach Qt's event loop, which
        // cannot unwind: it would end the process instead of the share.
        try {
            started = m_native->start(failure);
        } catch (const std::exception &error) {
            failure.message = QStringLiteral("Screen capture failed to start: %1")
                                  .arg(QString::fromLocal8Bit(error.what()));
        } catch (...) {
            failure.message = QStringLiteral("Screen capture failed to start (unknown error).");
        }
        if (!started) {
            BlackBox::record(area, QStringLiteral("could not start: ") + failure.message);
            fail(failure.message, failure.permanent);
            return;
        }
        // Now that it is open, the description can name the adapter, the
        // resolution and any fallback that was taken.
        BlackBox::setContext(area, m_native->describe());
        m_watchdog.start(firstFrameTimeoutMs);
        m_timer.start();
        return;
    }

    BlackBox::setContext(area, QStringLiteral("Qt Multimedia, ") + describeSource(source));
    BlackBox::record(area, QStringLiteral("starting Qt Multimedia capture of ")
                               + describeSource(source));
    if (source.kind == ScreenShareSource::Kind::Screen) {
        m_screenCapture = std::make_unique<QScreenCapture>();
        m_screenCapture->setScreen(source.screen);
        connect(m_screenCapture.get(), &QScreenCapture::errorOccurred, this,
                [this, generation](QScreenCapture::Error error, const QString &message) {
                    if (error == QScreenCapture::NoError || generation != m_generation)
                        return;
                    const bool permanent = error == QScreenCapture::CapturingNotSupported;
                    fail(message.isEmpty()
                             ? QStringLiteral("Screen sharing is not available on this system.")
                             : message,
                         permanent);
                }, Qt::QueuedConnection);
        m_session.setScreenCapture(m_screenCapture.get());
        m_screenCapture->start();
    } else {
        m_windowCapture = std::make_unique<QWindowCapture>();
        m_windowCapture->setWindow(source.window);
        connect(m_windowCapture.get(), &QWindowCapture::errorOccurred, this,
                [this, generation](QWindowCapture::Error error, const QString &message) {
                    if (error == QWindowCapture::NoError || generation != m_generation)
                        return;
                    fail(error == QWindowCapture::NotFound
                             ? QStringLiteral("The window being shared was closed.")
                             : (message.isEmpty()
                                    ? QStringLiteral("That window could not be captured.")
                                    : message),
                         error == QWindowCapture::CapturingNotSupported);
                }, Qt::QueuedConnection);
        m_session.setWindowCapture(m_windowCapture.get());
        m_windowCapture->start();
    }
    m_watchdog.start(firstFrameTimeoutMs);
    m_timer.start();
}

void QtScreenCapture::deliver(const ScreenFrameView &view)
{
    if (Q_UNLIKELY(g_crashOnNextFrame.exchange(false)))
        crashForTesting();
    if (m_framesDelivered == 0) {
        BlackBox::record(area, QStringLiteral("first frame: %1x%2, %3 bytes per line, format %4")
                                   .arg(view.width)
                                   .arg(view.height)
                                   .arg(view.bytesPerLine)
                                   .arg(int(view.format)));
    }
    ++m_framesDelivered;
    m_lastDeliveryMs = QDateTime::currentMSecsSinceEpoch();
    BlackBox::Activity activity(area, "encoding and sending a frame");
    onFrame(view);
}

void QtScreenCapture::pullNativeFrame()
{
    BlackBox::Activity activity(area, "pulling a frame from the native capture");
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool redeliver = m_nativeHadFrame && now - m_lastDeliveryMs >= redeliverIntervalMs;
    // The sink may end the share (a failure in the encoder, a stop from the
    // UI), which destroys the capture it is being called from; the generation
    // says whether it is still this share afterwards.
    const quint64 generation = m_generation;
    NativeScreenCapture::Failure failure;
    NativeScreenCapture::Pull result = NativeScreenCapture::Pull::Failed;
    // A stop requested from inside the sink must not destroy the capture while
    // its pull() is still on the stack; teardown() parks it here instead.
    m_pulling = true;
    try {
        result = m_native->pull(
            redeliver,
            [this, generation](const ScreenFrameView &view) {
                if (generation == m_generation && view.isValid())
                    deliver(view);
            },
            failure);
    } catch (const std::bad_alloc &) {
        failure.message = QStringLiteral("Screen sharing ran out of memory and was stopped.");
    } catch (const std::exception &error) {
        failure.message = QStringLiteral("Screen capture failed: %1")
                              .arg(QString::fromLocal8Bit(error.what()));
    } catch (...) {
        failure.message = QStringLiteral("Screen capture failed (unknown error).");
    }
    m_pulling = false;
    m_retiredNative.clear();
    if (generation != m_generation)
        return;
    switch (result) {
    case NativeScreenCapture::Pull::Delivered:
        m_nativeHadFrame = true;
        m_watchdog.start(firstFrameTimeoutMs);
        break;
    case NativeScreenCapture::Pull::Unchanged:
        // Before the first frame, "nothing new" is not evidence of life: a
        // capture that never produces anything must still be noticed.
        if (m_nativeHadFrame)
            m_watchdog.start(firstFrameTimeoutMs);
        break;
    case NativeScreenCapture::Pull::Failed:
        BlackBox::record(area, QStringLiteral("capture failed: ") + failure.message);
        fail(failure.message, failure.permanent);
        break;
    }
}

void QtScreenCapture::pullFrame()
{
    if (!m_requested || !onFrame)
        return;
    if (m_native) {
        pullNativeFrame();
        return;
    }
    BlackBox::Activity activity(area, "pulling a frame (Qt Multimedia)");
    const QVideoFrame frame = m_sink.videoFrame();
    if (!frame.isValid())
        return;
    // A still desktop hands out the same frame over and over. Recognising it
    // here means a motionless share costs one comparison, not a hash of every
    // pixel on the display. A backend that does not stamp its frames simply
    // falls through, and the encoder's own change detection catches it.
    const qint64 stamp = frame.startTime();
    if (stamp >= 0 && stamp == m_lastFrameTime)
        return;
    m_lastFrameTime = stamp;
    m_watchdog.start(firstFrameTimeoutMs);

    // A frame that still lives in a GPU texture has to come down to memory
    // before it can be read; the camera path's existing helper is the one place
    // that knows how to do that safely. Desktop capture normally delivers CPU
    // frames, so this is the exception, not the rule.
    QVideoFrame readable = frame;
    if (frame.handleType() != QVideoFrame::NoHandle) {
        readable = copyVideoFrameToMemory(frame);
        if (!readable.isValid())
            return;
    }

    const QImage::Format imageFormat =
        QVideoFrameFormat::imageFormatFromPixelFormat(readable.pixelFormat());
    if (isDirectlyReadable(imageFormat)) {
        if (!readable.map(QVideoFrame::ReadOnly))
            return;
        const auto unmap = qScopeGuard([&readable] { readable.unmap(); });
        ScreenFrameView view;
        view.bits = readable.bits(0);
        view.bytesPerLine = readable.bytesPerLine(0);
        view.width = readable.width();
        view.height = readable.height();
        view.format = imageFormat;
        if (view.isValid()) {
            // The whole point of the direct path: the encoder reads the
            // compositor's own buffer and nothing is copied at all.
            deliver(view);
            return;
        }
        // An unusably aligned buffer falls through to the conversion below
        // rather than being read wrongly.
    }

    // Planar or otherwise unreadable: convert once into a buffer that is reused
    // for the life of the share rather than allocated per frame. Qt reuses the
    // destination when the size and format already match.
    const QImage converted = readable.toImage();
    if (converted.isNull())
        return;
    if (m_conversion.size() != converted.size()
        || m_conversion.format() != QImage::Format_RGB32) {
        m_conversion = QImage(converted.size(), QImage::Format_RGB32);
    }
    if (converted.format() == QImage::Format_RGB32) {
        m_conversion = converted;
    } else {
        QPainter painter(&m_conversion);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.drawImage(0, 0, converted);
    }
    const ScreenFrameView view = ScreenFrameView::fromImage(m_conversion);
    if (view.isValid())
        deliver(view);
}

void QtScreenCapture::fail(const QString &message, bool permanent)
{
    const bool wasRequested = m_requested;
    BlackBox::record(area, QStringLiteral("share ended by failure%1: %2")
                               .arg(permanent ? QStringLiteral(" (permanent)") : QString())
                               .arg(message));
    teardown();
    if (wasRequested || permanent)
        emit failed(message, permanent);
}

void QtScreenCapture::stop()
{
    teardown();
}

void QtScreenCapture::teardown()
{
    if (m_requested) {
        BlackBox::record(area, QStringLiteral("stopped after %1 s, %2 frames delivered")
                                   .arg((QDateTime::currentMSecsSinceEpoch() - m_startedMs) / 1000)
                                   .arg(m_framesDelivered));
    }
    m_requested = false;
    ++m_generation;
    m_timer.stop();
    m_watchdog.stop();
    if (m_native) {
        BlackBox::Activity activity(area, "stopping the native capture");
        if (m_pulling)
            m_retiredNative.push_back(std::move(m_native));
        else
            m_native.reset();
    }
    BlackBox::setContext(area, QString());
    // Order matters: the capture object goes down before the session that holds
    // it, and the sink is emptied last so nothing is left holding a frame.
    if (m_screenCapture) {
        m_screenCapture->stop();
        m_session.setScreenCapture(nullptr);
        m_screenCapture.reset();
    }
    if (m_windowCapture) {
        m_windowCapture->stop();
        m_session.setWindowCapture(nullptr);
        m_windowCapture.reset();
    }
    m_sink.setVideoFrame(QVideoFrame());
    m_lastFrameTime = -1;
    // The conversion buffer is a full desktop. It is released outright rather
    // than kept warm for a share that may never happen again.
    m_conversion = QImage();
    m_source = ScreenShareSource();
}

} // namespace OpenChat
