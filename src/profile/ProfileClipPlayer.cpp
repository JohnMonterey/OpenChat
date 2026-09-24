#include "profile/ProfileClipPlayer.h"

#include "profile/ClipCodec.h"
#include "profile/ProfilePanelMedia.h"
#include "profile/SongPlayer.h"

#include <QPainter>
#include <QPointer>

#include <algorithm>

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

void ProfileClipPlayer::setSegmentKeys(const QStringList &keys)
{
    if (keys == m_keys)
        return;
    setPlaying(false);
    m_keys = keys;
    m_segments.clear();
    m_segmentStartMs.clear();
    m_durationMs = 0;
    m_hasSound = false;
    m_picture = {};
    emit segmentKeysChanged();
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
    }
    emit playingChanged();
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
    m_hasSound = clipSoundtrack(m_segments).has_value();
    emit segmentKeysChanged();
    return true;
}

void ProfileClipPlayer::start()
{
    stopSound();
    m_decoder = std::make_unique<ClipVideoDecoder>();
    m_segment = 0;
    m_frame = 0;
    m_positionMs = 0;
    if (const auto song = clipSoundtrack(m_segments)) {
        QString error;
        std::unique_ptr<SongOutput> output = SongPlayer::openOutput(song->channels, error);
        if (output) {
            auto stream = std::make_unique<SongStream>(*song, output->format());
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
        return std::max<qint64>(0, heard);
    }
    return m_clock.elapsed();
}

void ProfileClipPlayer::tick()
{
    if (!m_playing || m_segments.isEmpty() || !m_decoder)
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
    // The frame that should be on screen now.
    qsizetype segment = 0;
    while (segment + 1 < m_segments.size() && m_segmentStartMs.at(segment + 1) <= now)
        ++segment;
    const ClipContainer &clip = m_segments.at(segment);
    const qsizetype frame = std::min<qsizetype>(
        (now - m_segmentStartMs.at(segment)) * clip.fps / 1000, clip.frames.size() - 1);

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
        if (last && !picture.isNull()) {
            m_picture = std::move(picture);
            update();
        }
        ++m_frame;
    }
    if (now / 250 != m_positionMs / 250) {
        m_positionMs = now;
        emit positionChanged();
    }
}

void ProfileClipPlayer::paint(QPainter *painter)
{
    if (m_picture.isNull())
        return;
    const QSizeF shown = QSizeF(m_picture.size()).scaled(QSizeF(width(), height()), Qt::KeepAspectRatio);
    const QRectF target(QPointF((width() - shown.width()) / 2, (height() - shown.height()) / 2), shown);
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter->drawImage(target, m_picture);
}

} // namespace OpenChat
