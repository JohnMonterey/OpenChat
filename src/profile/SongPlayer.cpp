#include "profile/SongPlayer.h"

#include <QAudioSink>
#include <QList>
#include <QMediaDevices>
#include <QMutexLocker>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace OpenChat {

namespace {

constexpr int songRate = SongContainer::sampleRate;
// How long the system output buffers ahead. A song is not latency bound, and
// the sink may be fed from the GUI thread, which a busy page layout can stall
// for longer than the call path's two-frame ring would survive.
constexpr int systemBufferMs = 150;
// Extra time a retired voice is kept after its fade and the device's buffer,
// so the tail is heard before the device stops.
constexpr int retireSlackMs = 50;
constexpr int peakReleaseMs = 50;

[[nodiscard]] double dbToAmplitude(double db)
{
    return std::pow(10.0, db / 20.0);
}

// A one-pole smoothing coefficient: the gain covers 63% of a step in `ms`.
[[nodiscard]] float smoothing(int ms)
{
    return float(1.0 - std::exp(-1.0 / (double(ms) * songRate / 1000.0)));
}

[[nodiscard]] bool isPlayableFormat(QAudioFormat::SampleFormat format)
{
    return format == QAudioFormat::UInt8 || format == QAudioFormat::Int16 || format == QAudioFormat::Int32
        || format == QAudioFormat::Float;
}

// Every live player: SongLibrary tells the ones waiting for a song when it
// arrives. GUI thread only.
QList<SongPlayer *> &livePlayers()
{
    static QList<SongPlayer *> players;
    return players;
}

// The one player allowed to sound.
SongPlayer *&soundingPlayer()
{
    static SongPlayer *player = nullptr;
    return player;
}

SongOutputFactory &outputFactory()
{
    static SongOutputFactory factory;
    return factory;
}

// A QAudioSink on a real device, pulling from the stream.
class SystemSongOutput final : public SongOutput
{
public:
    SystemSongOutput(const QAudioDevice &device, const QAudioFormat &format)
        : m_sink(device, format)
    {
        m_sink.setBufferSize(format.bytesForDuration(qint64(systemBufferMs) * 1000));
        connect(&m_sink, &QAudioSink::stateChanged, this, [this](QAudio::State state) {
            // Running dry is how a pull-mode sink sees the song end (older
            // Qt reports that as an underrun); only these errors mean the
            // device itself went away.
            if (!m_running || state != QAudio::StoppedState)
                return;
            const QAudio::Error error = m_sink.error();
            if (error == QAudio::OpenError || error == QAudio::IOError || error == QAudio::FatalError) {
                m_running = false;
                emit failed(QStringLiteral("The audio output stopped working."));
            }
        });
    }

    ~SystemSongOutput() override { SystemSongOutput::stop(); }

    [[nodiscard]] QAudioFormat format() const override { return m_sink.format(); }

    [[nodiscard]] int bufferMs() const override
    {
        const QAudioFormat format = m_sink.format();
        return int(format.durationForBytes(qint32(m_sink.bufferSize())) / 1000);
    }

    bool start(QIODevice *stream) override
    {
        m_running = true;
        m_sink.start(stream);
        return m_sink.error() == QAudio::NoError;
    }

    void stop() override
    {
        m_running = false;
        m_sink.stop();
    }

private:
    QAudioSink m_sink;
    bool m_running = false;
};

} // namespace

// ---------------------------------------------------------------------------
// SongLibrary
// ---------------------------------------------------------------------------

SongLibrary &SongLibrary::instance()
{
    static SongLibrary library;
    return library;
}

void SongLibrary::put(const QString &key, const QByteArray &container)
{
    if (key.isEmpty() || container.isEmpty())
        return;
    std::erase_if(m_entries, [&key](const Entry &entry) { return entry.key == key; });
    m_entries.push_back({key, container});
    while (m_entries.size() > std::size_t(capacity))
        m_entries.erase(m_entries.begin());
    SongPlayer::songArrived(key);
}

QByteArray SongLibrary::get(const QString &key) const
{
    const auto found =
        std::find_if(m_entries.cbegin(), m_entries.cend(), [&key](const Entry &entry) { return entry.key == key; });
    return found == m_entries.cend() ? QByteArray() : found->container;
}

void SongLibrary::release(const QString &key)
{
    std::erase_if(m_entries, [&key](const Entry &entry) { return entry.key == key; });
}

void SongLibrary::clear()
{
    m_entries.clear();
}

// ---------------------------------------------------------------------------
// SongStream
// ---------------------------------------------------------------------------

SongStream::SongStream(SongContainer song, QAudioFormat sinkFormat, qint64 startSample, QObject *parent)
    : QIODevice(parent)
    , m_decoder(std::move(song))
    , m_format(sinkFormat)
{
    m_songChannels = std::clamp(m_decoder.channels(), 1, 2);
    m_sinkChannels = std::max(0, sinkFormat.channelCount());
    m_sinkRate = std::max(0, sinkFormat.sampleRate());
    m_bytesPerSample = isPlayableFormat(sinkFormat.sampleFormat()) ? sinkFormat.bytesPerSample() : 0;
    m_step = m_sinkRate > 0 ? double(songRate) / m_sinkRate : 1.0;
    m_powerRing.assign(std::size_t(songRate) * rmsWindowMs / 1000, 0.0F);
    m_fadeInTotal = std::max<qint64>(1, qint64(resumeFadeInMs) * std::max(m_sinkRate, 1) / 1000);
    if (startSample > 0) {
        m_decoder.seekToSample(startSample);
        m_fadeInDone = 0;
    } else {
        // The encoder already faded the song's own first samples in.
        m_fadeInDone = m_fadeInTotal;
    }
    m_queueBase = m_decoder.position();
    open(QIODevice::ReadOnly | QIODevice::Unbuffered);
}

SongStream::~SongStream()
{
    close();
}

bool SongStream::isValid() const
{
    QMutexLocker lock(&m_mutex);
    return m_decoder.isValid() && m_sinkChannels > 0 && m_sinkRate > 0 && m_bytesPerSample > 0;
}

qint64 SongStream::bytesAvailable() const
{
    // Some backends ask before they read; the stream is endless until it ends.
    return (finished() ? 0 : 65536) + QIODevice::bytesAvailable();
}

qint64 SongStream::framesPlayed() const
{
    QMutexLocker lock(&m_mutex);
    return std::min(m_decoder.totalSamples(), m_queueBase + qint64(m_readPos));
}

bool SongStream::finished() const
{
    QMutexLocker lock(&m_mutex);
    return finishedLocked();
}

bool SongStream::fadedOut() const
{
    QMutexLocker lock(&m_mutex);
    return m_fadedOut;
}

void SongStream::startFadeOut(int ms)
{
    QMutexLocker lock(&m_mutex);
    if (m_fadedOut || m_fadeOutLeft >= 0)
        return;
    if (ms <= 0 || m_sinkRate <= 0) {
        m_fadedOut = true;
        return;
    }
    m_fadeOutTotal = std::max<qint64>(1, qint64(ms) * m_sinkRate / 1000);
    m_fadeOutLeft = m_fadeOutTotal;
}

void SongStream::seekToSample(qint64 sample)
{
    QMutexLocker lock(&m_mutex);
    m_decoder.seekToSample(sample);
    m_queue.clear();
    m_readPos = 0.0;
    m_realFrames = 0;
    m_flushed = false;
    m_queueBase = m_decoder.position();
    m_fadeInDone = 0;
    // The loudness guard keeps its state: a seek must not reopen a window
    // for a loud song to burst through.
}

qsizetype SongStream::queuedFramesLocked() const
{
    return qsizetype(m_queue.size()) / m_songChannels;
}

bool SongStream::finishedLocked() const
{
    if (m_fadedOut || !m_decoder.isValid() || m_sinkChannels <= 0 || m_sinkRate <= 0 || m_bytesPerSample <= 0)
        return true;
    return m_flushed && qsizetype(m_readPos) >= m_realFrames;
}

void SongStream::refillLocked()
{
    const QVector<qint16> chunk = m_decoder.atEnd() ? QVector<qint16>() : m_decoder.next();
    if (chunk.isEmpty()) {
        // One silent frame past the end, so the last real frame has a
        // neighbour to interpolate against.
        m_queue.insert(m_queue.end(), std::size_t(m_songChannels), 0.0F);
        m_flushed = true;
        return;
    }
    const qsizetype frames = chunk.size() / m_songChannels;
    const std::size_t base = m_queue.size();
    m_queue.resize(base + std::size_t(frames) * m_songChannels);
    float *out = m_queue.data() + base;
    for (qsizetype frame = 0; frame < frames; ++frame) {
        for (int channel = 0; channel < m_songChannels; ++channel)
            out[channel] = float(chunk[frame * m_songChannels + channel]) / 32768.0F;
        guardFrameLocked(out);
        out += m_songChannels;
    }
    m_realFrames += frames;
}

void SongStream::guardFrameLocked(float *frame)
{
    static const float attack = smoothing(attackMs);
    static const float release = smoothing(releaseMs);
    static const float peakRelease = smoothing(peakReleaseMs);
    static const double rmsCeiling = dbToAmplitude(rmsCeilingDb);
    static const float peakCeiling = float(dbToAmplitude(peakCeilingDb));

    // Short-term power over the last 400 ms, as a running sum over a ring.
    double power = 0.0;
    for (int channel = 0; channel < m_songChannels; ++channel)
        power += double(frame[channel]) * frame[channel];
    power /= m_songChannels;
    m_powerSum += power - m_powerRing[std::size_t(m_ringIndex)];
    m_powerRing[std::size_t(m_ringIndex)] = float(power);
    if (++m_ringIndex == qsizetype(m_powerRing.size())) {
        // Re-add from scratch once per window so rounding cannot drift.
        m_ringIndex = 0;
        m_powerSum = 0.0;
        for (const float value : m_powerRing)
            m_powerSum += value;
    }
    const double meanPower = std::max(0.0, m_powerSum / double(m_powerRing.size()));
    const float target =
        meanPower > rmsCeiling * rmsCeiling ? float(rmsCeiling / std::sqrt(meanPower)) : 1.0F;
    m_rmsGain += (target - m_rmsGain) * (target < m_rmsGain ? attack : release);

    // Then no sample above -1 dBFS: the gain drops at once and recovers.
    float peak = 0.0F;
    for (int channel = 0; channel < m_songChannels; ++channel) {
        frame[channel] *= m_rmsGain;
        peak = std::max(peak, std::abs(frame[channel]));
    }
    if (peak * m_peakGain > peakCeiling)
        m_peakGain = peakCeiling / peak;
    else
        m_peakGain += (1.0F - m_peakGain) * peakRelease;
    for (int channel = 0; channel < m_songChannels; ++channel)
        frame[channel] *= m_peakGain;
}

void SongStream::writeFrameLocked(char *out, const float *song, float gain) const
{
    const float left = song[0];
    const float right = m_songChannels == 2 ? song[1] : song[0];
    for (int channel = 0; channel < m_sinkChannels; ++channel) {
        float value = 0.0F;
        if (m_sinkChannels == 1)
            value = 0.5F * (left + right);
        else if (channel == 0)
            value = left;
        else if (channel == 1)
            value = right;
        value = std::clamp(value * gain, -1.0F, 1.0F);
        char *sample = out + qsizetype(channel) * m_bytesPerSample;
        switch (m_format.sampleFormat()) {
        case QAudioFormat::UInt8: {
            const auto converted = quint8(std::lround(value * 127.0F + 128.0F));
            std::memcpy(sample, &converted, sizeof(converted));
            break;
        }
        case QAudioFormat::Int16: {
            const auto converted = qint16(std::lround(value * 32767.0F));
            std::memcpy(sample, &converted, sizeof(converted));
            break;
        }
        case QAudioFormat::Int32: {
            const auto converted = qint32(std::llround(double(value) * 2147483647.0));
            std::memcpy(sample, &converted, sizeof(converted));
            break;
        }
        case QAudioFormat::Float:
            std::memcpy(sample, &value, sizeof(value));
            break;
        default:
            break;
        }
    }
}

qint64 SongStream::readData(char *data, qint64 maxSize)
{
    QMutexLocker lock(&m_mutex);
    if (finishedLocked())
        return 0;
    const qint64 frameBytes = qint64(m_sinkChannels) * m_bytesPerSample;
    const qint64 wanted = maxSize / frameBytes;
    qint64 written = 0;
    const int channels = m_songChannels;
    while (written < wanted) {
        const auto index = qsizetype(m_readPos);
        while (index + 1 >= queuedFramesLocked() && !m_flushed)
            refillLocked();
        if (index >= m_realFrames || index + 1 >= queuedFramesLocked())
            break; // the song's last frame has gone out
        float gain = float(playbackGain);
        if (m_fadeOutLeft >= 0) {
            if (m_fadeOutLeft == 0) {
                m_fadedOut = true;
                break;
            }
            gain *= float(m_fadeOutLeft) / float(m_fadeOutTotal);
            --m_fadeOutLeft;
        }
        if (m_fadeInDone < m_fadeInTotal) {
            ++m_fadeInDone;
            gain *= float(m_fadeInDone) / float(m_fadeInTotal);
        }
        const float fraction = float(m_readPos - double(index));
        const float *a = m_queue.data() + index * channels;
        const float *b = a + channels;
        float frame[2] = {0.0F, 0.0F};
        for (int channel = 0; channel < channels; ++channel)
            frame[channel] = a[channel] + (b[channel] - a[channel]) * fraction;
        writeFrameLocked(data + written * frameBytes, frame, gain);
        ++written;
        m_readPos += m_step;
    }
    // Drop what the read position has passed, keeping the frame it sits on.
    const qsizetype consumed = std::min(qsizetype(m_readPos), m_realFrames);
    if (consumed > 0) {
        m_queue.erase(m_queue.begin(), m_queue.begin() + consumed * channels);
        m_queueBase += consumed;
        m_readPos -= double(consumed);
        m_realFrames -= consumed;
    }
    return written * frameBytes;
}

// ---------------------------------------------------------------------------
// SongPlayer
// ---------------------------------------------------------------------------

SongPlayer::SongPlayer(QObject *parent)
    : QObject(parent)
{
    m_positionTimer.setInterval(positionIntervalMs);
    connect(&m_positionTimer, &QTimer::timeout, this, &SongPlayer::updatePosition);
    m_retireTimer.setSingleShot(true);
    connect(&m_retireTimer, &QTimer::timeout, this, &SongPlayer::killRetiring);
    livePlayers().append(this);
}

SongPlayer::~SongPlayer()
{
    m_positionTimer.stop();
    m_retireTimer.stop();
    for (Voice *voice : {&m_voice, &m_retiring}) {
        if (voice->output)
            voice->output->stop();
        voice->output.reset();
        voice->stream.reset();
    }
    livePlayers().removeOne(this);
    if (soundingPlayer() == this)
        soundingPlayer() = nullptr;
}

QString SongPlayer::songKey() const
{
    return m_key;
}

void SongPlayer::setSongKey(const QString &key)
{
    if (key == m_key)
        return;
    const bool wasPlaying = m_playing;
    const bool hadError = !m_error.isEmpty();
    if (m_voice)
        retireVoice(fadeOutMs);
    m_playing = false;
    m_error.clear();
    m_key = key;
    load();
    setPositionMs(0);
    emit sourceChanged();
    if (wasPlaying || hadError)
        emit stateChanged();
}

bool SongPlayer::valid() const
{
    return m_song.has_value();
}

qint64 SongPlayer::durationMs() const
{
    return m_durationMs;
}

qint64 SongPlayer::positionMs() const
{
    return m_positionMs;
}

bool SongPlayer::playing() const
{
    return m_playing;
}

bool SongPlayer::suspended() const
{
    return m_suspended;
}

void SongPlayer::setSuspended(bool suspended)
{
    if (suspended == m_suspended)
        return;
    m_suspended = suspended;
    if (suspended && m_playing) {
        // A call owns the speakers: pause where the listener was, and wait
        // for them to press play again after the call.
        updatePosition();
        retireVoice(fadeOutMs);
        m_playing = false;
    }
    emit stateChanged();
}

bool SongPlayer::active() const
{
    return m_active;
}

void SongPlayer::setActive(bool active)
{
    if (active == m_active)
        return;
    m_active = active;
    if (!active) {
        // The page left the screen: the song stops and starts over next time.
        if (m_voice)
            retireVoice(fadeOutMs);
        m_playing = false;
        setPositionMs(0);
    }
    emit stateChanged();
}

QString SongPlayer::error() const
{
    return m_error;
}

void SongPlayer::play()
{
    if (m_playing || !m_active || m_suspended || !m_song)
        return;
    if (SongPlayer *other = soundingPlayer(); other != nullptr && other != this)
        other->pause();

    QString error;
    std::unique_ptr<SongOutput> output;
    if (const SongOutputFactory &factory = outputFactory())
        output = factory(m_song->channels, error);
    else
        output = openSystemOutput(QMediaDevices::defaultAudioOutput(), m_song->channels, error);
    if (!output) {
        setError(error.isEmpty() ? QStringLiteral("No audio output") : error);
        return;
    }

    const qint64 startMs = m_positionMs >= m_durationMs ? 0 : m_positionMs;
    auto stream = std::make_unique<SongStream>(*m_song, output->format(), startMs * songRate / 1000);
    if (!stream->isValid() || !output->start(stream.get())) {
        output->stop();
        output.reset();
        setError(QStringLiteral("The audio output can't play this song."));
        return;
    }
    const SongOutput *raw = output.get();
    connect(
        output.get(), &SongOutput::failed, this,
        [this, raw](const QString &message) { outputFailed(raw, message); }, Qt::QueuedConnection);
    m_voice.stream = std::move(stream);
    m_voice.output = std::move(output);
    m_voiceStartMs = startMs;
    soundingPlayer() = this;
    m_playing = true;
    m_error.clear();
    setPositionMs(startMs);
    m_positionTimer.start();
    emit stateChanged();
}

void SongPlayer::pause()
{
    if (!m_playing)
        return;
    updatePosition();
    retireVoice(fadeOutMs);
    m_playing = false;
    emit stateChanged();
}

void SongPlayer::stop()
{
    const bool wasPlaying = m_playing;
    if (m_voice)
        retireVoice(fadeOutMs);
    m_playing = false;
    setPositionMs(0);
    if (wasPlaying)
        emit stateChanged();
}

void SongPlayer::toggle()
{
    if (m_playing)
        pause();
    else
        play();
}

void SongPlayer::seek(qint64 positionMs)
{
    if (!m_song)
        return;
    const qint64 target = std::clamp<qint64>(positionMs, 0, m_durationMs);
    if (m_voice) {
        m_voice.stream->seekToSample(target * songRate / 1000);
        m_voiceStartMs = target;
    }
    setPositionMs(target);
}

void SongPlayer::seekBy(qint64 deltaMs)
{
    seek(m_positionMs + deltaMs);
}

std::unique_ptr<SongOutput> SongPlayer::openSystemOutput(const QAudioDevice &device, int channels, QString &error)
{
    if (device.isNull()) {
        error = QStringLiteral("No audio output");
        return {};
    }
    // 48 kHz with the song's own channels when the device takes it; else the
    // device's preferred rate and layout (as CaseAudio does), which the
    // stream resamples and maps to.
    QAudioFormat format;
    format.setSampleRate(songRate);
    format.setChannelCount(std::clamp(channels, 1, 2));
    format.setSampleFormat(QAudioFormat::Int16);
    if (!device.isFormatSupported(format)) {
        format = device.preferredFormat();
        QAudioFormat int16 = format;
        int16.setSampleFormat(QAudioFormat::Int16);
        if (device.isFormatSupported(int16))
            format = int16;
    }
    if (!format.isValid() || format.channelCount() <= 0 || !isPlayableFormat(format.sampleFormat())) {
        error = QStringLiteral("No audio output");
        return {};
    }
    return std::make_unique<SystemSongOutput>(device, format);
}

void SongPlayer::setOutputFactoryForTesting(SongOutputFactory factory)
{
    outputFactory() = std::move(factory);
}

void SongPlayer::songArrived(const QString &key)
{
    // Copied: a slot reacting to sourceChanged could create or destroy a player.
    const QList<SongPlayer *> players = livePlayers();
    for (SongPlayer *player : players) {
        if (!livePlayers().contains(player) || player->m_key != key || player->m_loaded)
            continue;
        player->load();
        if (player->m_loaded)
            emit player->sourceChanged();
    }
}

void SongPlayer::load()
{
    m_song.reset();
    m_loaded = false;
    m_durationMs = 0;
    if (m_key.isEmpty())
        return;
    const QByteArray bytes = SongLibrary::instance().get(m_key);
    if (bytes.isEmpty())
        return; // not arrived yet: SongLibrary::put() calls back
    m_loaded = true;
    std::optional<SongContainer> song = decodeSongContainer(bytes);
    if (!song)
        return;
    // A decoder checks every packet's TOC; a song it refuses is "Can't play
    // this song on this computer.", never a stream that stops half way.
    if (!SongDecoder(*song).isValid())
        return;
    m_durationMs = song->durationMs();
    m_song = std::move(song);
}

void SongPlayer::retireVoice(int fadeMs)
{
    m_positionTimer.stop();
    if (soundingPlayer() == this)
        soundingPlayer() = nullptr;
    if (!m_voice)
        return;
    killRetiring();
    m_retiring.stream = std::move(m_voice.stream);
    m_retiring.output = std::move(m_voice.output);
    if (fadeMs > 0)
        m_retiring.stream->startFadeOut(fadeMs);
    m_retireTimer.start(fadeMs + m_retiring.output->bufferMs() + retireSlackMs);
}

void SongPlayer::killRetiring()
{
    m_retireTimer.stop();
    if (m_retiring.output)
        m_retiring.output->stop();
    m_retiring.output.reset();
    m_retiring.stream.reset();
}

void SongPlayer::updatePosition()
{
    if (!m_voice)
        return;
    // What the device has pulled is ahead of the speaker by its buffer.
    const qint64 pulledMs = m_voice.stream->framesPlayed() * 1000 / songRate;
    setPositionMs(std::clamp<qint64>(pulledMs - m_voice.output->bufferMs(), m_voiceStartMs,
                                     std::max(m_voiceStartMs, m_durationMs)));
    if (m_voice.stream->finished()) {
        // Played to the end: let the device play out what it holds, rewind.
        retireVoice(0);
        m_playing = false;
        setPositionMs(0);
        emit stateChanged();
    }
}

void SongPlayer::setPositionMs(qint64 positionMs)
{
    if (positionMs == m_positionMs)
        return;
    m_positionMs = positionMs;
    emit positionChanged();
}

void SongPlayer::setError(const QString &error)
{
    if (error == m_error)
        return;
    m_error = error;
    emit stateChanged();
}

void SongPlayer::outputFailed(const SongOutput *output, const QString &message)
{
    // Queued, so the output is never destroyed inside its own signal; the
    // pointer is only compared, never followed.
    if (output == m_retiring.output.get()) {
        killRetiring();
        return;
    }
    if (output != m_voice.output.get())
        return;
    updatePosition();
    m_positionTimer.stop();
    if (soundingPlayer() == this)
        soundingPlayer() = nullptr;
    m_voice.output->stop();
    m_voice.output.reset();
    m_voice.stream.reset();
    m_playing = false;
    m_error = message;
    emit stateChanged();
}

} // namespace OpenChat
