#include "profile/SongImport.h"

#include "domain/ProfilePage.h"
#include "domain/SongContainer.h"

#include <QAudioBuffer>
#include <QAudioDecoder>
#include <QAudioFormat>
#include <QFile>
#include <QFileInfo>
#include <QMediaMetaData>
#include <QMediaPlayer>
#include <QStringDecoder>
#include <QTimer>
#include <QUrl>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

namespace OpenChat {

namespace {

constexpr qsizetype riffHeaderBytes = 12;
constexpr qsizetype chunkHeaderBytes = 8;
// The LIST/INFO walk is bounded: a tag is a courtesy, never a reason to read
// a hostile file's chunk table to its end.
constexpr int maxTagChunks = 64;
constexpr qsizetype maxTagFieldBytes = 4096;
// The Multimedia tag probe gets this long before the file name stands in.
constexpr int tagProbeMs = 3'000;
// How far past a compressed window the decoder listens for more music, to
// tell a window that cuts the song from one that ends where the song does.
constexpr int lookAfterWindowMs = 2'000;
constexpr int peakBlockMs = 10;
constexpr int songWindowMs = SongContainer::maxDurationMs;

// What counts as music when choosing and cutting a window: the encoder's own
// silence threshold, so "audible" means the same on both sides.
[[nodiscard]] double silenceThresholdDb()
{
    static const double threshold = SongEncodeOptions{}.silenceThresholdDb;
    return threshold;
}

[[nodiscard]] bool isRiffWave(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QByteArray header = file.read(riffHeaderBytes);
    return header.size() == riffHeaderBytes && header.startsWith("RIFF") && header.mid(8, 4) == "WAVE";
}

// The |max| of the mid signal per 10 ms block, streamed; reduced to the
// waveform's buckets once the length is known.
class PeakAccumulator final
{
public:
    void feed(const qint16 *interleaved, qsizetype frames, int channels, int sampleRate)
    {
        if (channels <= 0 || sampleRate <= 0)
            return;
        if (m_blockFrames == 0)
            m_blockFrames = std::max(1, sampleRate * peakBlockMs / 1000);
        for (qsizetype frame = 0; frame < frames; ++frame) {
            int sum = 0;
            for (int channel = 0; channel < channels; ++channel)
                sum += interleaved[frame * channels + channel];
            m_current = std::max(m_current, std::abs(sum / channels));
            if (++m_inBlock == m_blockFrames) {
                m_blocks.append(quint16(m_current));
                m_current = 0;
                m_inBlock = 0;
            }
        }
    }

    [[nodiscard]] QVector<quint8> buckets(int count) const
    {
        QVector<quint16> blocks = m_blocks;
        if (m_inBlock > 0)
            blocks.append(quint16(m_current));
        QVector<quint8> out(std::max(count, 0), 0);
        const qsizetype total = blocks.size();
        if (total == 0)
            return out;
        for (int bucket = 0; bucket < count; ++bucket) {
            const qsizetype first = qsizetype(bucket) * total / count;
            const qsizetype last = std::clamp<qsizetype>(qsizetype(bucket + 1) * total / count, first + 1, total);
            quint16 peak = 0;
            for (qsizetype block = first; block < last; ++block)
                peak = std::max(peak, blocks.at(block));
            out[bucket] = quint8(std::min<long>(255, std::lround(peak * 255.0 / 32768.0)));
        }
        return out;
    }

private:
    QVector<quint16> m_blocks;
    int m_blockFrames = 0;
    int m_inBlock = 0;
    int m_current = 0;
};

// INFO text is nominally ASCII; real taggers write UTF-8 or Latin-1.
[[nodiscard]] QString decodeTagText(QByteArray bytes)
{
    if (const qsizetype nul = bytes.indexOf('\0'); nul >= 0)
        bytes.truncate(nul);
    QStringDecoder utf8(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    QString text = utf8(bytes);
    if (utf8.hasError())
        text = QString::fromLatin1(bytes);
    return text;
}

struct WavTags final {
    QString title, artist;
};

// Title (INAM) and artist (IART) from a RIFF LIST/INFO chunk.
[[nodiscard]] WavTags readWavTags(const QByteArray &bytes)
{
    WavTags tags;
    int visited = 0;
    qsizetype cursor = riffHeaderBytes;
    while (cursor + chunkHeaderBytes <= bytes.size() && visited++ < maxTagChunks) {
        const qsizetype payload = cursor + chunkHeaderBytes;
        const qsizetype size = std::min<qsizetype>(qFromLittleEndian<quint32>(bytes.constData() + cursor + 4),
                                                   bytes.size() - payload);
        if (std::memcmp(bytes.constData() + cursor, "LIST", 4) == 0 && size >= 4
            && std::memcmp(bytes.constData() + payload, "INFO", 4) == 0) {
            const qsizetype end = payload + size;
            qsizetype sub = payload + 4;
            while (sub + chunkHeaderBytes <= end && visited++ < maxTagChunks) {
                const qsizetype field = sub + chunkHeaderBytes;
                const qsizetype fieldSize =
                    std::min<qsizetype>(qFromLittleEndian<quint32>(bytes.constData() + sub + 4), end - field);
                const QByteArray value = bytes.mid(field, std::min(fieldSize, maxTagFieldBytes));
                if (std::memcmp(bytes.constData() + sub, "INAM", 4) == 0)
                    tags.title = decodeTagText(value);
                else if (std::memcmp(bytes.constData() + sub, "IART", 4) == 0)
                    tags.artist = decodeTagText(value);
                sub = field + fieldSize + (fieldSize & 1);
            }
        }
        cursor = payload + size + (size & 1);
    }
    return tags;
}

[[nodiscard]] QString durationText(qint64 ms)
{
    const qint64 seconds = (ms + 500) / 1000;
    const qint64 hours = seconds / 3600;
    const qint64 minutes = seconds % 3600 / 60;
    const QString secondsText = QStringLiteral("%1").arg(seconds % 60, 2, 10, QLatin1Char('0'));
    if (hours > 0)
        return QStringLiteral("%1:%2:%3").arg(hours).arg(minutes, 2, 10, QLatin1Char('0')).arg(secondsText);
    return QStringLiteral("%1:%2").arg(minutes).arg(secondsText);
}

[[nodiscard]] QString formatLabel(const QString &format, qint64 durationMs, int sampleRate, int channels)
{
    const QString rate = QString::number(sampleRate / 1000.0, 'g', 6);
    QString layout;
    if (channels == 1)
        layout = QStringLiteral("mono");
    else if (channels == 2)
        layout = QStringLiteral("stereo");
    else
        layout = QStringLiteral("%1-channel").arg(channels);
    const QString dot = QStringLiteral(" %1 ").arg(QChar(0x00B7));
    return format + dot + durationText(durationMs) + dot + rate + QStringLiteral(" kHz ") + layout;
}

// The label's format word for a non-WAV file: its extension, as the user
// knows the file.
[[nodiscard]] QString formatName(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toUpper();
    return suffix.isEmpty() || suffix.size() > 5 ? QStringLiteral("Audio") : suffix;
}

[[nodiscard]] QString titleOrFileName(const QString &title, const QString &path)
{
    const QString tagged = Profile::sanitizeLine(title, Profile::TextBounds::songTitle);
    return tagged.isEmpty()
        ? Profile::sanitizeLine(QFileInfo(path).completeBaseName(), Profile::TextBounds::songTitle)
        : tagged;
}

// The default window starts where the music does, but never so late that a
// full window no longer fits.
[[nodiscard]] qint64 defaultWindowStart(qint64 firstAudibleMs, qint64 durationMs)
{
    const qint64 windowMs = std::min<qint64>(songWindowMs, durationMs);
    return std::clamp<qint64>(firstAudibleMs, 0, std::max<qint64>(0, durationMs - windowMs));
}

[[nodiscard]] SongImportError wavError(WavError error)
{
    switch (error) {
    case WavError::NotFound:
        return SongImportError::FileMissing;
    case WavError::TooLarge:
        return SongImportError::FileTooLarge;
    case WavError::NotRiffWave:
    case WavError::Unsupported:
        return SongImportError::UnsupportedFormat;
    case WavError::MalformedChunk:
    case WavError::MissingFormat:
    case WavError::MissingData:
    case WavError::Empty:
    case WavError::WriteFailed:
        break;
    }
    return SongImportError::DecodeFailed;
}

[[nodiscard]] std::optional<SongImportError> encodeError(SongEncodeError error)
{
    switch (error) {
    case SongEncodeError::Silent:
        return SongImportError::Silent;
    case SongEncodeError::TooShort:
        return SongImportError::TooShort;
    case SongEncodeError::EncoderUnavailable:
    case SongEncodeError::TooLarge:
        return SongImportError::EncodeFailed;
    case SongEncodeError::Cancelled:
        break;
    }
    return std::nullopt; // cancelled: a newer call owns the importer now
}

struct WavSource final {
    WavAudio audio;
    WavTags tags;
};

// Reads a whole WAV (the size was checked before) and keeps at most
// maxSourceMs of it.
[[nodiscard]] Result<WavSource, SongImportError> readWav(const QString &path, const SongImportLimits &limits)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return Result<WavSource, SongImportError>::failure(SongImportError::FileMissing);
    if (file.size() > limits.maxFileBytes)
        return Result<WavSource, SongImportError>::failure(SongImportError::FileTooLarge);
    const QByteArray bytes = file.readAll();
    file.close();
    auto decoded = WavFile::decode(bytes);
    if (!decoded)
        return Result<WavSource, SongImportError>::failure(wavError(decoded.error()));
    WavSource source;
    source.audio = std::move(decoded).value();
    source.tags = readWavTags(bytes);
    const qint64 maxFrames = limits.maxSourceMs * source.audio.sampleRate / 1000;
    if (source.audio.frameCount() > maxFrames)
        source.audio.samples.resize(maxFrames * source.audio.channels);
    return Result<WavSource, SongImportError>::success(std::move(source));
}

// Pass 1 for a WAV, on the worker.
[[nodiscard]] Result<SongSourceInfo, SongImportError> analyseWav(const QString &path, const SongImportLimits &limits)
{
    using Outcome = Result<SongSourceInfo, SongImportError>;
    auto read = readWav(path, limits);
    if (!read)
        return Outcome::failure(read.error());
    const WavSource &source = read.value();
    const WavAudio &audio = source.audio;
    // Refused here rather than after the first encode, so picking such a
    // file says why at once.
    if (audio.durationMs() < SongEncodeOptions{}.minDurationMs)
        return Outcome::failure(SongImportError::TooShort);
    const std::optional<qint64> audible = findAudibleMs(audio, silenceThresholdDb());
    if (!audible)
        return Outcome::failure(SongImportError::Silent);

    PeakAccumulator peaks;
    peaks.feed(audio.samples.constData(), audio.frameCount(), audio.channels, audio.sampleRate);
    SongSourceInfo info;
    info.fileName = QFileInfo(path).fileName();
    info.durationMs = audio.durationMs();
    info.sampleRate = audio.sampleRate;
    info.channels = audio.channels;
    info.formatLabel = formatLabel(QStringLiteral("WAV"), info.durationMs, info.sampleRate, info.channels);
    info.title = titleOrFileName(source.tags.title, path);
    info.artist = Profile::sanitizeLine(source.tags.artist, Profile::TextBounds::songArtist);
    info.peaks = peaks.buckets(limits.peakBuckets);
    info.defaultWindowStartMs = defaultWindowStart(*audible, info.durationMs);
    return Outcome::success(std::move(info));
}

// The 45 s at `startMs` (clamped to fit) as an encoder clip, and whether
// music lies on either side of it in the source.
struct WindowClip final {
    SongClip clip;
    qint64 startMs = 0;
};

[[nodiscard]] WindowClip sliceWindow(const WavAudio &audio, qint64 startMs, double thresholdDb)
{
    const qint64 durationMs = audio.durationMs();
    const qint64 windowMs = std::min<qint64>(songWindowMs, durationMs);
    WindowClip window;
    window.startMs = std::clamp<qint64>(startMs, 0, durationMs - windowMs);
    const qsizetype frames = audio.frameCount();
    const qsizetype first = std::min<qsizetype>(frames, window.startMs * audio.sampleRate / 1000);
    const qsizetype end = std::min<qsizetype>(frames, first + windowMs * audio.sampleRate / 1000);
    window.clip.pcm.sampleRate = audio.sampleRate;
    window.clip.pcm.channels = audio.channels;
    window.clip.pcm.samples = audio.samples.mid(first * audio.channels, (end - first) * audio.channels);
    window.clip.trimmedStart = findAudibleMs(audio, thresholdDb, 0, first).has_value();
    window.clip.trimmedEnd = findAudibleMs(audio, thresholdDb, end).has_value();
    return window;
}

// Any QAudioBuffer as interleaved S16.
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

// A stereo decode of a mono file (the decoder was asked for two channels)
// carries the same samples twice; encoding it as mono keeps the mono bitrate.
void foldDualMono(WavAudio &audio)
{
    if (audio.channels != 2)
        return;
    const qsizetype frames = audio.frameCount();
    for (qsizetype frame = 0; frame < frames; ++frame) {
        if (audio.samples[frame * 2] != audio.samples[frame * 2 + 1])
            return;
    }
    QVector<qint16> mono(frames);
    for (qsizetype frame = 0; frame < frames; ++frame)
        mono[frame] = audio.samples[frame * 2];
    audio.samples = std::move(mono);
    audio.channels = 1;
}

} // namespace

QString songImportErrorText(SongImportError error)
{
    switch (error) {
    case SongImportError::FileMissing:
        return QStringLiteral("That file can't be opened.");
    case SongImportError::FileTooLarge:
        return QStringLiteral("That file is too large. Pick one under 64 MB.");
    case SongImportError::UnsupportedFormat:
        return QStringLiteral("OpenChat can't read this kind of file here. WAV songs always work.");
    case SongImportError::DecoderUnavailable:
        return QStringLiteral("This computer can only import WAV songs.");
    case SongImportError::DecodeFailed:
        return QStringLiteral("That file is damaged or holds no sound.");
    case SongImportError::Silent:
        return QStringLiteral("That part of the song is silent.");
    case SongImportError::TooShort:
        return QStringLiteral("A profile song needs at least a second of sound.");
    case SongImportError::EncodeFailed:
        return QStringLiteral("The song couldn't be prepared. Try another file.");
    case SongImportError::TimedOut:
        return QStringLiteral("Reading that file took too long.");
    }
    return {};
}

// ---------------------------------------------------------------------------
// DecodeRun: one QAudioDecoder pass over a non-WAV file, on the importer's
// thread. Its QObjects are children, and it is only ever deleteLater()'d, so
// nothing is destroyed inside one of its own signals.
// ---------------------------------------------------------------------------

class SongImporter::DecodeRun final : public QObject
{
public:
    DecodeRun(SongImporter &owner, quint64 generation, QString path, bool analyse, qint64 startMs, bool retry)
        : QObject(&owner)
        , m_owner(owner)
        , m_generation(generation)
        , m_path(std::move(path))
        , m_analyse(analyse)
        , m_retry(retry)
        , m_startMs(startMs)
        , m_decoder(this)
        , m_timeout(this)
    {
    }

    void start()
    {
        const SongImportLimits &limits = m_owner.m_limits;
        m_timeout.setSingleShot(true);
        connect(&m_timeout, &QTimer::timeout, this, [this] { fail(SongImportError::TimedOut); });
        connect(&m_decoder, &QAudioDecoder::bufferReady, this, [this] { drain(); });
        connect(&m_decoder, &QAudioDecoder::finished, this, [this] { decodeFinished(); });
        connect(&m_decoder, qOverload<QAudioDecoder::Error>(&QAudioDecoder::error), this,
                [this](QAudioDecoder::Error error) { decodeFailed(error); });
        if (!m_analyse) {
            // Asking for 48 kHz lets the backend's resampler (better than
            // the encoder's linear one) do the conversion. Two channels
            // because the source's count is not known yet; a mono file comes
            // back as identical pairs and is folded back.
            QAudioFormat format;
            format.setSampleRate(SongContainer::sampleRate);
            format.setChannelCount(2);
            format.setSampleFormat(QAudioFormat::Int16);
            m_decoder.setAudioFormat(format);
            if (const auto known = m_owner.m_knownDurations.constFind(m_path);
                known != m_owner.m_knownDurations.cend()) {
                m_windowMs = std::min<qint64>(songWindowMs, *known);
                m_startMs = std::clamp<qint64>(m_startMs, 0, *known - m_windowMs);
            } else {
                m_startMs = std::max<qint64>(0, m_startMs);
            }
        } else {
            startTagProbe();
        }
        m_decoder.setSource(QUrl::fromLocalFile(m_path));
        m_timeout.start(limits.timeoutMs);
        m_decoder.start();
    }

    void halt()
    {
        m_done = true;
        m_timeout.stop();
        m_tagTimeout.stop();
        m_decoder.stop();
        if (m_tagProbe != nullptr)
            m_tagProbe->stop();
    }

private:
    void startTagProbe()
    {
        m_tagsDone = false;
        m_tagProbe = new QMediaPlayer(this);
        m_tagTimeout.setSingleShot(true);
        connect(&m_tagTimeout, &QTimer::timeout, this, [this] { tagsDone(); });
        connect(m_tagProbe, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
            if (status == QMediaPlayer::LoadedMedia || status == QMediaPlayer::BufferedMedia) {
                const QMediaMetaData data = m_tagProbe->metaData();
                m_title = data.stringValue(QMediaMetaData::Title);
                m_artist = data.stringValue(QMediaMetaData::ContributingArtist);
                if (m_artist.isEmpty())
                    m_artist = data.stringValue(QMediaMetaData::AlbumArtist);
                tagsDone();
            } else if (status == QMediaPlayer::InvalidMedia) {
                tagsDone();
            }
        });
        connect(m_tagProbe, &QMediaPlayer::errorOccurred, this, [this] { tagsDone(); });
        m_tagTimeout.start(tagProbeMs);
        m_tagProbe->setSource(QUrl::fromLocalFile(m_path));
    }

    void tagsDone()
    {
        if (m_tagsDone)
            return;
        m_tagsDone = true;
        m_tagTimeout.stop();
        finishAnalysisIfReady();
    }

    void drain()
    {
        while (!m_done && !m_decodeDone && m_decoder.bufferAvailable()) {
            const QAudioBuffer buffer = m_decoder.read();
            if (!buffer.isValid())
                break;
            const QAudioFormat format = buffer.format();
            if (m_rate == 0) {
                m_rate = format.sampleRate();
                m_channels = format.channelCount();
                if (m_rate <= 0 || m_channels <= 0) {
                    fail(SongImportError::DecodeFailed);
                    return;
                }
            } else if (format.sampleRate() != m_rate || format.channelCount() != m_channels) {
                fail(SongImportError::DecodeFailed); // a stream that changes shape midway
                return;
            }
            std::optional<QVector<qint16>> samples = toS16(buffer);
            if (!samples) {
                fail(SongImportError::UnsupportedFormat);
                return;
            }
            if (m_analyse)
                consumeForAnalysis(std::move(*samples));
            else
                consumeForWindow(std::move(*samples));
        }
    }

    void consumeForAnalysis(QVector<qint16> samples)
    {
        const qint64 limitFrames = m_owner.m_limits.maxSourceMs * m_rate / 1000;
        const qint64 frames = std::min<qint64>(samples.size() / m_channels, limitFrames - m_frames);
        if (frames > 0) {
            samples.resize(frames * m_channels);
            m_peaks.feed(samples.constData(), frames, m_channels, m_rate);
            if (!m_firstAudibleMs) {
                const WavAudio chunk{m_rate, m_channels, std::move(samples)};
                if (const std::optional<qint64> at = findAudibleMs(chunk))
                    m_firstAudibleMs = m_frames * 1000 / m_rate + *at;
            }
            m_frames += frames;
        }
        if (const qint64 duration = m_decoder.duration(); duration > 0)
            emit m_owner.progressChanged(std::clamp(0.95 * double(m_frames * 1000 / m_rate) / double(duration), 0.0, 0.95));
        if (m_frames >= limitFrames) {
            // Only the first maxSourceMs can be picked from; the rest need
            // not be decoded.
            m_decoder.stop();
            decodeFinished();
        }
    }

    void consumeForWindow(QVector<qint16> samples)
    {
        const qint64 frames = samples.size() / m_channels;
        const qint64 startFrame = m_startMs * m_rate / 1000;
        const qint64 endFrame = startFrame + m_windowMs * m_rate / 1000;
        const qint64 lookEnd = std::min<qint64>(endFrame + qint64(lookAfterWindowMs) * m_rate / 1000,
                                                m_owner.m_limits.maxSourceMs * m_rate / 1000);
        const WavAudio chunk{m_rate, m_channels, std::move(samples)};
        const qint64 chunkStart = m_frames;
        const auto relative = [chunkStart](qint64 frame, qint64 limit) {
            return qsizetype(std::clamp<qint64>(frame - chunkStart, 0, limit));
        };
        // Before the window: is there music the window cuts off?
        if (!m_audibleBefore && chunkStart < startFrame)
            m_audibleBefore = findAudibleMs(chunk, silenceThresholdDb(), 0, relative(startFrame, frames)).has_value();
        // The window itself.
        const qsizetype from = relative(startFrame, frames);
        const qsizetype to = relative(endFrame, frames);
        if (to > from)
            m_window.append(chunk.samples.mid(from * m_channels, (to - from) * m_channels));
        // After it: more music, or the song's own end?
        if (chunkStart + frames > endFrame)
            m_audibleAfter = m_audibleAfter
                || findAudibleMs(chunk, silenceThresholdDb(), relative(endFrame, frames), relative(lookEnd, frames))
                       .has_value();
        m_frames += frames;
        if (m_decoder.duration() > 0)
            emit m_owner.progressChanged(std::clamp(0.5 * double(m_frames * 1000 / m_rate - m_startMs) / double(m_windowMs), 0.0, 0.5));
        if (m_audibleAfter || m_frames >= lookEnd) {
            m_decoder.stop();
            decodeFinished();
        }
    }

    void decodeFinished()
    {
        if (m_done || m_decodeDone)
            return;
        m_decodeDone = true;
        m_timeout.stop();
        if (m_analyse) {
            finishAnalysisIfReady();
            return;
        }
        const qint64 decodedMs = m_rate > 0 ? m_frames * 1000 / m_rate : 0;
        const qint64 collectedFrames = m_channels > 0 ? m_window.size() / m_channels : 0;
        const qint64 wantedFrames = std::min<qint64>(m_windowMs, decodedMs) * m_rate / 1000;
        if (!m_retry && m_startMs > 0 && collectedFrames + m_rate / 10 < wantedFrames) {
            // The file ended inside the window and its length was not known
            // beforehand. Now it is: decode again, once, from where a full
            // window fits. A fresh run, from the event loop, because a
            // decoder cannot be restarted from inside its own finished().
            const qint64 corrected = std::max<qint64>(0, decodedMs - m_windowMs);
            if (corrected < m_startMs) {
                m_done = true;
                SongImporter &owner = m_owner;
                const quint64 generation = m_generation;
                const QString path = m_path;
                owner.m_knownDurations.insert(path, decodedMs);
                owner.endDecodeRun(); // deletes this later; nothing of it is used below
                QMetaObject::invokeMethod(
                    &owner,
                    [&owner, generation, path, corrected] {
                        if (owner.isCurrent(generation))
                            owner.startDecodeRun(generation, path, false, corrected, true);
                    },
                    Qt::QueuedConnection);
                return;
            }
        }
        if (m_frames == 0 || collectedFrames == 0) {
            fail(SongImportError::DecodeFailed);
            return;
        }
        SongClip clip;
        clip.pcm.sampleRate = m_rate;
        clip.pcm.channels = m_channels;
        clip.pcm.samples = std::move(m_window);
        foldDualMono(clip.pcm);
        clip.trimmedStart = m_audibleBefore;
        clip.trimmedEnd = m_audibleAfter;
        m_done = true;
        SongImporter &owner = m_owner;
        const quint64 generation = m_generation;
        const qint64 startMs = m_startMs;
        owner.endDecodeRun(); // deletes this later; nothing of it is used below
        owner.startEncode(generation, std::move(clip), startMs);
    }

    void finishAnalysisIfReady()
    {
        if (m_done || !m_decodeDone || !m_tagsDone)
            return;
        m_done = true;
        SongImporter &owner = m_owner;
        const quint64 generation = m_generation;
        if (m_frames == 0 || m_rate <= 0) {
            owner.endDecodeRun();
            owner.postFailure(generation, SongImportError::DecodeFailed);
            return;
        }
        SongSourceInfo info;
        info.fileName = QFileInfo(m_path).fileName();
        info.durationMs = m_frames * 1000 / m_rate;
        info.sampleRate = m_rate;
        info.channels = m_channels;
        info.formatLabel = formatLabel(formatName(m_path), info.durationMs, m_rate, m_channels);
        info.title = titleOrFileName(m_title, m_path);
        info.artist = Profile::sanitizeLine(m_artist, Profile::TextBounds::songArtist);
        info.peaks = m_peaks.buckets(owner.m_limits.peakBuckets);
        info.defaultWindowStartMs = defaultWindowStart(m_firstAudibleMs.value_or(0), info.durationMs);
        const QString path = m_path;
        const bool audible = m_firstAudibleMs.has_value();
        owner.endDecodeRun(); // deletes this later; nothing of it is used below
        if (info.durationMs < SongEncodeOptions{}.minDurationMs) {
            owner.postFailure(generation, SongImportError::TooShort);
        } else if (!audible) {
            owner.postFailure(generation, SongImportError::Silent);
        } else {
            QMetaObject::invokeMethod(
                &owner, [&owner, generation, path, info] { owner.deliverAnalysis(generation, path, info); },
                Qt::QueuedConnection);
        }
    }

    void decodeFailed(QAudioDecoder::Error error)
    {
        switch (error) {
        case QAudioDecoder::NoError:
            return;
        case QAudioDecoder::FormatError:
            fail(SongImportError::UnsupportedFormat);
            return;
        case QAudioDecoder::NotSupportedError:
            fail(SongImportError::DecoderUnavailable);
            return;
        case QAudioDecoder::AccessDeniedError:
            fail(SongImportError::FileMissing);
            return;
        case QAudioDecoder::ResourceError:
            break;
        }
        fail(SongImportError::DecodeFailed);
    }

    void fail(SongImportError error)
    {
        if (m_done)
            return;
        halt();
        SongImporter &owner = m_owner;
        const quint64 generation = m_generation;
        owner.endDecodeRun();
        // Queued: a decoder can fail inside start(), which runs inside the
        // caller's analyse() or encodeWindow().
        owner.postFailure(generation, error);
    }

    SongImporter &m_owner;
    const quint64 m_generation;
    const QString m_path;
    const bool m_analyse;
    const bool m_retry; // the second pass of a window placed past the end
    qint64 m_startMs = 0;
    qint64 m_windowMs = songWindowMs;
    QAudioDecoder m_decoder;
    QTimer m_timeout;
    QTimer m_tagTimeout;
    QMediaPlayer *m_tagProbe = nullptr;
    int m_rate = 0;
    int m_channels = 0;
    qint64 m_frames = 0; // decoded so far, at m_rate
    bool m_decodeDone = false;
    bool m_tagsDone = true; // a window pass reads no tags
    bool m_done = false;
    // Analysis
    PeakAccumulator m_peaks;
    std::optional<qint64> m_firstAudibleMs;
    QString m_title, m_artist;
    // Window
    QVector<qint16> m_window;
    bool m_audibleBefore = false;
    bool m_audibleAfter = false;
};

// ---------------------------------------------------------------------------
// SongImporter
// ---------------------------------------------------------------------------

SongImporter::SongImporter(SongImportLimits limits, QObject *parent)
    : QObject(parent)
    , m_limits(limits)
{
    m_pool.setMaxThreadCount(1);
}

SongImporter::~SongImporter()
{
    cancel();
    m_pool.waitForDone();
}

bool SongImporter::canDecodeCompressed()
{
    const QAudioDecoder probe;
    return probe.isSupported();
}

void SongImporter::setForceDecoderForTesting(bool force)
{
    m_forceDecoder = force;
}

bool SongImporter::busy() const
{
    return m_busy;
}

void SongImporter::cancel()
{
    ++m_generation;
    m_busy = false;
    endDecodeRun();
}

quint64 SongImporter::begin()
{
    cancel();
    m_busy = true;
    emit progressChanged(0.0);
    return m_generation.load();
}

bool SongImporter::isCurrent(quint64 generation) const
{
    return generation == m_generation.load();
}

std::optional<SongImportError> SongImporter::checkFile(const QString &path) const
{
    const QFileInfo info(path);
    if (path.isEmpty() || !info.exists() || !info.isFile() || !info.isReadable())
        return SongImportError::FileMissing;
    if (info.size() > m_limits.maxFileBytes)
        return SongImportError::FileTooLarge;
    return std::nullopt;
}

void SongImporter::analyse(const QString &path)
{
    const quint64 generation = begin();
    if (const auto refused = checkFile(path)) {
        postFailure(generation, *refused);
        return;
    }
    if (!m_forceDecoder && isRiffWave(path)) {
        const SongImportLimits limits = m_limits;
        m_pool.start([this, generation, path, limits] {
            if (!isCurrent(generation))
                return;
            auto outcome = std::make_shared<Result<SongSourceInfo, SongImportError>>(analyseWav(path, limits));
            QMetaObject::invokeMethod(
                this,
                [this, generation, path, outcome] {
                    if (*outcome)
                        deliverAnalysis(generation, path, outcome->value());
                    else
                        deliverFailure(generation, outcome->error());
                },
                Qt::QueuedConnection);
        });
        return;
    }
    if (!canDecodeCompressed()) {
        postFailure(generation, SongImportError::DecoderUnavailable);
        return;
    }
    startDecodeRun(generation, path, true, 0);
}

void SongImporter::encodeWindow(const QString &path, qint64 startMs)
{
    const quint64 generation = begin();
    if (const auto refused = checkFile(path)) {
        postFailure(generation, *refused);
        return;
    }
    if (!m_forceDecoder && isRiffWave(path)) {
        const SongImportLimits limits = m_limits;
        m_pool.start([this, generation, path, limits, startMs] {
            if (!isCurrent(generation))
                return;
            WindowClip window;
            {
                auto read = readWav(path, limits);
                if (!read) {
                    QMetaObject::invokeMethod(
                        this, [this, generation, error = read.error()] { deliverFailure(generation, error); },
                        Qt::QueuedConnection);
                    return;
                }
                window = sliceWindow(read.value().audio, startMs, silenceThresholdDb());
            } // the whole source is freed before the encode
            auto encoded = encodeSong(window.clip, {}, [this, generation] { return !isCurrent(generation); });
            if (!encoded) {
                if (const auto error = encodeError(encoded.error())) {
                    QMetaObject::invokeMethod(
                        this, [this, generation, error = *error] { deliverFailure(generation, error); },
                        Qt::QueuedConnection);
                }
                return;
            }
            const QByteArray container = std::move(encoded).value();
            const qint64 durationMs = decodeSongContainer(container).value_or(SongContainer{}).durationMs();
            QMetaObject::invokeMethod(
                this,
                [this, generation, container, durationMs, windowStartMs = window.startMs] {
                    deliverEncoded(generation, container, durationMs, windowStartMs);
                },
                Qt::QueuedConnection);
        });
        return;
    }
    if (!canDecodeCompressed()) {
        postFailure(generation, SongImportError::DecoderUnavailable);
        return;
    }
    startDecodeRun(generation, path, false, startMs);
}

void SongImporter::startDecodeRun(quint64 generation, const QString &path, bool analyse, qint64 startMs,
                                  bool retry)
{
    endDecodeRun();
    m_run = new DecodeRun(*this, generation, path, analyse, startMs, retry);
    m_run->start();
}

void SongImporter::endDecodeRun()
{
    if (m_run == nullptr)
        return;
    DecodeRun *run = m_run;
    m_run = nullptr;
    run->halt();
    run->deleteLater();
}

void SongImporter::startEncode(quint64 generation, SongClip clip, qint64 windowStartMs)
{
    if (!isCurrent(generation))
        return;
    auto shared = std::make_shared<SongClip>(std::move(clip));
    m_pool.start([this, generation, shared, windowStartMs] {
        if (!isCurrent(generation))
            return;
        auto encoded = encodeSong(*shared, {}, [this, generation] { return !isCurrent(generation); });
        if (!encoded) {
            if (const auto error = encodeError(encoded.error())) {
                QMetaObject::invokeMethod(
                    this, [this, generation, error = *error] { deliverFailure(generation, error); },
                    Qt::QueuedConnection);
            }
            return;
        }
        const QByteArray container = std::move(encoded).value();
        const qint64 durationMs = decodeSongContainer(container).value_or(SongContainer{}).durationMs();
        QMetaObject::invokeMethod(
            this,
            [this, generation, container, durationMs, windowStartMs] {
                deliverEncoded(generation, container, durationMs, windowStartMs);
            },
            Qt::QueuedConnection);
    });
}

void SongImporter::postFailure(quint64 generation, SongImportError error)
{
    // Always from the event loop, never inside the caller's analyse() or
    // encodeWindow(): the controller sets its own state after calling.
    QMetaObject::invokeMethod(
        this, [this, generation, error] { deliverFailure(generation, error); }, Qt::QueuedConnection);
}

void SongImporter::deliverFailure(quint64 generation, SongImportError error)
{
    if (!isCurrent(generation))
        return;
    m_busy = false;
    emit failed(error, songImportErrorText(error));
}

void SongImporter::deliverAnalysis(quint64 generation, const QString &path, const SongSourceInfo &info)
{
    if (!isCurrent(generation))
        return;
    m_busy = false;
    m_knownDurations.insert(path, info.durationMs);
    emit progressChanged(1.0);
    emit analysed(info);
}

void SongImporter::deliverEncoded(quint64 generation, const QByteArray &container, qint64 durationMs,
                                  qint64 windowStartMs)
{
    if (!isCurrent(generation))
        return;
    m_busy = false;
    emit progressChanged(1.0);
    emit encoded(container, durationMs, windowStartMs);
}

} // namespace OpenChat
