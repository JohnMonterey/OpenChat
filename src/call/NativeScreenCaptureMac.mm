#include "call/NativeScreenCapture.h"

#include "call/QtScreenCapture.h"
#include "diagnostics/BlackBox.h"

#include <QDesktopServices>
#include <QGuiApplication>
#include <QScreen>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <mutex>

#include <unistd.h>

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

// ScreenCaptureKit (macOS 12.3) for displays and single windows.
//
// Qt's own capture cannot be relied on here: Homebrew's Qt is built with the
// FFmpeg multimedia backend switched off, which leaves QScreenCapture with no
// implementation at all, and the official Qt captures through
// AVCaptureScreenInput and CGWindowListCreateImage, both deprecated. On macOS
// older than 12.3 isAvailable() is false and QtScreenCapture falls back to Qt.
//
// Frames arrive on ScreenCaptureKit's own queue. The newest is kept (retained,
// not copied) until the pacing timer pulls it on the main thread, so, exactly
// as on the other platforms, nothing ever queues up behind the encoder.

namespace {

constexpr const char *area = "screen share";

// The longest edge asked of ScreenCaptureKit, the encoder's own ceiling
// (ScreenShareTuning::maxOutputEdge): scaling on the GPU here is free, and a
// 5K display read back at full size would only be scaled down again.
constexpr double maxCaptureEdge = 1920.0;

constexpr NSInteger userDeclinedError = -3801; // SCStreamErrorUserDeclined

QString fromNSString(NSString *text)
{
    return text == nil ? QString() : QString::fromNSString(text);
}

QString describeError(NSError *error)
{
    if (error == nil)
        return QStringLiteral("unknown error");
    return QStringLiteral("%1 (%2 %3)")
        .arg(fromNSString(error.localizedDescription), fromNSString(error.domain))
        .arg(error.code);
}

// Even sizes, fitted within the encoder's ceiling, never zero.
void fitSize(double width, double height, size_t &outWidth, size_t &outHeight)
{
    const double scale = std::min(1.0, maxCaptureEdge / std::max(1.0, std::max(width, height)));
    outWidth = std::max<size_t>(2, size_t(std::lround(width * scale)) & ~size_t(1));
    outHeight = std::max<size_t>(2, size_t(std::lround(height * scale)) & ~size_t(1));
}

QScreen *matchQtScreen(CGDirectDisplayID display)
{
    if (qGuiApp == nullptr)
        return nullptr;
    // Both in points, both with the origin at the main display's top-left.
    const CGRect bounds = CGDisplayBounds(display);
    const QRect rect(int(bounds.origin.x), int(bounds.origin.y), int(bounds.size.width),
                     int(bounds.size.height));
    for (QScreen *screen : QGuiApplication::screens()) {
        if (screen != nullptr && screen->geometry() == rect)
            return screen;
    }
    return nullptr;
}

QSize pixelSizeOf(CGDirectDisplayID display)
{
    CGDisplayModeRef mode = CGDisplayCopyDisplayMode(display);
    if (mode == nullptr) {
        const CGRect bounds = CGDisplayBounds(display);
        return QSize(int(bounds.size.width), int(bounds.size.height));
    }
    const QSize size(int(CGDisplayModeGetPixelWidth(mode)), int(CGDisplayModeGetPixelHeight(mode)));
    CGDisplayModeRelease(mode);
    return size;
}

double largestBackingScale()
{
    double scale = 1.0;
    for (NSScreen *screen in [NSScreen screens])
        scale = std::max(scale, double(screen.backingScaleFactor));
    return scale;
}

} // namespace

// Receives ScreenCaptureKit's frames and its stop notice, on its queue, and
// keeps only the newest picture for the main thread to take.
API_AVAILABLE(macos(12.3))
@interface OpenChatScreenStreamOutput : NSObject <SCStreamOutput, SCStreamDelegate>
// +1 retained, or NULL when nothing new arrived since the last take.
- (CVPixelBufferRef)takeLatest CF_RETURNS_RETAINED;
- (NSError *)stopError;
@end

@implementation OpenChatScreenStreamOutput {
    std::mutex _lock;
    CVPixelBufferRef _latest;
    NSError *_stopError;
}

- (void)dealloc
{
    if (_latest != NULL)
        CVPixelBufferRelease(_latest);
}

- (void)stream:(SCStream *)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
                   ofType:(SCStreamOutputType)type
{
    (void)stream;
    if (type != SCStreamOutputTypeScreen || !CMSampleBufferIsValid(sampleBuffer))
        return;
    // Only a complete frame carries new pixels; idle and blank ones say that
    // nothing changed or that nothing is visible.
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, false);
    if (attachments == NULL || CFArrayGetCount(attachments) == 0)
        return;
    NSDictionary *info = (__bridge NSDictionary *)CFArrayGetValueAtIndex(attachments, 0);
    NSNumber *status = info[SCStreamFrameInfoStatus];
    if (status == nil)
        return;
    // Stopped is how a closed window can end a stream without the delegate
    // hearing about it; treat it as the end rather than as "nothing new",
    // which would keep redelivering the last picture forever.
    if (status.integerValue == SCFrameStatusStopped) {
        std::lock_guard<std::mutex> guard(_lock);
        if (_stopError == nil) {
            _stopError = [NSError errorWithDomain:@"OpenChat"
                                             code:0
                                         userInfo:@{NSLocalizedDescriptionKey : @"The content stopped."}];
        }
        return;
    }
    if (status.integerValue != SCFrameStatusComplete)
        return;
    CVImageBufferRef buffer = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (buffer == NULL)
        return;
    CVPixelBufferRetain(buffer);
    std::lock_guard<std::mutex> guard(_lock);
    if (_latest != NULL)
        CVPixelBufferRelease(_latest);
    _latest = buffer;
}

- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error
{
    (void)stream;
    std::lock_guard<std::mutex> guard(_lock);
    _stopError = error != nil
        ? error
        : [NSError errorWithDomain:@"OpenChat"
                              code:0
                          userInfo:@{NSLocalizedDescriptionKey : @"The capture stopped."}];
}

- (CVPixelBufferRef)takeLatest
{
    std::lock_guard<std::mutex> guard(_lock);
    CVPixelBufferRef latest = _latest;
    _latest = NULL;
    return latest;
}

- (NSError *)stopError
{
    std::lock_guard<std::mutex> guard(_lock);
    return _stopError;
}

@end

namespace OpenChat {

namespace {

// ScreenCaptureKit answers asynchronously; the picker and start() need an
// answer now. Waiting on the main thread is safe because the completion
// handlers run on ScreenCaptureKit's own queues.
API_AVAILABLE(macos(12.3))
SCShareableContent *fetchShareableContent(NSError **errorOut)
{
    __block SCShareableContent *result = nil;
    __block NSError *failure = nil;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [SCShareableContent
        getShareableContentExcludingDesktopWindows:YES
                               onScreenWindowsOnly:YES
                                 completionHandler:^(SCShareableContent *content, NSError *error) {
                                     result = content;
                                     failure = error;
                                     dispatch_semaphore_signal(done);
                                 }];
    if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) != 0) {
        if (errorOut != nullptr) {
            *errorOut = [NSError errorWithDomain:@"OpenChat"
                                            code:1
                                        userInfo:@{NSLocalizedDescriptionKey :
                                                       @"macOS did not list the screens in time."}];
        }
        return nil;
    }
    if (errorOut != nullptr)
        *errorOut = failure;
    return result;
}

class ScreenCaptureKitCapture final : public NativeScreenCapture
{
public:
    explicit ScreenCaptureKitCapture(const ScreenShareSource &source)
        : m_isWindow(source.kind == ScreenShareSource::Kind::Window)
        , m_id(source.nativeId)
        , m_subject(m_isWindow ? QStringLiteral("a window") : source.nativeName)
    {
    }

    ~ScreenCaptureKitCapture() override
    {
        stop();
        if (m_current != NULL)
            CVPixelBufferRelease(m_current);
    }

    QString describe() const override
    {
        QString text = QStringLiteral("ScreenCaptureKit, ") + m_subject;
        if (m_width > 0)
            text += QStringLiteral(", %1x%2").arg(m_width).arg(m_height);
        return text;
    }

    bool start(Failure &failure) override
    {
        BlackBox::Activity activity(area, "starting ScreenCaptureKit");
        if (@available(macOS 12.3, *)) {
            @try {
                return startStream(failure);
            } @catch (NSException *exception) {
                failure.message = QStringLiteral("ScreenCaptureKit refused to start: %1 (%2)")
                                      .arg(fromNSString(exception.reason),
                                           fromNSString(exception.name));
                stop();
                return false;
            }
        }
        failure.message = QStringLiteral("Screen sharing needs macOS 12.3 or later.");
        failure.permanent = true;
        return false;
    }

    Pull pull(bool redeliver, const FrameSink &sink, Failure &failure) override
    {
        if (@available(macOS 12.3, *)) {
            BlackBox::Activity activity(area, "taking a frame from ScreenCaptureKit");
            auto *output = (OpenChatScreenStreamOutput *)m_output;
            if (output == nil) {
                failure.message = QStringLiteral("The screen capture is not running.");
                return Pull::Failed;
            }
            if (NSError *stopped = [output stopError]) {
                failure.message = stopped.code == userDeclinedError
                    ? NativeScreenCapturePlatform::accessDeniedMessage()
                    : (m_isWindow ? QStringLiteral("The window being shared went away: %1")
                                  : QStringLiteral("macOS stopped the screen capture: %1"))
                          .arg(describeError(stopped));
                return Pull::Failed;
            }
            CVPixelBufferRef fresh = [output takeLatest];
            if (fresh != NULL) {
                if (m_current != NULL)
                    CVPixelBufferRelease(m_current);
                m_current = fresh;
                return deliver(sink);
            }
            if (redeliver && m_current != NULL)
                return deliver(sink);
            return Pull::Unchanged;
        }
        failure.message = QStringLiteral("Screen sharing needs macOS 12.3 or later.");
        failure.permanent = true;
        return Pull::Failed;
    }

private:
    API_AVAILABLE(macos(12.3))
    bool startStream(Failure &failure)
    {
        NSError *error = nil;
        SCShareableContent *content = fetchShareableContent(&error);
        if (content == nil) {
            failure.message = error != nil && error.code == userDeclinedError
                ? NativeScreenCapturePlatform::accessDeniedMessage()
                : QStringLiteral("macOS would not list what can be shared: %1").arg(describeError(error));
            return false;
        }

        SCContentFilter *filter = nil;
        SCStreamConfiguration *configuration = [[SCStreamConfiguration alloc] init];
        size_t width = 0;
        size_t height = 0;
        if (m_isWindow) {
            SCWindow *target = nil;
            for (SCWindow *window in content.windows) {
                if (window.windowID == CGWindowID(m_id)) {
                    target = window;
                    break;
                }
            }
            if (target == nil) {
                failure.message = QStringLiteral("The window being shared was closed.");
                return false;
            }
            NSString *application = target.owningApplication.applicationName;
            if (application.length > 0)
                m_subject = QStringLiteral("a window of ") + fromNSString(application);
            filter = [[SCContentFilter alloc] initWithDesktopIndependentWindow:target];
            const double scale = largestBackingScale();
            fitSize(target.frame.size.width * scale, target.frame.size.height * scale, width, height);
            // A window that is resized keeps being drawn whole into the frame.
            configuration.scalesToFit = YES;
        } else {
            SCDisplay *target = nil;
            for (SCDisplay *display in content.displays) {
                if (display.displayID == CGDirectDisplayID(m_id)) {
                    target = display;
                    break;
                }
            }
            if (target == nil) {
                failure.message = QStringLiteral("The display being shared was disconnected.");
                return false;
            }
            filter = [[SCContentFilter alloc] initWithDisplay:target excludingWindows:@[]];
            const QSize pixels = pixelSizeOf(target.displayID);
            fitSize(pixels.width(), pixels.height(), width, height);
        }
        m_width = int(width);
        m_height = int(height);
        configuration.width = width;
        configuration.height = height;
        configuration.pixelFormat = kCVPixelFormatType_32BGRA;
        configuration.showsCursor = YES;
        // Up to 60 fps offered; the pacing timer takes what the encoder wants.
        configuration.minimumFrameInterval = CMTimeMake(1, 60);
        // One held as the current frame, one newest waiting, the rest free
        // for ScreenCaptureKit to keep drawing into.
        configuration.queueDepth = 5;

        OpenChatScreenStreamOutput *output = [[OpenChatScreenStreamOutput alloc] init];
        SCStream *stream = [[SCStream alloc] initWithFilter:filter
                                              configuration:configuration
                                                   delegate:output];
        dispatch_queue_t queue =
            dispatch_queue_create("app.openchat.screen-capture", DISPATCH_QUEUE_SERIAL);
        if (![stream addStreamOutput:output type:SCStreamOutputTypeScreen sampleHandlerQueue:queue error:&error]) {
            failure.message = QStringLiteral("ScreenCaptureKit would not deliver frames: %1")
                                  .arg(describeError(error));
            return false;
        }

        __block NSError *startError = nil;
        dispatch_semaphore_t started = dispatch_semaphore_create(0);
        [stream startCaptureWithCompletionHandler:^(NSError *completionError) {
            startError = completionError;
            dispatch_semaphore_signal(started);
        }];
        if (dispatch_semaphore_wait(started, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) != 0) {
            failure.message = QStringLiteral("ScreenCaptureKit did not start in time.");
            // Kept, not dropped: a start that completes late would otherwise
            // leave a running stream nothing refers to. Stopping it here
            // covers both orders.
            m_stream = stream;
            m_output = output;
            stop();
            return false;
        }
        if (startError != nil) {
            failure.message = startError.code == userDeclinedError
                ? NativeScreenCapturePlatform::accessDeniedMessage()
                : QStringLiteral("ScreenCaptureKit could not start: %1").arg(describeError(startError));
            return false;
        }
        m_stream = stream;
        m_output = output;
        return true;
    }

    Pull deliver(const FrameSink &sink)
    {
        BlackBox::Activity activity(area, "reading a ScreenCaptureKit frame");
        if (CVPixelBufferLockBaseAddress(m_current, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess)
            return Pull::Unchanged;
        ScreenFrameView view;
        view.bits = static_cast<const uchar *>(CVPixelBufferGetBaseAddress(m_current));
        view.bytesPerLine = qsizetype(CVPixelBufferGetBytesPerRow(m_current));
        view.width = int(CVPixelBufferGetWidth(m_current));
        view.height = int(CVPixelBufferGetHeight(m_current));
        // BGRA in memory is 0xAARRGGBB on every Mac (all little-endian); a
        // captured screen's alpha carries nothing, so it is read as opaque.
        view.format = QImage::Format_RGB32;
        Pull result = Pull::Unchanged;
        if (view.isValid()) {
            sink(view);
            result = Pull::Delivered;
        }
        CVPixelBufferUnlockBaseAddress(m_current, kCVPixelBufferLock_ReadOnly);
        return result;
    }

    void stop()
    {
        if (@available(macOS 12.3, *)) {
            SCStream *stream = (SCStream *)m_stream;
            OpenChatScreenStreamOutput *output = (OpenChatScreenStreamOutput *)m_output;
            m_stream = nil;
            m_output = nil;
            if (stream == nil)
                return;
            @try {
                // The stream and its output are kept alive by the block until
                // the stream has really stopped: frames can still be in flight
                // until then.
                [stream stopCaptureWithCompletionHandler:^(NSError *ignored) {
                    (void)ignored;
                    (void)stream;
                    (void)output;
                }];
            } @catch (NSException *) {
                // Stopping is best effort; everything is released regardless.
            }
        }
    }

    bool m_isWindow = false;
    quint64 m_id = 0;
    QString m_subject;
    int m_width = 0;
    int m_height = 0;
    // Typed as id so this class compiles for systems without the framework;
    // cast back inside @available blocks.
    id m_stream = nil;
    id m_output = nil;
    // The frame most recently handed to the encoder, kept for redelivery
    // while nothing changes.
    CVPixelBufferRef m_current = NULL;
};

} // namespace

namespace NativeScreenCapturePlatform {

bool isAvailable()
{
    if (qEnvironmentVariable("OPENCHAT_SCREEN_CAPTURE").trimmed().toLower() == QStringLiteral("qt"))
        return false;
    if (@available(macOS 12.3, *))
        return true;
    return false;
}

QString name()
{
    return isAvailable() ? QStringLiteral("ScreenCaptureKit") : QString();
}

QVector<ScreenShareSource> sources()
{
    QVector<ScreenShareSource> result;
    if (@available(macOS 12.3, *)) {
        @try {
            NSError *error = nil;
            SCShareableContent *content = fetchShareableContent(&error);
            if (content == nil) {
                BlackBox::record(area, QStringLiteral("could not list screens: ") + describeError(error));
                return result;
            }
            const int displayCount = int(content.displays.count);
            int ordinal = 0;
            for (SCDisplay *display in content.displays) {
                ++ordinal;
                ScreenShareSource source;
                source.kind = ScreenShareSource::Kind::Screen;
                source.native = true;
                source.nativeId = display.displayID;
                source.nativeName = QStringLiteral("display %1").arg(display.displayID);
                source.size = pixelSizeOf(display.displayID);
                source.screen = matchQtScreen(display.displayID);
                source.id = QStringLiteral("screen:%1").arg(display.displayID);
                const QString label = source.screen.isNull() || source.screen->name().isEmpty()
                    ? QStringLiteral("Display %1").arg(ordinal)
                    : source.screen->name();
                source.name = displayCount > 1
                    ? QStringLiteral("%1 (%2×%3)").arg(label).arg(source.size.width()).arg(source.size.height())
                    : QStringLiteral("Entire screen (%1×%2)").arg(source.size.width()).arg(source.size.height());
                result.append(source);
            }
            const pid_t self = getpid();
            for (SCWindow *window in content.windows) {
                // Ordinary application windows only: not menus, the Dock or
                // the menu bar, not OpenChat's own, and nothing too small to
                // be a window a person would choose.
                if (window.windowLayer != 0 || window.title.length == 0
                    || window.owningApplication == nil
                    || window.owningApplication.processID == self
                    || window.frame.size.width < 32 || window.frame.size.height < 32) {
                    continue;
                }
                ScreenShareSource source;
                source.kind = ScreenShareSource::Kind::Window;
                source.native = true;
                source.nativeId = window.windowID;
                source.name = fromNSString(window.title);
                source.size = QSize(int(window.frame.size.width), int(window.frame.size.height));
                source.id = QStringLiteral("window:%1").arg(window.windowID);
                result.append(source);
            }
        } @catch (NSException *exception) {
            BlackBox::record(area, QStringLiteral("listing screens threw: ") + fromNSString(exception.reason));
        }
    }
    return result;
}

std::unique_ptr<NativeScreenCapture> create(const ScreenShareSource &source)
{
    if (!source.native)
        return nullptr;
    return std::make_unique<ScreenCaptureKitCapture>(source);
}

ScreenCaptureAccess access(bool request)
{
    if (CGPreflightScreenCaptureAccess())
        return ScreenCaptureAccess::Granted;
    // Shows macOS's own prompt the first time it is asked; after that it
    // returns at once and only System Settings can change the answer.
    if (request)
        (void)CGRequestScreenCaptureAccess();
    return ScreenCaptureAccess::Denied;
}

QString accessDeniedMessage()
{
    return QStringLiteral(
        "macOS is not letting OpenChat record the screen. In System Settings, open Privacy & "
        "Security, then Screen & System Audio Recording, turn OpenChat on, and quit and reopen "
        "OpenChat. (Started from Terminal? Then it is Terminal that needs the permission.)");
}

bool openAccessSettings()
{
    return QDesktopServices::openUrl(QUrl(QStringLiteral(
        "x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture")));
}

} // namespace NativeScreenCapturePlatform

} // namespace OpenChat
