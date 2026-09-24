#pragma once

#include "domain/ClipContainer.h"

#include <QElapsedTimer>
#include <QImage>
#include <QQuickPaintedItem>
#include <QStringList>
#include <QTimer>

#include <memory>

namespace OpenChat {

class ClipVideoDecoder;
class SongOutput;
class SongStream;

// Plays a panel video (docs/profile-panels.md): its segments, by the hex
// keys PanelMediaLibrary holds them under, back to back, the pictures
// fitted into the item and the sound through the same guarded stream as a
// profile song (resampled, loudness-capped, never louder than the UI).
// Pictures follow the sound's clock; a silent clip runs on a timer. Only one
// page sound plays at a time: starting takes over from a song, and a song
// starting stops this. Every segment is checked again (decodeClipContainer)
// before anything reaches a decoder.
class ProfileClipPlayer : public QQuickPaintedItem
{
    Q_OBJECT
    Q_PROPERTY(QStringList segmentKeys READ segmentKeys WRITE setSegmentKeys NOTIFY segmentKeysChanged)
    Q_PROPERTY(bool loop READ loop WRITE setLoop NOTIFY loopChanged)
    Q_PROPERTY(bool playing READ playing WRITE setPlaying NOTIFY playingChanged)
    Q_PROPERTY(qint64 positionMs READ positionMs NOTIFY positionChanged)
    Q_PROPERTY(qint64 durationMs READ durationMs NOTIFY segmentKeysChanged)
    Q_PROPERTY(bool hasSound READ hasSound NOTIFY segmentKeysChanged)

public:
    explicit ProfileClipPlayer(QQuickItem *parent = nullptr);
    ~ProfileClipPlayer() override;

    [[nodiscard]] QStringList segmentKeys() const { return m_keys; }
    void setSegmentKeys(const QStringList &keys);
    [[nodiscard]] bool loop() const noexcept { return m_loop; }
    void setLoop(bool loop);
    [[nodiscard]] bool playing() const noexcept { return m_playing; }
    void setPlaying(bool playing);
    [[nodiscard]] qint64 positionMs() const noexcept { return m_positionMs; }
    [[nodiscard]] qint64 durationMs() const noexcept { return m_durationMs; }
    [[nodiscard]] bool hasSound() const noexcept { return m_hasSound; }

    void paint(QPainter *painter) override;

signals:
    void segmentKeysChanged();
    void loopChanged();
    void playingChanged();
    void positionChanged();
    void finished(); // reached the end without looping

private:
    bool load();
    void start();
    void stopSound();
    void tick();
    [[nodiscard]] qint64 clockMs() const;

    QStringList m_keys;
    bool m_loop = false;
    bool m_playing = false;
    QVector<ClipContainer> m_segments;
    QVector<qint64> m_segmentStartMs;
    qint64 m_durationMs = 0;
    qint64 m_positionMs = 0;
    bool m_hasSound = false;

    std::unique_ptr<ClipVideoDecoder> m_decoder;
    std::unique_ptr<SongStream> m_stream;
    std::unique_ptr<SongOutput> m_output;
    quint64 m_soundToken = 0;
    QElapsedTimer m_clock; // the silent clip's (and a sound's fallback) clock
    QTimer m_timer;
    qsizetype m_segment = 0; // next frame to decode: segment and index
    qsizetype m_frame = 0;
    QImage m_picture;
};

} // namespace OpenChat
