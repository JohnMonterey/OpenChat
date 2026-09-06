#include "call/QtScreenCapture.h"

#include "call/VideoFrameCopy.h"

#include <QGuiApplication>
#include <QPainter>
#include <QScopeGuard>
#include <QScreenCapture>
#include <QVideoFrameFormat>
#include <QWindowCapture>

#include <algorithm>

namespace OpenChat {

namespace {

// Long enough that a genuinely still desktop is never mistaken for a dead one
// (some backends stop producing frames when nothing changes), short enough that
// a closed window or an unplugged monitor is noticed while the user still
// remembers doing it.
constexpr int firstFrameTimeoutMs = 8000;

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
    return kind == Kind::Screen ? !screen.isNull() : window.isValid();
}

QVector<ScreenShareSource> QtScreenCapture::availableSources()
{
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

void QtScreenCapture::pullFrame()
{
    if (!m_requested || !onFrame)
        return;
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
            onFrame(view);
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
        onFrame(view);
}

void QtScreenCapture::fail(const QString &message, bool permanent)
{
    const bool wasRequested = m_requested;
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
    m_requested = false;
    ++m_generation;
    m_timer.stop();
    m_watchdog.stop();
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
