#include "profile/ClipImport.h"

#include "domain/ProfilePageCodec.h"

#include <QAudioBuffer>
#include <QBuffer>
#include <QFileInfo>
#include <QImageWriter>
#include <QUrl>
#include <QVideoFrame>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace OpenChat {

namespace {

constexpr qint64 minimumClipMs = 500;
constexpr qint64 packetMs = 20;
constexpr int posterQuality = 82;

[[nodiscard]] std::optional<QVector<qint16>> toS16(const QAudioBuffer &buffer)
{
    const QAudioFormat format = buffer.format();
    const qsizetype count = buffer.sampleCount();
    QVector<qint16> out(count);
    switch (format.sampleFormat()) {
    case QAudioFormat::UInt8: {
        const auto *in = buffer.constData<quint8>();
        for (qsizetype i = 0; i < count; ++i)
            out[i] = qint16((int(in[i]) - 128) * 256);
        break;
    }
    case QAudioFormat::Int16:
        std::memcpy(out.data(), buffer.constData<qint16>(), std::size_t(count) * sizeof(qint16));
        break;
    case QAudioFormat::Int32: {
        const auto *in = buffer.constData<qint32>();
        for (qsizetype i = 0; i < count; ++i)
            out[i] = qint16(in[i] >> 16);
        break;
    }
    case QAudioFormat::Float: {
        const auto *in = buffer.constData<float>();
        for (qsizetype i = 0; i < count; ++i) {
            const float value = std::isfinite(in[i]) ? std::clamp(in[i], -1.0F, 1.0F) : 0.0F;
            out[i] = qint16(std::lround(value * 32767.0F));
        }
        break;
    }
    default:
        return std::nullopt;
    }
    return out;
}

// A stereo decode of a mono source carries every sample twice; as mono it
// costs less and sounds the same.
void foldDualMono(WavAudio &audio)
{
    if (audio.channels != 2)
        return;
    const QVector<qint16> &s = audio.samples;
    for (qsizetype i = 0; i + 1 < s.size(); i += 2) {
        if (s.at(i) != s.at(i + 1))
            return;
    }
    QVector<qint16> mono;
    mono.reserve(s.size() / 2);
    for (qsizetype i = 0; i + 1 < s.size(); i += 2)
        mono.push_back(s.at(i));
    audio.samples = std::move(mono);
    audio.channels = 1;
}

[[nodiscard]] QByteArray jpegOf(const QImage &image)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    QImageWriter writer(&buffer, "jpeg");
    writer.setQuality(posterQuality);
    if (!writer.write(image.convertToFormat(QImage::Format_RGB32)))
        return {};
    return bytes;
}

} // namespace

// One import: the player and decoder reading the file, the pictures of the
// segment being filled, and the encodes under way. Children of the
// importer, only ever deleteLater()'d.
class ClipImporter::Run final : public QObject
{
public:
    Run(ClipImporter &owner, QString path)
        : QObject(&owner)
        , m_owner(owner)
        , m_path(std::move(path))
        , m_player(this)
        , m_sink(this)
        , m_audio(this)
        , m_stall(this)
        , m_cancelled(std::make_shared<std::atomic_bool>(false))
    {
    }

    ~Run() override { *m_cancelled = true; }

    void start()
    {
        const QFileInfo file(m_path);
        if (!file.isFile() || !file.isReadable()) {
            fail(QStringLiteral("That file can't be opened."));
            return;
        }
        if (file.size() > m_owner.m_limits.maxFileBytes) {
            fail(QStringLiteral("That video file is too large."));
            return;
        }
        m_stall.setSingleShot(true);
        m_stall.setInterval(m_owner.m_limits.stallTimeoutMs);
        connect(&m_stall, &QTimer::timeout, this, [this] {
            // Pictures in but the sound never finishing: the clip goes silent
            // rather than not at all.
            if (m_picturesDone && !m_audioDone) {
                m_audio.stop();
                m_pcm = {};
                finishAudio();
                return;
            }
            fail(QStringLiteral("Reading that video took too long."));
        });

        m_player.setVideoSink(&m_sink);
        connect(&m_sink, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame &frame) { onFrame(frame); });
        connect(&m_player, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
            if (status == QMediaPlayer::LoadedMedia && !m_loaded)
                onLoaded();
            else if (status == QMediaPlayer::EndOfMedia)
                finishPictures();
            else if (status == QMediaPlayer::InvalidMedia)
                fail(QStringLiteral("This file can't be opened as a video."));
        });
        connect(&m_player, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error error) {
            if (error != QMediaPlayer::NoError && !m_picturesDone)
                fail(QStringLiteral("This file can't be opened as a video."));
        });

        // The sound, decoded on its own: a file without any is a silent clip.
        QAudioFormat format;
        format.setSampleRate(ClipContainer::audioSampleRate);
        format.setChannelCount(2);
        format.setSampleFormat(QAudioFormat::Int16);
        m_audio.setAudioFormat(format);
        connect(&m_audio, &QAudioDecoder::bufferReady, this, [this] { drainAudio(); });
        connect(&m_audio, &QAudioDecoder::finished, this, [this] { finishAudio(); });
        connect(&m_audio, qOverload<QAudioDecoder::Error>(&QAudioDecoder::error), this, [this] {
            m_pcm = {};
            finishAudio();
        });

        m_stall.start();
        m_player.setSource(QUrl::fromLocalFile(m_path));
        m_audio.setSource(QUrl::fromLocalFile(m_path));
        m_audio.start();
    }

    void halt()
    {
        m_done = true;
        *m_cancelled = true;
        m_stall.stop();
        m_player.stop();
        m_audio.stop();
    }

private:
    [[nodiscard]] const ClipEncodeOptions &options() const { return m_owner.m_options; }

    void onLoaded()
    {
        m_loaded = true;
        if (!m_player.hasVideo()) {
            fail(QStringLiteral("This file has no video in it."));
            return;
        }
        const qint64 duration = m_player.duration();
        m_durationKnown = duration > 0;
        m_trimmed = duration > options().maxDurationMs;
        m_clipMs = std::min<qint64>(duration > 0 ? duration : options().maxDurationMs, options().maxDurationMs);
        m_clipMs -= m_clipMs % packetMs;
        if (m_clipMs < minimumClipMs) {
            fail(QStringLiteral("This video is too short."));
            return;
        }
        for (qint64 at = 0; at < m_clipMs; at += options().segmentMs)
            m_durations.push_back(std::min<qint64>(options().segmentMs, m_clipMs - at));
        for (const qint64 length : std::as_const(m_durations))
            m_frameCounts.push_back(int((length * options().fps + 999) / 1000));
        m_encoded.resize(m_durations.size());
        m_player.setPlaybackRate(m_owner.m_limits.playbackRate);
        m_player.play();
    }

    // The picture for frame `index` of the clip is the first decoded frame
    // at or after its time.
    [[nodiscard]] qint64 frameTimeMs(qint64 index) const { return index * 1000 / options().fps; }
    [[nodiscard]] qint64 totalFrames() const
    {
        qint64 total = 0;
        for (const int count : m_frameCounts)
            total += count;
        return total;
    }

    void onFrame(const QVideoFrame &frame)
    {
        if (m_done || m_picturesDone || !m_loaded || !frame.isValid())
            return;
        const qint64 startUs = frame.startTime();
        const qint64 atMs = startUs >= 0 ? startUs / 1000 : m_player.position();
        QImage picture;
        while (m_nextFrame < totalFrames() && frameTimeMs(m_nextFrame) <= atMs + 1000 / (2 * options().fps)) {
            if (picture.isNull()) {
                QImage full = frame.toImage();
                if (full.isNull())
                    return;
                if (m_size.isEmpty())
                    m_size = clipFrameSize(full.size(), options().longSide);
                if (m_size.isEmpty()) {
                    fail(QStringLiteral("This video's pictures can't be read."));
                    return;
                }
                picture = full.scaled(m_size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                              .convertToFormat(QImage::Format_RGB32);
            }
            addPicture(picture);
        }
        if (!m_heldBack)
            m_stall.start();
        emit m_owner.progressChanged(progress());
        // A source that never said how long it is shows it was cut by
        // running on past the clip.
        if (!m_durationKnown && atMs >= m_clipMs)
            m_trimmed = true;
        if (atMs >= m_clipMs || m_nextFrame >= totalFrames())
            finishPictures();
    }

    void addPicture(const QImage &picture)
    {
        // The poster: the picture a second in (or the first of a shorter clip).
        if (m_poster.isNull() || frameTimeMs(m_nextFrame) <= 1000)
            m_poster = picture;
        m_pending.push_back(picture);
        m_last = picture;
        ++m_nextFrame;
        if (m_pending.size() == m_frameCounts.at(m_fillSegment))
            dispatchSegment();
    }

    void finishPictures()
    {
        if (m_done || m_picturesDone || !m_loaded)
            return;
        m_player.stop();
        if (m_last.isNull()) {
            fail(QStringLiteral("This video's pictures can't be read."));
            return;
        }
        // A file that ends well before the length it announced (or announced
        // none): the clip ends where its pictures do. Segments already sent
        // to the encoder are whole and stay as they are.
        const qint64 coveredMs = frameTimeMs(m_nextFrame);
        if (coveredMs + 1000 < m_clipMs) {
            const qint64 clipMs = coveredMs - coveredMs % packetMs;
            if (clipMs < minimumClipMs) {
                fail(QStringLiteral("This video is too short."));
                return;
            }
            qint64 start = 0;
            for (int k = 0; k < m_fillSegment; ++k)
                start += m_durations.at(k);
            const int kept = clipMs > start ? m_fillSegment + 1 : m_fillSegment;
            m_durations.resize(kept);
            m_frameCounts.resize(kept);
            m_encoded.resize(kept);
            if (kept > m_fillSegment) {
                m_durations[m_fillSegment] = clipMs - start;
                m_frameCounts[m_fillSegment] = int(((clipMs - start) * options().fps + 999) / 1000);
                if (m_pending.size() > m_frameCounts.at(m_fillSegment))
                    m_pending.resize(m_frameCounts.at(m_fillSegment));
            } else {
                m_pending.clear();
            }
            m_clipMs = clipMs;
            m_nextFrame = std::min<qint64>(m_nextFrame, totalFrames());
            if (!m_pending.isEmpty() && m_pending.size() == m_frameCounts.at(m_fillSegment))
                dispatchSegment();
        }
        // A file a few pictures short holds its last picture to the end.
        while (m_nextFrame < totalFrames())
            addPicture(m_last);
        m_picturesDone = true;
        m_stall.start(); // now the sound's to finish
        finishIfReady();
    }

    void dispatchSegment()
    {
        const int index = m_fillSegment++;
        QVector<QImage> frames = std::exchange(m_pending, {});
        const qint64 length = m_durations.at(index);
        // What the sound will take of the segment, with room to spare.
        const qint64 soundBytes = qint64(options().stereoAudioBps) / 8 * length / 1000 * 6 / 5
                                  + (length / packetMs + 2) * 2;
        const qsizetype videoCap = options().maxSegmentBytes - soundBytes - frames.size() * 5 - 256;
        ++m_encodesRunning;
        const int queueLimit = m_owner.m_limits.maxQueuedSegments;
        if (queueLimit > 0 && m_encodesRunning > queueLimit && !m_heldBack
            && m_player.playbackState() == QMediaPlayer::PlayingState) {
            // The encoder is behind: hold the reading until it catches up.
            // Waiting for it is no stall, so the stall timer waits too.
            m_heldBack = true;
            m_stall.stop();
            m_player.pause();
        }
        QPointer<Run> self(this);
        const std::shared_ptr<std::atomic_bool> cancelled = m_cancelled;
        const int fps = options().fps;
        const QVector<int> kbps = options().videoKbps;
        m_owner.pool()->start([self, cancelled, index, frames, fps, kbps, videoCap] {
            auto encoded = encodeClipVideo(frames, fps, kbps, videoCap, [cancelled] { return cancelled->load(); });
            if (cancelled->load())
                return;
            QMetaObject::invokeMethod(
                self.data(),
                [self, index, encoded = std::move(encoded)] {
                    if (self)
                        self->segmentEncoded(index, encoded);
                },
                Qt::QueuedConnection);
        });
    }

    void segmentEncoded(int index, const std::optional<QVector<ClipContainer::Frame>> &encoded)
    {
        --m_encodesRunning;
        if (m_done)
            return;
        if (!encoded) {
            fail(QStringLiteral("This video has too much going on to fit. Try a shorter or calmer clip."));
            return;
        }
        m_encoded[index] = *encoded;
        ++m_encodesDone;
        if (m_heldBack && m_encodesRunning <= m_owner.m_limits.maxQueuedSegments) {
            m_heldBack = false;
            if (!m_picturesDone) {
                m_stall.start();
                m_player.play();
            }
        }
        emit m_owner.progressChanged(progress());
        finishIfReady();
    }

    void drainAudio()
    {
        while (!m_done && !m_audioDone && m_audio.bufferAvailable()) {
            const QAudioBuffer buffer = m_audio.read();
            if (!buffer.isValid())
                break;
            const QAudioFormat format = buffer.format();
            if (m_pcm.sampleRate == 0) {
                m_pcm.sampleRate = format.sampleRate();
                m_pcm.channels = format.channelCount();
                if (m_pcm.sampleRate <= 0 || m_pcm.sampleRate > 384'000 || m_pcm.channels <= 0 || m_pcm.channels > 8) {
                    m_pcm = {};
                    finishAudio();
                    return;
                }
            } else if (format.sampleRate() != m_pcm.sampleRate || format.channelCount() != m_pcm.channels) {
                m_pcm = {}; // a stream that changes shape midway: no sound rather than a wrong one
                finishAudio();
                return;
            }
            const std::optional<QVector<qint16>> samples = toS16(buffer);
            if (!samples) {
                m_pcm = {};
                finishAudio();
                return;
            }
            m_pcm.samples += *samples;
            if (m_picturesDone)
                m_stall.start();
            // Only the clip's length is kept; the rest of the file is not read.
            const qint64 wanted = options().maxDurationMs * m_pcm.sampleRate / 1000 * m_pcm.channels;
            if (m_pcm.samples.size() >= wanted) {
                m_pcm.samples.resize(wanted);
                m_audio.stop();
                finishAudio();
                return;
            }
        }
    }

    void finishAudio()
    {
        if (m_audioDone)
            return;
        m_audioDone = true;
        finishIfReady();
    }

    [[nodiscard]] qreal progress() const
    {
        const qint64 total = std::max<qint64>(1, totalFrames());
        const qreal pictures = qreal(m_nextFrame) / qreal(total);
        const qreal encodes = m_encoded.isEmpty() ? 0 : qreal(m_encodesDone) / qreal(m_encoded.size());
        return std::clamp(0.65 * pictures + 0.3 * encodes, 0.0, 0.95);
    }

    void finishIfReady()
    {
        if (m_done || !m_picturesDone || !m_audioDone || m_encodesDone < m_encoded.size() || m_packing)
            return;
        m_packing = true;
        m_stall.stop();
        WavAudio pcm = std::move(m_pcm);
        foldDualMono(pcm);
        const int bitrate = pcm.channels == 1 ? options().monoAudioBps : options().stereoAudioBps;
        QPointer<Run> self(this);
        const std::shared_ptr<std::atomic_bool> cancelled = m_cancelled;
        m_owner.pool()->start([self, cancelled, pcm = std::move(pcm), bitrate, clipMs = m_clipMs, video = m_encoded,
                               durations = m_durations, size = m_size, fps = options().fps,
                               maxBytes = options().maxSegmentBytes, poster = m_poster, trimmed = m_trimmed] {
            ImportedClip clip;
            clip.trimmed = trimmed;
            const std::optional<ClipAudio> audio = encodeClipAudio(pcm, clipMs, bitrate);
            if (audio)
                clip.segments = packClip(video, durations, size, fps, *audio, maxBytes);
            if (clip.segments.isEmpty() && audio && audio->channels > 0) {
                // The sound pushed a segment over: the clip goes without it
                // rather than not at all.
                clip.segments = packClip(video, durations, size, fps, ClipAudio{}, maxBytes);
            }
            for (const qint64 length : durations)
                clip.durationsMs.push_back(quint32(length));
            clip.size = size;
            clip.posterJpeg = jpegOf(poster);
            clip.posterSize = poster.size();
            if (cancelled->load())
                return;
            QMetaObject::invokeMethod(
                self.data(),
                [self, clip] {
                    if (self)
                        self->packed(clip);
                },
                Qt::QueuedConnection);
        });
    }

    void packed(const ImportedClip &clip)
    {
        if (m_done)
            return;
        if (clip.segments.isEmpty() || clip.posterJpeg.isEmpty() || clip.posterJpeg.size() > maxPanelImageBytes) {
            fail(QStringLiteral("This video couldn't be prepared. Try another file."));
            return;
        }
        m_done = true;
        emit m_owner.progressChanged(1.0);
        ClipImporter &owner = m_owner;
        owner.m_run = nullptr;
        deleteLater();
        emit owner.finished(clip);
    }

    void fail(const QString &message)
    {
        if (m_done)
            return;
        halt();
        ClipImporter &owner = m_owner;
        owner.m_run = nullptr;
        deleteLater();
        emit owner.failed(message);
    }

    ClipImporter &m_owner;
    QString m_path;
    QMediaPlayer m_player;
    QVideoSink m_sink;
    QAudioDecoder m_audio;
    QTimer m_stall;
    std::shared_ptr<std::atomic_bool> m_cancelled;

    bool m_loaded = false;
    bool m_durationKnown = false;
    bool m_trimmed = false;
    bool m_heldBack = false; // the player is paused for the encoder to catch up
    bool m_done = false;
    bool m_picturesDone = false;
    bool m_audioDone = false;
    bool m_packing = false;
    qint64 m_clipMs = 0;
    QVector<qint64> m_durations;
    QVector<int> m_frameCounts;
    QSize m_size;
    qint64 m_nextFrame = 0;
    int m_fillSegment = 0;
    QVector<QImage> m_pending;
    QImage m_last;
    QImage m_poster;
    QVector<QVector<ClipContainer::Frame>> m_encoded;
    int m_encodesRunning = 0;
    int m_encodesDone = 0;
    WavAudio m_pcm;
};

ClipImporter::ClipImporter(QObject *parent)
    : ClipImporter(ClipEncodeOptions{}, Limits{}, parent)
{
}

ClipImporter::ClipImporter(ClipEncodeOptions options, Limits limits, QObject *parent)
    : QObject(parent)
    , m_options(std::move(options))
    , m_limits(limits)
{
    qRegisterMetaType<OpenChat::ImportedClip>();
}

ClipImporter::~ClipImporter()
{
    cancel();
}

void ClipImporter::start(const QString &path)
{
    cancel();
    if (!clipCodecAvailable()) {
        QMetaObject::invokeMethod(
            this, [this] { emit failed(QStringLiteral("Videos can't be added on this computer.")); },
            Qt::QueuedConnection);
        return;
    }
    m_run = new Run(*this, path);
    m_run->start();
}

void ClipImporter::cancel()
{
    if (!m_run)
        return;
    Run *run = std::exchange(m_run, nullptr);
    run->halt();
    run->deleteLater();
}

} // namespace OpenChat
