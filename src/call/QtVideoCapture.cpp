#include "call/QtVideoCapture.h"
#include "call/CallVideoSession.h"
#include "call/VideoFrameCopy.h"
#include <QCamera>
#include <QCoreApplication>
#include <QMediaDevices>
#include <QPermissions>
#include <QVideoFrame>
#include <QTransform>

namespace OpenChat {

QtVideoCapture::QtVideoCapture(QObject *parent) : QObject(parent)
{
    m_session.setVideoSink(&m_sink);
    // Pull the latest camera frame: no queue of full-resolution frames builds
    // up if capture is faster than encoding or the UI thread is occupied.
    m_timer.setInterval(67);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        if (!m_requested)
            return;
        const QVideoFrame frame = m_sink.videoFrame();
        if (!frame.isValid() || frame == m_lastFrame)
            return;
        // toImage() may import native camera textures using a Metal cache that
        // belongs to another RHI context. Detach mapped pixels first so Qt can
        // convert an owned buffer without touching the camera's texture cache.
        // JPEG conversion already reads CPU bytes and has no texture path.
        const QVideoFrame owned = frame.pixelFormat() == QVideoFrameFormat::Format_Jpeg
            ? frame : copyVideoFrameToMemory(frame);
        QImage image = owned.toImage();
        if (image.isNull())
            return;
        m_lastFrame = frame;
        m_startTimeout.start(3000);
        // toImage includes the surface transform, but not presentation rotation.
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
        const int rotation = static_cast<int>(frame.rotation());
#else
        const int rotation = static_cast<int>(frame.rotationAngle());
#endif
        if (rotation)
            image = image.transformed(QTransform().rotate(rotation));
        // Mirroring is a local-preview choice; send an unmirrored image to the peer.
        if (image.width() > CallVideoSession::maxDimension
            || image.height() > CallVideoSession::maxDimension) {
            image = image.scaled(CallVideoSession::maxDimension, CallVideoSession::maxDimension,
                                 Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        emit frameCaptured(image);
    });
    m_startTimeout.setSingleShot(true);
    connect(&m_startTimeout, &QTimer::timeout, this, [this] {
        stop();
        emit failed(QStringLiteral("The camera is not delivering video. Check whether another app is using it."));
    });
}

QtVideoCapture::~QtVideoCapture() { stop(); }

void QtVideoCapture::start()
{
    if (m_requested)
        return;
    m_requested = true;
    const quint64 generation = ++m_generation;
    const QCameraPermission permission;
    if (qApp->checkPermission(permission) == Qt::PermissionStatus::Undetermined) {
        qApp->requestPermission(permission, this, [this, generation](const QPermission &result) {
            if (!m_requested || generation != m_generation)
                return;
            if (result.status() == Qt::PermissionStatus::Granted)
                openCamera();
            else {
                stop();
                emit failed(QStringLiteral("Camera access is denied. Allow OpenChat in system privacy settings."));
            }
        });
    } else if (qApp->checkPermission(permission) == Qt::PermissionStatus::Granted) {
        openCamera();
    } else {
        stop();
        emit failed(QStringLiteral("Camera access is denied. Allow OpenChat in system privacy settings."));
    }
}

void QtVideoCapture::openCamera()
{
    const QCameraDevice device = QMediaDevices::defaultVideoInput();
    if (device.isNull()) {
        stop();
        emit failed(QStringLiteral("No camera found. Connect a camera and try again."));
        return;
    }
    m_camera = std::make_unique<QCamera>(device);
    const quint64 generation = m_generation;
    connect(m_camera.get(), &QCamera::errorOccurred, this,
        [this, generation](QCamera::Error error, const QString &message) {
            if (error == QCamera::NoError || !m_requested || generation != m_generation)
                return;
            stop();
            emit failed(message.isEmpty() ? QStringLiteral("The camera is unavailable.") : message);
        }, Qt::QueuedConnection);
    m_session.setCamera(m_camera.get());
    m_startTimeout.start(8000);
    m_timer.start();
    m_camera->start();
}

void QtVideoCapture::stop()
{
    m_requested = false;
    ++m_generation;
    m_timer.stop();
    m_startTimeout.stop();
    m_lastFrame = QVideoFrame();
    if (m_camera) {
        m_camera->stop();
        m_session.setCamera(nullptr);
        m_camera.reset();
    }
    m_sink.setVideoFrame(QVideoFrame());
}

} // namespace OpenChat
