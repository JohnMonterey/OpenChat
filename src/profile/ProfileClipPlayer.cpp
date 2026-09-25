#include "profile/ProfileClipPlayer.h"

#include "profile/ClipCodec.h"
#include "profile/ProfilePanelMedia.h"
#include "profile/SongPlayer.h"

#include <QPainter>
#include <QPainterPath>
#include <QPointer>

#include <algorithm>
#include <utility>

namespace OpenChat {

namespace {

constexpr int tickMs = 15;

} // namespace

ProfileClipPlayer::ProfileClipPlayer(QQuickItem *parent)
    : QQuickPaintedItem(parent)
{
    m_timer.setInterval(tickMs);
    connect(&m_timer, &QTimer::timeout, this, &ProfileClipPlayer::tick);
}

ProfileClipPlayer::~ProfileClipPlayer()
{
    m_timer.stop();
    stopSound();
}

void ProfileClipPlayer::componentComplete()
{
    QQuickPaintedItem::componentComplete();
    // `playing: true` may have been set before the keys were.
    if (std::exchange(m_wantPlaying, false))
        setPlaying(true);
}

void ProfileClipPlayer::setSegmentKeys(const QStringList &keys)
{
    if (keys == m_keys)
        return;
    if (m_playing) {
        m_playing = false;
        m_timer.stop();
        stopSound();
        emit playingChanged();
    }
    m_keys = keys;
    m_segments.clear();
    m_segmentStartMs.clear();
    m_durationMs = 0;
    m_hasSound = false;
    m_nextStartMs = 0;
    const bool hadPicture = hasPicture();
    m_picture = {};
    emit segmentKeysChanged();
    if (hadPicture)
        emit hasPictureChanged();
    update();
}

void ProfileClipPlayer::setLoop(bool loop)
{
    if (loop == m_loop)
        return;
    m_loop = loop;
    emit loopChanged();
}

void ProfileClipPlayer::setPlaying(bool playing)
{
    if (!isComponentComplete()) {
        m_wantPlaying = playing;
        return;
    }
    if (playing == m_playing)
        return;
    if (playing) {
        if (!load())
            return;
        m_playing = true;
        start();
    } else {
        m_playing = false;
        m_timer.stop();
        stopSound();
        m_nextStartMs = 0;
    }
    emit playingChanged();
}

void ProfileClipPlayer::setPaused(bool paused)
{
    if (paused == m_paused)
        return;
    m_paused = paused;
    if (m_playing && !m_segments.isEmpty()) {
        if (paused) {
            // The picture stays and the decoder keeps its place; only the
            // clock and the sound stop, at the moment the listener heard.
            const qint64 now = std::clamp<qint64>(clockMs(), 0, m_durationMs);
            m_timer.stop();
            stopSound();
            m_runStartMs = now;
            setPosition(now);
        } else {
            run(m_runStartMs, false);
        }
    }
    emit pausedChanged();
}

void ProfileClipPlayer::setRadius(qreal radius)
{
    if (qFuzzyCompare(radius, m_radius))
        return;
    m_radius = radius;
    emit radiusChanged();
    update();
}

void ProfileClipPlayer::setLongForm(bool longForm)
{
    if (longForm == m_longForm)
        return;
    const bool running = m_playing && !m_paused && !m_segments.isEmpty();
    const qint64 now = running ? std::clamp<qint64>(clockMs(), 0, m_durationMs) : 0;
    m_longForm = longForm;
    if (!m_segments.isEmpty()) {
        m_hasSound = clipSoundtrack(m_segments, soundLimits()).has_value();
        emit segmentKeysChanged();
    }
    // A sound held to the other bounds: picked up again where it was.
    if (running)
        run(now, false);
    emit longFormChanged();
}

void ProfileClipPlayer::seek(qint64 positionMs)
{
    if (!load())
        return;
    const qint64 target = std::clamp<qint64>(positionMs, 0, m_durationMs);
    const qint64 at = m_segmentStartMs.at(segmentAt(target));
    if (m_playing) {
        run(at, true);
        return;
    }
    // Stopped: the next play starts here, and its first picture shows now.
    m_nextStartMs = at;
    m_decoder = std::make_unique<ClipVideoDecoder>();
    m_segment = segmentAt(at);
    m_frame = 0;
    setPosition(at);
    decodeTo(at);
}

bool ProfileClipPlayer::load()
{
    if (!m_segments.isEmpty())
        return true;
    if (!clipCodecAvailable() || m_keys.isEmpty())
        return false;
    QVector<ClipContainer> segments;
    QVector<qint64> starts;
    qint64 total = 0;
    for (const QString &key : m_keys) {
        const auto clip = decodeClipContainer(PanelMediaLibrary::instance().get(key));
        // Every piece the same shape and pace, or the clip is not played at all.
        if (!clip || (!segments.isEmpty()
                      && (clip->width != segments.first().width || clip->height != segments.first().height
                          || clip->fps != segments.first().fps)))
            return false;
        starts.push_back(total);
        total += clip->durationMs;
        segments.push_back(*clip);
    }
    m_segments = std::move(segments);
    m_segmentStartMs = std::move(starts);
    m_durationMs = total;
    m_hasSound = clipSoundtrack(m_segments, soundLimits()).has_value();
    emit segmentKeysChanged();
    return true;
}

void ProfileClipPlayer::start()
{
    run(std::exchange(m_nextStartMs, 0), true);
}

void ProfileClipPlayer::run(qint64 atMs, bool resetPictures)
{
    stopSound();
    m_timer.stop();
    if (resetPictures || !m_decoder) {
        m_decoder = std::make_unique<ClipVideoDecoder>();
        m_segment = segmentAt(atMs);
        m_frame = 0;
    }
    m_runStartMs = atMs;
    setPosition(atMs);
    if (m_paused) {
        decodeTo(atMs);
        return;
    }
    const SongContainerLimits limits = soundLimits();
    if (const auto song = clipSoundtrack(m_segments, limits)) {
        QString error;
        std::unique_ptr<SongOutput> output = SongPlayer::openOutput(song->channels, error);
        if (output) {
            auto stream = std::make_unique<SongStream>(*song, output->format(),
                                                       atMs * ClipContainer::audioSampleRate / 1000, limits);
            if (stream->isValid() && output->start(stream.get())) {
                m_stream = std::move(stream);
                m_output = std::move(output);
                QPointer<ProfileClipPlayer> self(this);
                m_soundToken = SongPlayer::takeOverSound([self] {
                    if (self)
                        self->setPlaying(false);
                });
            } else {
                output->stop();
            }
        }
        // No output: the pictures still play, silently.
    }
    m_clock.start();
    m_timer.start();
    tick();
}

void ProfileClipPlayer::stopSound()
{
    if (m_output)
        m_output->stop();
    m_output.reset();
    m_stream.reset();
    if (m_soundToken != 0) {
        SongPlayer::releaseSound(m_soundToken);
        m_soundToken = 0;
    }
}

qint64 ProfileClipPlayer::clockMs() const
{
    if (m_stream && m_output) {
        if (m_stream->finished())
            return m_durationMs;
        const qint64 heard = m_stream->framesPlayed() * 1000 / ClipContainer::audioSampleRate - m_output->bufferMs();
        return std::max<qint64>(m_runStartMs, heard);
    }
    return m_runStartMs + m_clock.elapsed();
}

SongContainerLimits ProfileClipPlayer::soundLimits() const
{
    return m_longForm ? chatSongLimits() : SongContainerLimits{};
}

qsizetype ProfileClipPlayer::segmentAt(qint64 atMs) const
{
    qsizetype segment = 0;
    while (segment + 1 < m_segments.size() && m_segmentStartMs.at(segment + 1) <= atMs)
        ++segment;
    return segment;
}

void ProfileClipPlayer::tick()
{
    if (!m_playing || m_paused || m_segments.isEmpty() || !m_decoder)
        return;
    const qint64 now = clockMs();
    if (now >= m_durationMs) {
        if (m_loop) {
            start();
            return;
        }
        setPlaying(false);
        emit finished();
        return;
    }
    decodeTo(now);
    if (now / 250 != m_positionMs / 250) {
        m_positionMs = now;
        emit positionChanged();
    }
}

void ProfileClipPlayer::decodeTo(qint64 atMs)
{
    if (m_segments.isEmpty() || !m_decoder)
        return;
    // The frame that should be on screen at `atMs`.
    const qsizetype segment = segmentAt(atMs);
    const ClipContainer &clip = m_segments.at(segment);
    const qsizetype frame = std::min<qsizetype>(
        (atMs - m_segmentStartMs.at(segment)) * clip.fps / 1000, clip.frames.size() - 1);

    // Decode forward to it; only the last one is turned into a picture.
    while (m_segment < segment || (m_segment == segment && m_frame <= frame)) {
        const ClipContainer &current = m_segments.at(m_segment);
        if (m_frame >= current.frames.size()) {
            ++m_segment;
            m_frame = 0;
            continue;
        }
        const bool last = m_segment == segment && m_frame == frame;
        QImage picture = m_decoder->decode(current.frames.at(m_frame), last);
        if (last && !picture.isNull())
            setPicture(std::move(picture));
        ++m_frame;
    }
}

void ProfileClipPlayer::setPicture(QImage picture)
{
    const bool hadPicture = hasPicture();
    m_picture = std::move(picture);
    update();
    if (hadPicture != hasPicture())
        emit hasPictureChanged();
}

void ProfileClipPlayer::setPosition(qint64 positionMs)
{
    if (positionMs == m_positionMs)
        return;
    m_positionMs = positionMs;
    emit positionChanged();
}

void ProfileClipPlayer::paint(QPainter *painter)
{
    if (m_picture.isNull())
        return;
    const QSizeF shown = QSizeF(m_picture.size()).scaled(QSizeF(width(), height()), Qt::KeepAspectRatio);
    const QRectF target(QPointF((width() - shown.width()) / 2, (height() - shown.height()) / 2), shown);
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
    if (m_radius > 0) {
        // The corners of the picture itself, wherever the fit puts it.
        painter->setRenderHint(QPainter::Antialiasing, true);
        QPainterPath path;
        path.addRoundedRect(target, m_radius, m_radius);
        painter->setClipPath(path);
    }
    painter->drawImage(target, m_picture);
}

} // namespace OpenChat
