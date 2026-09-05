#pragma once

#include <QObject>
#include <QImage>
#include <QMediaCaptureSession>
#include <QTimer>
#include <QVideoSink>
#include <QVideoFrame>
#include <memory>

QT_BEGIN_NAMESPACE
class QCamera;
QT_END_NAMESPACE

namespace OpenChat {

class QtVideoCapture final : public QObject
{
    Q_OBJECT
public:
    explicit QtVideoCapture(QObject *parent = nullptr);
    ~QtVideoCapture() override;
    void start();
    void stop();
signals:
    void frameCaptured(const QImage &image);
    void failed(const QString &message);
private:
    void openCamera();
    bool m_requested = false;
    quint64 m_generation = 0;
    std::unique_ptr<QCamera> m_camera;
    QMediaCaptureSession m_session;
    QVideoSink m_sink;
    QVideoFrame m_lastFrame;
    QTimer m_timer;
    QTimer m_startTimeout;
};

} // namespace OpenChat
