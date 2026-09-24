#pragma once

#include "profile/ClipCodec.h"

#include <QAudioDecoder>
#include <QByteArray>
#include <QElapsedTimer>
#include <QImage>
#include <QMediaPlayer>
#include <QObject>
#include <QPointer>
#include <QSize>
#include <QTimer>
#include <QVector>
#include <QVideoSink>

#include <atomic>
#include <memory>

namespace OpenChat {

// A panel video ready to store: its segments (each a ClipContainer within one
// PageMedia message), their lengths, and a poster frame as a JPEG.
struct ImportedClip final {
    QVector<QByteArray> segments;
    QVector<quint32> durationsMs;
    QSize size;
    QByteArray posterJpeg;
    QSize posterSize;
};

// Turns the owner's video file into an ImportedClip (docs/profile-panels.md):
// the first 30 seconds, at most 480 px on the long side and 15 pictures a
// second, with its sound. Qt Multimedia decodes the file (a QMediaPlayer
// into a video sink, sped up and silent, for the pictures; a QAudioDecoder
// for the sound), so what opens is what the computer's media backend reads:
// Linux's FFmpeg, Windows' Media Foundation (MP4, MOV, WMV, …). Encoding
// runs on the thread pool, a segment as soon as its pictures are in.
//
// One import at a time; start() cancels the one before, whose results are
// never emitted.
class ClipImporter final : public QObject
{
    Q_OBJECT

public:
    struct Limits final {
        qint64 maxFileBytes = 1024LL * 1024 * 1024;
        int stallTimeoutMs = 20'000; // no progress for this long fails the import
        qreal playbackRate = 3.0;
    };

    explicit ClipImporter(QObject *parent = nullptr);
    ClipImporter(ClipEncodeOptions options, Limits limits, QObject *parent = nullptr);
    ~ClipImporter() override;

    void start(const QString &path);
    void cancel();
    [[nodiscard]] bool busy() const noexcept { return m_run != nullptr; }

signals:
    void progressChanged(qreal progress);
    void finished(const OpenChat::ImportedClip &clip);
    void failed(const QString &message);

private:
    class Run;
    friend class Run;

    ClipEncodeOptions m_options;
    Limits m_limits;
    Run *m_run = nullptr;
};

} // namespace OpenChat

Q_DECLARE_METATYPE(OpenChat::ImportedClip)
