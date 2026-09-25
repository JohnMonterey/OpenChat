#pragma once

#include "domain/ClipContainer.h"
#include "domain/SongContainer.h"

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
//
// A chat video uses the same player with a few more controls, all off by
// default so a panel plays exactly as before: `paused` holds the picture
// and the place while `playing` stays true (false again carries on from
// there), seek() jumps to the start of the segment holding a time (every
// segment starts on a keyframe), `radius` rounds the picture's corners,
// `hasPicture` says when there is a frame to show instead of the poster, and
// `longForm` lets the sound run past a profile song's 45 seconds.
class ProfileClipPlayer : public QQuickPaintedItem
{
    Q_OBJECT
    Q_PROPERTY(QStringList segmentKeys READ segmentKeys WRITE setSegmentKeys NOTIFY segmentKeysChanged)
    Q_PROPERTY(bool loop READ loop WRITE setLoop NOTIFY loopChanged)
    Q_PROPERTY(bool playing READ playing WRITE setPlaying NOTIFY playingChanged)
    Q_PROPERTY(bool paused READ paused WRITE setPaused NOTIFY pausedChanged)
    Q_PROPERTY(qint64 positionMs READ positionMs NOTIFY positionChanged)
    Q_PROPERTY(qint64 durationMs READ durationMs NOTIFY segmentKeysChanged)
    Q_PROPERTY(bool hasSound READ hasSound NOTIFY segmentKeysChanged)
    Q_PROPERTY(bool hasPicture READ hasPicture NOTIFY hasPictureChanged)
    Q_PROPERTY(qreal radius READ radius WRITE setRadius NOTIFY radiusChanged)
    Q_PROPERTY(bool longForm READ longForm WRITE setLongForm NOTIFY longFormChanged)

public:
    explicit ProfileClipPlayer(QQuickItem *parent = nullptr);
    ~ProfileClipPlayer() override;

    [[nodiscard]] QStringList segmentKeys() const { return m_keys; }
    void setSegmentKeys(const QStringList &keys);
    [[nodiscard]] bool loop() const noexcept { return m_loop; }
    void setLoop(bool loop);
    [[nodiscard]] bool playing() const noexcept { return m_playing; }
    void setPlaying(bool playing);
    [[nodiscard]] bool paused() const noexcept { return m_paused; }
    void setPaused(bool paused);
    [[nodiscard]] qint64 positionMs() const noexcept { return m_positionMs; }
    [[nodiscard]] qint64 durationMs() const noexcept { return m_durationMs; }
    [[nodiscard]] bool hasSound() const noexcept { return m_hasSound; }
    [[nodiscard]] bool hasPicture() const noexcept { return !m_picture.isNull(); }
    [[nodiscard]] qreal radius() const noexcept { return m_radius; }
    void setRadius(qreal radius);
    [[nodiscard]] bool longForm() const noexcept { return m_longForm; }
    void setLongForm(bool longForm);

    // Moves to the start of the segment holding `positionMs`, playing or
    // paused as before. Stopped, it is where the next play starts.
    Q_INVOKABLE void seek(qint64 positionMs);

    void paint(QPainter *painter) override;

protected:
    void componentComplete() override;

signals:
    void segmentKeysChanged();
    void loopChanged();
    void playingChanged();
    void pausedChanged();
    void positionChanged();
    void hasPictureChanged();
    void radiusChanged();
    void longFormChanged();
    void finished(); // reached the end without looping

private:
    bool load();
    void start();
    // Runs the clip from `atMs`: the sound from there, the pictures from the
    // start of its segment when `resetPictures` (a keyframe), else from where
    // the decoder already is. Paused, only the picture at `atMs` is shown.
    void run(qint64 atMs, bool resetPictures);
    void stopSound();
    void tick();
    // Decodes forward to the frame on screen at `atMs`, turning only that
    // one into a picture.
    void decodeTo(qint64 atMs);
    void setPicture(QImage picture);
    void setPosition(qint64 positionMs);
    [[nodiscard]] qsizetype segmentAt(qint64 atMs) const;
    [[nodiscard]] qint64 clockMs() const;
    [[nodiscard]] SongContainerLimits soundLimits() const;

    QStringList m_keys;
    bool m_loop = false;
    bool m_playing = false;
    bool m_paused = false;
    bool m_wantPlaying = false; // asked for before the item was complete
    bool m_longForm = false;
    qreal m_radius = 0;
    QVector<ClipContainer> m_segments;
    QVector<qint64> m_segmentStartMs;
    qint64 m_durationMs = 0;
    qint64 m_positionMs = 0;
    qint64 m_runStartMs = 0;  // the clip time the running clock started at
    qint64 m_nextStartMs = 0; // where the next play starts (a seek while stopped)
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
