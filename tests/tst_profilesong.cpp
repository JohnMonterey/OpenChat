#include "domain/Attachment.h"
#include "domain/ProfilePageCodec.h"
#include "domain/SongContainer.h"
#include "media/SongCodec.h"
#include "media/WavFile.h"
#include "profile/SongImport.h"
#include "profile/SongPlayer.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QFile>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>

#include <opus.h>

#include <cmath>
#include <functional>
#include <memory>
#include <numbers>
#include <random>
#include <vector>

using namespace OpenChat;
using namespace Qt::StringLiterals;

namespace {

constexpr int songRate = SongContainer::sampleRate;

// The playback levels ARCH §5.5 fixes, spelled out rather than read back from
// SongStream: a weaker guard must fail here, not move the expectations with it.
constexpr double specPlaybackGain = 0.56;
constexpr double specRmsCeilingDb = -14.0;
constexpr double specPeakCeilingDb = -1.0;
static_assert(SongStream::playbackGain == specPlaybackGain);
static_assert(SongStream::rmsCeilingDb == specRmsCeilingDb);
static_assert(SongStream::peakCeilingDb == specPeakCeilingDb);

// ---------------------------------------------------------------------------
// Signals and measurements
// ---------------------------------------------------------------------------

// `seconds` of audio whose sample in `channel` at time `t` is signal(channel, t)
// (full scale = 1.0).
WavAudio synth(int rate, int channels, double seconds, const std::function<double(int, double)> &signal)
{
    WavAudio audio;
    audio.sampleRate = rate;
    audio.channels = channels;
    const auto frames = qsizetype(std::llround(seconds * rate));
    audio.samples.resize(frames * channels);
    for (qsizetype frame = 0; frame < frames; ++frame) {
        const double t = double(frame) / rate;
        for (int channel = 0; channel < channels; ++channel) {
            const double value = std::clamp(signal(channel, t) * 32768.0, -32768.0, 32767.0);
            audio.samples[frame * channels + channel] = qint16(std::lround(value));
        }
    }
    return audio;
}

double sine(double frequency, double t)
{
    return std::sin(2.0 * std::numbers::pi * frequency * t);
}

WavAudio tone(double frequency, double seconds, int rate = songRate, int channels = 1, double amplitude = 0.5)
{
    return synth(rate, channels, seconds, [=](int, double t) { return amplitude * sine(frequency, t); });
}

// Deterministic white noise, independent per channel.
WavAudio noise(double seconds, int rate, int channels, double amplitude, unsigned seed = 7)
{
    std::mt19937 generator(seed);
    std::uniform_real_distribution<double> uniform(-1.0, 1.0);
    return synth(rate, channels, seconds, [&](int, double) { return amplitude * uniform(generator); });
}

double dbToAmplitude(double db)
{
    return std::pow(10.0, db / 20.0);
}

double amplitudeToDb(double amplitude)
{
    return 20.0 * std::log10(std::max(amplitude, 1e-12));
}

// RMS of one channel over frames [from, from + count), full scale = 1.0.
double rms(const QVector<qint16> &samples, int channels, int channel, qsizetype from, qsizetype count)
{
    double sum = 0.0;
    for (qsizetype frame = from; frame < from + count; ++frame) {
        const double value = samples.at(frame * channels + channel) / 32768.0;
        sum += value * value;
    }
    return count > 0 ? std::sqrt(sum / double(count)) : 0.0;
}

double rmsMs(const QVector<qint16> &samples, int channels, int channel, double fromMs, double toMs, int rate = songRate)
{
    const auto from = qsizetype(fromMs * rate / 1000.0);
    const auto to = qsizetype(toMs * rate / 1000.0);
    return rms(samples, channels, channel, from, to - from);
}

double peakOf(const QVector<qint16> &samples)
{
    int peak = 0;
    for (const qint16 sample : samples)
        peak = std::max(peak, std::abs(int(sample)));
    return peak / 32768.0;
}

// Goertzel power of one frequency in one channel.
double goertzel(const QVector<qint16> &samples, int channels, int channel, qsizetype from, qsizetype count, int rate,
                double frequency)
{
    const double coefficient = 2.0 * std::cos(2.0 * std::numbers::pi * frequency / rate);
    double s1 = 0.0;
    double s2 = 0.0;
    for (qsizetype frame = from; frame < from + count; ++frame) {
        const double s0 = samples.at(frame * channels + channel) / 32768.0 + coefficient * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return s1 * s1 + s2 * s2 - coefficient * s1 * s2;
}

// True when `frequency` holds at least 20× the power of every rival.
bool dominates(const QVector<qint16> &samples, int channels, int channel, double fromSeconds, double seconds,
               int rate, double frequency, const QList<double> &rivals)
{
    const auto from = qsizetype(fromSeconds * rate);
    const auto count = qsizetype(seconds * rate);
    const double wanted = goertzel(samples, channels, channel, from, count, rate, frequency);
    for (const double rival : rivals) {
        if (wanted < 20.0 * goertzel(samples, channels, channel, from, count, rate, rival))
            return false;
    }
    return wanted > 0.0;
}

QByteArray encodeOrFail(const WavAudio &pcm, bool trimmedStart = false, bool trimmedEnd = false,
                        const SongEncodeOptions &options = {})
{
    auto result = encodeSong(SongClip{pcm, trimmedStart, trimmedEnd}, options);
    if (!result) {
        qWarning() << "encodeSong failed with" << int(result.error());
        return {};
    }
    return std::move(result).value();
}

QVector<qint16> decodeAll(SongDecoder &decoder)
{
    QVector<qint16> out;
    while (!decoder.atEnd())
        out += decoder.next();
    return out;
}

QVector<qint16> decodeAll(const QByteArray &container)
{
    const std::optional<SongContainer> song = decodeSongContainer(container);
    if (!song)
        return {};
    SongDecoder decoder(*song);
    return decodeAll(decoder);
}

// Average bitrate of a container's packets.
double bitrateOf(const SongContainer &song)
{
    qint64 bytes = 0;
    for (const QByteArray &packet : song.packets)
        bytes += packet.size();
    const double seconds = double(song.packets.size()) * song.frameSamples / songRate;
    return double(bytes) * 8.0 / seconds;
}

// ---------------------------------------------------------------------------
// WAV files as the importer meets them
// ---------------------------------------------------------------------------

struct WavSpec {
    int bits = 16;
    bool isFloat = false;
    quint16 formatTag = 0; // 0: PCM or IEEE float by isFloat
    QByteArray title;      // INAM, raw bytes; empty = no LIST chunk
    QByteArray artist;     // IART
    bool listAfterData = false;
    bool junkChunk = false;
};

void appendU16(QByteArray &out, quint16 value)
{
    char bytes[2];
    qToLittleEndian(value, bytes);
    out.append(bytes, 2);
}

void appendU32(QByteArray &out, quint32 value)
{
    char bytes[4];
    qToLittleEndian(value, bytes);
    out.append(bytes, 4);
}

void appendChunk(QByteArray &out, const char *id, const QByteArray &payload)
{
    out.append(id, 4);
    appendU32(out, quint32(payload.size()));
    out.append(payload);
    if (payload.size() % 2 != 0)
        out.append('\0');
}

QByteArray makeWav(const WavAudio &audio, const WavSpec &spec = {})
{
    const int bytesPerSample = spec.bits / 8;
    QByteArray format;
    appendU16(format, spec.formatTag != 0 ? spec.formatTag : (spec.isFloat ? 3 : 1));
    appendU16(format, quint16(audio.channels));
    appendU32(format, quint32(audio.sampleRate));
    appendU32(format, quint32(audio.sampleRate * audio.channels * bytesPerSample));
    appendU16(format, quint16(audio.channels * bytesPerSample));
    appendU16(format, quint16(spec.bits));

    QByteArray data;
    data.reserve(audio.samples.size() * bytesPerSample);
    for (const qint16 sample : audio.samples) {
        if (spec.isFloat) {
            const float value = float(sample) / 32768.0F;
            quint32 bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            appendU32(data, bits);
        } else if (spec.bits == 8) {
            data.append(char(quint8((sample >> 8) + 128)));
        } else if (spec.bits == 16) {
            appendU16(data, quint16(sample));
        } else if (spec.bits == 24) {
            const qint32 value = qint32(sample) * 256;
            data.append(char(value & 0xFF));
            data.append(char((value >> 8) & 0xFF));
            data.append(char((value >> 16) & 0xFF));
        } else {
            appendU32(data, quint32(qint32(sample) * 65536));
        }
    }

    QByteArray list;
    if (!spec.title.isEmpty() || !spec.artist.isEmpty()) {
        QByteArray info("INFO");
        if (!spec.title.isEmpty())
            appendChunk(info, "INAM", spec.title + '\0');
        if (!spec.artist.isEmpty())
            appendChunk(info, "IART", spec.artist + '\0');
        appendChunk(list, "LIST", info);
    }

    QByteArray body("WAVE");
    appendChunk(body, "fmt ", format);
    if (spec.junkChunk)
        appendChunk(body, "junk", QByteArray(37, 'j'));
    if (!spec.listAfterData)
        body.append(list);
    appendChunk(body, "data", data);
    if (spec.listAfterData)
        body.append(list);
    QByteArray file("RIFF");
    appendU32(file, quint32(body.size()));
    file.append(body);
    return file;
}

// ---------------------------------------------------------------------------
// A fake audio output: tests pull from the stream themselves, in "real time"
// only as fast as they ask.
// ---------------------------------------------------------------------------

struct FakeOutputState {
    QAudioFormat format;
    QIODevice *stream = nullptr;
    SongOutput *output = nullptr; // while it lives: a test makes the device fail through it
    bool started = false;
    bool stopped = false;
    bool destroyed = false;
};

class FakeSongOutput final : public SongOutput
{
public:
    explicit FakeSongOutput(std::shared_ptr<FakeOutputState> state)
        : m_state(std::move(state))
    {
        m_state->output = this;
    }
    ~FakeSongOutput() override
    {
        m_state->stream = nullptr;
        m_state->output = nullptr;
        m_state->destroyed = true;
    }
    [[nodiscard]] QAudioFormat format() const override { return m_state->format; }
    [[nodiscard]] int bufferMs() const override { return 0; }
    bool start(QIODevice *stream) override
    {
        m_state->stream = stream;
        m_state->started = true;
        return true;
    }
    void stop() override
    {
        m_state->stream = nullptr;
        m_state->stopped = true;
    }

private:
    std::shared_ptr<FakeOutputState> m_state;
};

QAudioFormat pcmFormat(int rate, int channels, QAudioFormat::SampleFormat sampleFormat = QAudioFormat::Int16)
{
    QAudioFormat format;
    format.setSampleRate(rate);
    format.setChannelCount(channels);
    format.setSampleFormat(sampleFormat);
    return format;
}

// Pulls `ms` of audio (or what is left) from an output's stream.
QVector<qint16> pull(FakeOutputState &state, int ms)
{
    if (state.stream == nullptr)
        return {};
    const QByteArray bytes = state.stream->read(state.format.bytesForDuration(qint64(ms) * 1000));
    QVector<qint16> samples(bytes.size() / 2);
    std::memcpy(samples.data(), bytes.constData(), std::size_t(samples.size()) * 2);
    return samples;
}

// Pulls until the stream gives no more (the song's end, or a fade's);
// returns how many frames came.
qsizetype pullToTheEnd(FakeOutputState &state)
{
    qsizetype frames = 0;
    for (;;) {
        const QVector<qint16> chunk = pull(state, 1'000);
        if (chunk.isEmpty())
            return frames;
        frames += chunk.size() / std::max(1, state.format.channelCount());
    }
}

// Everything a stream gives, in 4 KiB reads, as the sink would take it.
QByteArray readAll(QIODevice &stream)
{
    QByteArray out;
    for (;;) {
        const QByteArray chunk = stream.read(4096);
        if (chunk.isEmpty())
            return out;
        out += chunk;
    }
}

// How many frames a stream gives to its end, read a mebibyte at a time
// (five minutes of stereo is too much to keep).
qint64 countFrames(QIODevice &stream, int channels)
{
    qint64 bytes = 0;
    for (;;) {
        const QByteArray chunk = stream.read(1024 * 1024);
        if (chunk.isEmpty())
            return bytes / (2 * channels);
        bytes += chunk.size();
    }
}

QVector<qint16> asS16(const QByteArray &bytes)
{
    QVector<qint16> samples(bytes.size() / 2);
    std::memcpy(samples.data(), bytes.constData(), std::size_t(samples.size()) * 2);
    return samples;
}

const QString keyA = QString(64, u'a');
const QString keyB = QString(64, u'b');
const QString keyC = QString(64, u'c');

} // namespace

class ProfileSongTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    // Codec
    void encodeFitsTheBudgetFor45sStereoNoise();
    void bitrateStepsDownUntilItFits();
    void encodeRejectsSilenceAndTooShort();
    void trimmedEdgesFadeOverHalfASecond();
    void untrimmedEdgesUseShortFades();
    void loudnessIsNormalisedAndPeaksLimited();
    void decodeRoundTripKeepsPitch();
    void decoderHonoursPreSkipAndLength();
    void decoderRejectsWrongPacketDurations();
    void decoderConcealsACorruptPacket();
    void channelCountsAreKept();
    void chatOptionsKeepFiveMinutesWithinTheirLimits();
    void chatOptionsTakeShortQuietClips();
    void monoFallbackFitsWhatNoStereoRungCan();
    void encodeProgressNeverGoesBack();

    // Import
    void analyseReadsWavPeaksDurationAndInfoTags_data();
    void analyseReadsWavPeaksDurationAndInfoTags();
    void encodeWindowSlicesTheChosenPart();
    void newerWindowCancelsTheOlderEncode();
    void workerThatThrowsFailsInsteadOfAborting();
    void importRefusesUnreadableAndOversizedFiles();
    void importCompressedWhenDecoderAvailable();
    void analyseKeepsOnlyTheSourceLimit();
    void encodeWholeKeepsTheFirstFiveMinutesOfAWav();
    void encodeWholeTakesAShortQuietVoiceNote();
    void encodeWholeFindsTheSamplesBehindOtherChunks();
    void encodeWholeRefusesSilence();
    void encodeWholeThroughTheDecoder();
    void encodeWholeStreamsAWavTooDenseToReadWhole();

    // Playback
    void songStreamProducesDecodedPcmAndEnds();
    void playbackGuardTamesAFullScaleSquareWave();
    void setSongKeyWithTheSameKeyIsANoOp();
    void onlyOnePlayerPlaysAtATime();
    void inactivePlayerFadesAndStops();
    void playerNeverStartsByItself();
    void playerRefusesWhileSuspended();
    void playerWithoutDeviceReportsError();
    void playerStopsAndRewindsAtTheEnd();
    void outputFailureStopsThePlayerAndSaysSo();
    void outputFailureAfterTheSongEndedIsHarmless();
    void songLibraryKeepsTheThreeLatestSongs();
    void fiveMinuteSongPlaysToTheEndAndSeeks();
    void longFormPlayerPlaysAChatSong();

private:
    QString writeFile(const QString &name, const QByteArray &bytes);
    void installFakeOutput();
    const QByteArray &chatSong();

    QTemporaryDir m_dir;
    QByteArray m_monoSong;   // 3 s of 440 Hz, mono
    QByteArray m_stereoSong; // 2 s, 440 Hz left and 660 Hz right
    // A chat's audio attachment, made once when a test first asks: 5:10 of
    // stereo in, the first five minutes out. 440 Hz left (880 Hz from 4:30),
    // 660 Hz right.
    QByteArray m_chatSong;
    std::vector<std::shared_ptr<FakeOutputState>> m_outputs;
    int m_factoryCalls = 0;
};

void ProfileSongTest::initTestCase()
{
    qRegisterMetaType<SongSourceInfo>();
    qRegisterMetaType<SongImportError>();
    QVERIFY(m_dir.isValid());
    m_monoSong = encodeOrFail(tone(440.0, 3.0));
    QVERIFY(!m_monoSong.isEmpty());
    m_stereoSong = encodeOrFail(synth(songRate, 2, 2.0, [](int channel, double t) {
        return 0.4 * sine(channel == 0 ? 440.0 : 660.0, t);
    }));
    QVERIFY(!m_stereoSong.isEmpty());
}

void ProfileSongTest::init()
{
    m_outputs.clear();
    m_factoryCalls = 0;
    SongLibrary::instance().clear();
    installFakeOutput();
}

void ProfileSongTest::cleanup()
{
    SongPlayer::setOutputFactoryForTesting({});
    SongLibrary::instance().clear();
}

QString ProfileSongTest::writeFile(const QString &name, const QByteArray &bytes)
{
    const QString path = m_dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(bytes) != bytes.size())
        qFatal("cannot write %s", qPrintable(path));
    return path;
}

const QByteArray &ProfileSongTest::chatSong()
{
    if (m_chatSong.isEmpty()) {
        const WavAudio pcm = synth(songRate, 2, 310.0, [](int channel, double t) {
            return 0.3 * sine(channel == 1 ? 660.0 : t < 270.0 ? 440.0 : 880.0, t);
        });
        m_chatSong = encodeOrFail(pcm, false, false, chatSongEncodeOptions());
    }
    return m_chatSong;
}

void ProfileSongTest::installFakeOutput()
{
    SongPlayer::setOutputFactoryForTesting([this](int channels, QString &) {
        ++m_factoryCalls;
        auto state = std::make_shared<FakeOutputState>();
        state->format = pcmFormat(songRate, channels);
        m_outputs.push_back(state);
        return std::make_unique<FakeSongOutput>(state);
    });
}

// ---------------------------------------------------------------------------
// Codec
// ---------------------------------------------------------------------------

void ProfileSongTest::encodeFitsTheBudgetFor45sStereoNoise()
{
    // Noise is the hardest thing to compress; 46 s at 44.1 kHz also proves
    // the cut to 45 s and the resampling to 48 kHz.
    const WavAudio pcm = noise(46.0, 44'100, 2, 0.5);
    auto result = encodeSong(SongClip{pcm, false, false});
    QVERIFY(result);
    const QByteArray bytes = result.value();
    QVERIFY2(bytes.size() <= maxSongBytes, qPrintable(u"%1 bytes"_s.arg(bytes.size())));

    const std::optional<SongContainer> song = decodeSongContainer(bytes);
    QVERIFY(song);
    QCOMPARE(song->channels, 2);
    QCOMPARE(song->frameSamples, 2880);
    QCOMPARE(song->totalSamples, qint64(45) * songRate);
    QCOMPARE(song->durationMs(), qint64(45'000));
    QCOMPARE(song->gainQ8, 0);
    QVERIFY(song->preSkip > 0 && song->preSkip <= SongContainer::maxPreSkip);
    QCOMPARE(decodeAll(bytes).size(), qsizetype(45) * songRate * 2);
}

void ProfileSongTest::bitrateStepsDownUntilItFits()
{
    const WavAudio stereo = noise(10.0, songRate, 2, 0.5);

    // With room to spare the first rung (40 kbit/s) wins.
    const std::optional<SongContainer> roomy = decodeSongContainer(encodeOrFail(stereo));
    QVERIFY(roomy);
    const double roomyRate = bitrateOf(*roomy);
    QVERIFY2(roomyRate > 34'000 && roomyRate < 46'000, qPrintable(QString::number(roomyRate)));

    // 35 000 bytes hold 10 s at 24 kbit/s but not at 32: the ladder steps
    // down past 40, 36 and 32.
    SongEncodeOptions tight;
    tight.maxBytes = 35'000;
    const QByteArray small = encodeOrFail(stereo, false, false, tight);
    QVERIFY(!small.isEmpty());
    QVERIFY(small.size() <= tight.maxBytes);
    const std::optional<SongContainer> stepped = decodeSongContainer(small);
    QVERIFY(stepped);
    const double steppedRate = bitrateOf(*stepped);
    QVERIFY2(steppedRate > 20'000 && steppedRate < 28'000, qPrintable(QString::number(steppedRate)));

    // Mono uses its own ladder (32, then 24).
    const std::optional<SongContainer> mono =
        decodeSongContainer(encodeOrFail(noise(10.0, songRate, 1, 0.5), false, false, tight));
    QVERIFY(mono);
    QCOMPARE(mono->channels, 1);
    const double monoRate = bitrateOf(*mono);
    QVERIFY2(monoRate > 20'000 && monoRate < 28'000, qPrintable(QString::number(monoRate)));

    // Below the last rung nothing fits.
    SongEncodeOptions impossible;
    impossible.maxBytes = 20'000;
    auto refused = encodeSong(SongClip{stereo, false, false}, impossible);
    QVERIFY(!refused);
    QCOMPARE(refused.error(), SongEncodeError::TooLarge);
}

void ProfileSongTest::encodeRejectsSilenceAndTooShort()
{
    const auto errorOf = [](const WavAudio &pcm) {
        auto result = encodeSong(SongClip{pcm, false, false});
        return result ? std::optional<SongEncodeError>() : std::optional<SongEncodeError>(result.error());
    };
    QCOMPARE(errorOf(synth(songRate, 1, 5.0, [](int, double) { return 0.0; })), SongEncodeError::Silent);
    // Hiss at -60 dBFS is below the -50 dBFS threshold: still silence.
    QCOMPARE(errorOf(noise(5.0, songRate, 2, dbToAmplitude(-60.0))), SongEncodeError::Silent);
    QCOMPARE(errorOf(tone(440.0, 0.5)), SongEncodeError::TooShort);
    QCOMPARE(errorOf(tone(440.0, 0.9, 44'100, 2)), SongEncodeError::TooShort);
    QCOMPARE(errorOf(WavAudio{}), SongEncodeError::TooShort);
    QCOMPARE(errorOf(tone(440.0, 1.2)), std::optional<SongEncodeError>());

    // A cancelled encode says so and produces nothing.
    auto cancelled = encodeSong(SongClip{tone(440.0, 3.0), false, false}, {}, [] { return true; });
    QVERIFY(!cancelled);
    QCOMPARE(cancelled.error(), SongEncodeError::Cancelled);
}

void ProfileSongTest::trimmedEdgesFadeOverHalfASecond()
{
    const QVector<qint16> out = decodeAll(encodeOrFail(tone(440.0, 4.0), true, true));
    QCOMPARE(out.size(), qsizetype(4) * songRate);
    const double steady = rmsMs(out, 1, 0, 1'500, 2'500);
    QVERIFY(steady > 0.05);
    const double endMs = 4'000;

    // A raised-cosine ramp over 500 ms: nearly nothing at first, half way at
    // 250 ms, nearly full at 450 ms, full after.
    QVERIFY(rmsMs(out, 1, 0, 0, 50) < 0.1 * steady);
    const double middle = rmsMs(out, 1, 0, 225, 275) / steady;
    QVERIFY2(middle > 0.35 && middle < 0.65, qPrintable(QString::number(middle)));
    QVERIFY(rmsMs(out, 1, 0, 420, 480) > 0.8 * steady);
    QVERIFY(rmsMs(out, 1, 0, 600, 700) > 0.9 * steady);

    QVERIFY(rmsMs(out, 1, 0, endMs - 50, endMs) < 0.1 * steady);
    const double ending = rmsMs(out, 1, 0, endMs - 275, endMs - 225) / steady;
    QVERIFY2(ending > 0.35 && ending < 0.65, qPrintable(QString::number(ending)));
    QVERIFY(rmsMs(out, 1, 0, endMs - 700, endMs - 600) > 0.9 * steady);
}

void ProfileSongTest::untrimmedEdgesUseShortFades()
{
    const QVector<qint16> out = decodeAll(encodeOrFail(tone(440.0, 4.0), false, false));
    const double steady = rmsMs(out, 1, 0, 1'500, 2'500);
    // 10 ms: still a ramp (no click at sample 0), then full level at once.
    QVERIFY(rmsMs(out, 1, 0, 0, 2) < 0.5 * steady);
    QVERIFY(rmsMs(out, 1, 0, 20, 70) > 0.9 * steady);
    QVERIFY(rmsMs(out, 1, 0, 225, 275) > 0.9 * steady);
    QVERIFY(rmsMs(out, 1, 0, 4'000 - 70, 4'000 - 20) > 0.9 * steady);
    QVERIFY(rmsMs(out, 1, 0, 4'000 - 2, 4'000) < 0.5 * steady);
}

void ProfileSongTest::loudnessIsNormalisedAndPeaksLimited()
{
    const double ceiling = dbToAmplitude(-1.0 + 0.5); // -1 dBFS plus the codec's overshoot
    const auto levelAfter = [](const WavAudio &pcm) {
        const QVector<qint16> out = decodeAll(encodeOrFail(pcm));
        return std::pair(amplitudeToDb(rmsMs(out, 1, 0, 500, 2'500)), peakOf(out));
    };
    // A sine's RMS is its amplitude - 3.01 dB.
    const auto sineAt = [](double rmsDb) {
        return tone(440.0, 3.0, songRate, 1, dbToAmplitude(rmsDb + 3.0103));
    };

    // -24 dBFS gains 8 dB and lands on the -16 dBFS target.
    auto [level, peak] = levelAfter(sineAt(-24.0));
    QVERIFY2(std::abs(level - -16.0) < 0.7, qPrintable(QString::number(level)));
    QVERIFY2(peak < ceiling, qPrintable(QString::number(peak)));

    // -30 dBFS would need 14 dB: the gain stops at +12.
    std::tie(level, peak) = levelAfter(sineAt(-30.0));
    QVERIFY2(std::abs(level - -18.0) < 0.7, qPrintable(QString::number(level)));

    // A loud song comes down to the same target.
    std::tie(level, peak) = levelAfter(sineAt(-3.5));
    QVERIFY2(std::abs(level - -16.0) < 0.7, qPrintable(QString::number(level)));
    QVERIFY(peak < ceiling);

    // A quiet song with loud 40 ms bursts: the loudness asks for a boost, but
    // the bursts already peak at -0.9 dBFS, so the peak ceiling wins and the
    // quiet part stays where it was instead of rising 4 dB or more. (The
    // bursts swell and fade, as music does; a hard-edged burst makes any
    // codec overshoot, which is what the -1 dBFS headroom is for.)
    const WavAudio spiky = synth(songRate, 1, 3.0, [](int, double t) {
        const double inBurst = std::fmod(t, 1.0) - 0.7;
        if (inBurst >= 0.0 && inBurst < 0.04)
            return 0.9 * std::pow(std::sin(std::numbers::pi * inBurst / 0.04), 2.0) * sine(1000.0, t);
        return dbToAmplitude(-30.0 + 3.0103) * sine(440.0, t);
    });
    const QVector<qint16> out = decodeAll(encodeOrFail(spiky));
    const double quiet = amplitudeToDb(rmsMs(out, 1, 0, 100, 600));
    QVERIFY2(quiet < -29.0 && quiet > -32.0, qPrintable(QString::number(quiet)));
    QVERIFY2(peakOf(out) < ceiling, qPrintable(QString::number(peakOf(out))));
}

void ProfileSongTest::decodeRoundTripKeepsPitch()
{
    // 44.1 kHz in: a resampling mistake would move the pitch (to 479 Hz).
    const QVector<qint16> mono = decodeAll(encodeOrFail(tone(440.0, 3.0, 44'100)));
    QCOMPARE(mono.size(), qsizetype(3) * songRate);
    QVERIFY(dominates(mono, 1, 0, 1.0, 1.0, songRate, 440.0, {415.3, 466.2, 479.0, 880.0}));

    const QVector<qint16> stereo = decodeAll(m_stereoSong);
    QCOMPARE(stereo.size(), qsizetype(2) * songRate * 2);
    QVERIFY(dominates(stereo, 2, 0, 0.5, 1.0, songRate, 440.0, {660.0, 415.3, 466.2}));
    QVERIFY(dominates(stereo, 2, 1, 0.5, 1.0, songRate, 660.0, {440.0, 622.3, 698.5}));
}

void ProfileSongTest::decoderHonoursPreSkipAndLength()
{
    // Half a second of silence, then three incommensurate tones: their sum
    // does not repeat within the search range, so the correlation peak is
    // where the decoded song really lines up with the source.
    const auto chord = [](int, double t) {
        return t < 0.5 ? 0.0 : 0.2 * (sine(220.0, t) + sine(347.0, t) + sine(513.0, t));
    };
    const WavAudio source = synth(songRate, 1, 1.734, chord);
    const QByteArray bytes = encodeOrFail(source);
    const std::optional<SongContainer> song = decodeSongContainer(bytes);
    QVERIFY(song);
    QCOMPARE(song->totalSamples, source.frameCount());
    QVERIFY(song->preSkip > 0);

    SongDecoder decoder(*song);
    QVERIFY(decoder.isValid());
    QCOMPARE(decoder.totalSamples(), source.frameCount());
    const QVector<qint16> out = decodeAll(decoder);
    QCOMPARE(out.size(), source.frameCount()); // the pre-skip and the padded tail are both cut
    QVERIFY(decoder.atEnd());
    QVERIFY(decoder.next().isEmpty());

    // Without the pre-skip the song would lag by it (312 samples).
    const qsizetype from = songRate;
    const qsizetype count = songRate / 2;
    int bestLag = 0;
    double best = -1.0;
    for (int lag = -400; lag <= 400; ++lag) {
        double sum = 0.0;
        for (qsizetype i = from; i < from + count; ++i)
            sum += double(out.at(i + lag)) * source.samples.at(i);
        if (sum > best) {
            best = sum;
            bestLag = lag;
        }
    }
    QVERIFY2(std::abs(bestLag) <= 2, qPrintable(QString::number(bestLag)));

    // A seek decodes the same audio as a straight run once the pre-roll has
    // settled the decoder, and stops at the same end.
    SongDecoder seeking(*song);
    seeking.seekToSample(songRate);
    QCOMPARE(seeking.position(), qint64(songRate));
    const QVector<qint16> tail = decodeAll(seeking);
    QCOMPARE(tail.size(), source.frameCount() - songRate);
    double difference = 0.0;
    double energy = 0.0;
    for (qsizetype i = 960; i < 4'800; ++i) {
        const double a = out.at(songRate + i);
        difference += (tail.at(i) - a) * (tail.at(i) - a);
        energy += a * a;
    }
    QVERIFY2(difference < 0.05 * energy, qPrintable(QString::number(difference / energy)));
    seeking.seekToSample(song->totalSamples);
    QVERIFY(seeking.atEnd());

    // gainQ8 attenuates: -6 dB is half the amplitude.
    SongContainer quieter = *song;
    quieter.gainQ8 = -6 * 256;
    SongDecoder attenuated(quieter);
    const QVector<qint16> soft = decodeAll(attenuated);
    const double ratio = rmsMs(soft, 1, 0, 800, 1'600) / rmsMs(out, 1, 0, 800, 1'600);
    QVERIFY2(std::abs(amplitudeToDb(ratio) - -6.0) < 0.1, qPrintable(QString::number(ratio)));
}

void ProfileSongTest::decoderRejectsWrongPacketDurations()
{
    const std::optional<SongContainer> valid = decodeSongContainer(m_monoSong);
    QVERIFY(valid);
    QVERIFY(SongDecoder(*valid).isValid());
    const qsizetype last = valid->packets.size() - 1;
    const QByteArray celt20ms = QByteArray::fromHex("f8");      // CELT 20 ms, one frame
    const QByteArray noFrames = QByteArray::fromHex("fb00");    // code 3 with a frame count of 0
    const QByteArray celt120ms = QByteArray::fromHex("fb06");   // code 3, six 20 ms frames

    SongContainer shortMiddle = *valid;
    shortMiddle.packets[5] = celt20ms;
    QVERIFY(!SongDecoder(shortMiddle).isValid());

    SongContainer unparseable = *valid;
    unparseable.packets[5] = noFrames;
    QVERIFY(!SongDecoder(unparseable).isValid());

    SongContainer longLast = *valid;
    longLast.packets[last] = celt120ms;
    QVERIFY(!SongDecoder(longLast).isValid());

    // A shorter last packet is how a song ends, as long as the packets still
    // cover every sample the header promises.
    SongContainer shortLast = *valid;
    shortLast.packets[last] = celt20ms;
    shortLast.totalSamples = qint64(last) * shortLast.frameSamples + 960 - shortLast.preSkip;
    SongDecoder endsShort(shortLast);
    QVERIFY(endsShort.isValid());
    QCOMPARE(decodeAll(endsShort).size(), shortLast.totalSamples);
    // One sample more than the packets hold: the container's packet count
    // still matches, but the song would stop short of its own length.
    SongContainer overpromised = shortLast;
    ++overpromised.totalSamples;
    QVERIFY(decodeSongContainer(encodeSongContainer(overpromised)));
    QVERIFY(!SongDecoder(overpromised).isValid());

    // Such a song passes the container's own checks, so it reaches the player,
    // which reports it as unplayable instead of playing garbage.
    const QByteArray hostile = encodeSongContainer(shortMiddle);
    QVERIFY(decodeSongContainer(hostile));
    SongLibrary::instance().put(keyA, hostile);
    SongPlayer player;
    player.setSongKey(keyA);
    QVERIFY(!player.valid());
    player.play();
    QVERIFY(!player.playing());
    QCOMPARE(m_factoryCalls, 0);
}

void ProfileSongTest::decoderConcealsACorruptPacket()
{
    std::optional<SongContainer> song = decodeSongContainer(m_monoSong);
    QVERIFY(song);
    // A code 3 packet whose TOC promises three 20 ms frames (60 ms, so the
    // duration check passes) but whose frame lengths run past its end.
    const QByteArray corrupt = QByteArray::fromHex("fb83fafa");
    {
        int error = OPUS_OK;
        OpusDecoder *raw = opus_decoder_create(songRate, 1, &error);
        QCOMPARE(error, OPUS_OK);
        std::vector<float> buffer(5760);
        // The premise: libopus itself refuses to decode it.
        QVERIFY(opus_decode_float(raw, reinterpret_cast<const unsigned char *>(corrupt.constData()),
                                  opus_int32(corrupt.size()), buffer.data(), 5760, 0)
                < 0);
        QCOMPARE(opus_packet_get_nb_samples(reinterpret_cast<const unsigned char *>(corrupt.constData()),
                                            opus_int32(corrupt.size()), songRate),
                 2880);
        opus_decoder_destroy(raw);
    }
    const QVector<qint16> clean = decodeAll(m_monoSong);
    const qsizetype index = 20; // decoder output [57 600, 60 480): song samples 57 600 - preSkip on
    song->packets[index] = corrupt;
    SongDecoder decoder(*song);
    QVERIFY(decoder.isValid());
    const QVector<qint16> out = decodeAll(decoder);
    QCOMPARE(out.size(), clean.size()); // the song keeps its length and runs to its end

    const qsizetype lost = qsizetype(index) * song->frameSamples - song->preSkip;
    const double steady = rms(clean, 1, 0, lost, song->frameSamples);
    const double concealed = rms(out, 1, 0, lost, song->frameSamples);
    // Concealment carries the tone on; silence (or an abort) would not.
    QVERIFY2(concealed > 0.2 * steady, qPrintable(u"%1 of %2"_s.arg(concealed).arg(steady)));
    // And the song recovers after it.
    const double later = rms(out, 1, 0, lost + 4 * song->frameSamples, song->frameSamples);
    QVERIFY(later > 0.8 * steady);
}

void ProfileSongTest::channelCountsAreKept()
{
    const auto channelsOf = [](const QByteArray &bytes) {
        const std::optional<SongContainer> song = decodeSongContainer(bytes);
        return song ? song->channels : 0;
    };
    QCOMPARE(channelsOf(encodeOrFail(tone(440.0, 1.5, 22'050, 1))), 1);
    QCOMPARE(channelsOf(encodeOrFail(tone(440.0, 1.5, songRate, 2))), 2);

    // Three channels: the first two stay left and right.
    const QByteArray three = encodeOrFail(synth(songRate, 3, 1.5, [](int channel, double t) {
        return channel == 0 ? 0.4 * sine(440.0, t) : channel == 1 ? 0.4 * sine(660.0, t) : 0.0;
    }));
    QCOMPARE(channelsOf(three), 2);
    const QVector<qint16> threeOut = decodeAll(three);
    QVERIFY(dominates(threeOut, 2, 0, 0.5, 0.8, songRate, 440.0, {660.0}));
    QVERIFY(dominates(threeOut, 2, 1, 0.5, 0.8, songRate, 660.0, {440.0}));

    // Six channels: a centre-only voice lands in both sides equally.
    const QByteArray six = encodeOrFail(synth(songRate, 6, 1.5, [](int channel, double t) {
        return channel == 2 ? 0.5 * sine(550.0, t) : 0.0;
    }));
    QCOMPARE(channelsOf(six), 2);
    const QVector<qint16> sixOut = decodeAll(six);
    const double left = rmsMs(sixOut, 2, 0, 500, 1'300);
    const double right = rmsMs(sixOut, 2, 1, 500, 1'300);
    QVERIFY(left > 0.01);
    QVERIFY2(std::abs(amplitudeToDb(left / right)) < 1.0, qPrintable(u"%1 %2"_s.arg(left).arg(right)));
}

void ProfileSongTest::chatOptionsKeepFiveMinutesWithinTheirLimits()
{
    // 5:10 in: a chat's limits cut at 5:00 (a profile song's at 0:45), and
    // five minutes of stereo fit a chat attachment's 4 MiB at the top rung.
    const QByteArray bytes = chatSong();
    QVERIFY(!bytes.isEmpty());
    QVERIFY2(bytes.size() <= AttachmentLimits::maxAudioBytes, qPrintable(QString::number(bytes.size())));
    QVERIFY(bytes.size() > maxSongBytes);

    // Every piece held to a profile song's limits refuses it; to a chat's,
    // takes it. The two never disagree about the same song.
    QVERIFY(!decodeSongContainer(bytes));
    const std::optional<SongContainer> song = decodeSongContainer(bytes, chatSongLimits());
    QVERIFY(song);
    QCOMPARE(song->channels, 2);
    QCOMPARE(song->totalSamples, qint64(300) * songRate);
    QCOMPARE(song->durationMs(), AttachmentLimits::maxAudioMs);
    QVERIFY(!SongDecoder(*song).isValid());
    QVERIFY(SongDecoder(*song, chatSongLimits()).isValid());
    const double rate = bitrateOf(*song);
    QVERIFY2(rate > 54'000 && rate < 74'000, qPrintable(QString::number(rate)));

    // The cut goes through the music, so it fades over half a second.
    SongDecoder decoder(*song, chatSongLimits());
    decoder.seekToSample(qint64(298) * songRate);
    QVector<qint16> tail;
    while (!decoder.atEnd())
        tail += decoder.next();
    QCOMPARE(tail.size(), qsizetype(2) * songRate * 2);
    const double steady = rmsMs(tail, 2, 0, 500, 1'000);
    QVERIFY(steady > 0.05);
    QVERIFY(rmsMs(tail, 2, 0, 2'000 - 50, 2'000) < 0.1 * steady);

    // Profile options are untouched by all this: the same source, 45 s.
    const std::optional<SongContainer> profile =
        decodeSongContainer(encodeOrFail(tone(440.0, 50.0, songRate, 2, 0.3)));
    QVERIFY(profile);
    QCOMPARE(profile->durationMs(), qint64(SongContainer::maxDurationMs));
}

void ProfileSongTest::chatOptionsTakeShortQuietClips()
{
    const SongEncodeOptions chat = chatSongEncodeOptions();
    const auto errorOf = [](const WavAudio &pcm, const SongEncodeOptions &options) {
        auto result = encodeSong(SongClip{pcm, false, false}, options);
        return result ? std::optional<SongEncodeError>() : std::optional<SongEncodeError>(result.error());
    };
    // 400 ms: too short for a profile song, a voice note in a chat; under
    // 300 ms is too short for both.
    QCOMPARE(errorOf(tone(440.0, 0.4), {}), SongEncodeError::TooShort);
    QCOMPARE(errorOf(tone(440.0, 0.4), chat), std::optional<SongEncodeError>());
    QCOMPARE(errorOf(tone(440.0, 0.25), chat), SongEncodeError::TooShort);
    // Hiss at -60 dBFS is silence to a profile song and sound to a chat;
    // only under -70 dBFS is a chat's clip silent.
    QCOMPARE(errorOf(noise(3.0, songRate, 1, dbToAmplitude(-60.0)), {}), SongEncodeError::Silent);
    QCOMPARE(errorOf(noise(3.0, songRate, 1, dbToAmplitude(-60.0)), chat), std::optional<SongEncodeError>());
    QCOMPARE(errorOf(noise(3.0, songRate, 1, dbToAmplitude(-80.0)), chat), SongEncodeError::Silent);

    // A quiet recording (RMS -30 dBFS) is lifted by at most 6 dB, where a
    // profile song gets 12.
    const WavAudio quiet = tone(440.0, 3.0, songRate, 1, dbToAmplitude(-30.0 + 3.0103));
    const QVector<qint16> lifted = decodeAll(encodeOrFail(quiet, false, false, chat));
    const double level = amplitudeToDb(rmsMs(lifted, 1, 0, 500, 2'500));
    QVERIFY2(std::abs(level - -24.0) < 0.7, qPrintable(QString::number(level)));
}

void ProfileSongTest::monoFallbackFitsWhatNoStereoRungCan()
{
    // 10 s of stereo noise in 25 000 bytes: 24 kbit/s stereo needs about
    // 30 000; the same song in one channel at 16 kbit/s fits.
    const WavAudio stereo = noise(10.0, songRate, 2, 0.5);
    SongEncodeOptions options;
    options.maxBytes = 25'000;
    options.stereoBitrates = {24'000};
    options.monoBitrates = {16'000};
    auto refused = encodeSong(SongClip{stereo, false, false}, options);
    QVERIFY(!refused);
    QCOMPARE(refused.error(), SongEncodeError::TooLarge);

    options.monoFallback = true;
    const QByteArray bytes = encodeOrFail(stereo, false, false, options);
    QVERIFY(!bytes.isEmpty());
    QVERIFY(bytes.size() <= options.maxBytes);
    const std::optional<SongContainer> song = decodeSongContainer(bytes);
    QVERIFY(song);
    QCOMPARE(song->channels, 1);
    QCOMPARE(song->totalSamples, qint64(10) * songRate);

    // Both sides end up in the one channel: 440 Hz left and 660 Hz right
    // come out together. (No stereo rung at all goes straight to mono.)
    SongEncodeOptions monoOnly = options;
    monoOnly.maxBytes = maxSongBytes;
    monoOnly.stereoBitrates.clear();
    monoOnly.monoBitrates = {32'000};
    const QVector<qint16> both = decodeAll(encodeOrFail(synth(songRate, 2, 3.0, [](int channel, double t) {
        return 0.3 * sine(channel == 0 ? 440.0 : 660.0, t);
    }), false, false, monoOnly));
    QCOMPARE(both.size(), qsizetype(3) * songRate);
    QVERIFY(dominates(both, 1, 0, 1.0, 1.0, songRate, 440.0, {415.3, 550.0}));
    QVERIFY(dominates(both, 1, 0, 1.0, 1.0, songRate, 660.0, {622.3, 550.0}));
}

void ProfileSongTest::encodeProgressNeverGoesBack()
{
    // The budget that steps down past three rungs (see above): each is given
    // up part way, and the figure waits for the next to pass it.
    SongEncodeOptions tight;
    tight.maxBytes = 35'000;
    QVector<qreal> steps;
    auto result = encodeSong(SongClip{noise(10.0, songRate, 2, 0.5), false, false}, tight, {},
                             [&steps](qreal done) { steps.push_back(done); });
    QVERIFY(result);
    QVERIFY2(steps.size() >= 50 && steps.size() <= 100, qPrintable(QString::number(steps.size())));
    for (qsizetype i = 1; i < steps.size(); ++i)
        QVERIFY2(steps.at(i) >= steps.at(i - 1) + 0.01 - 1e-9, qPrintable(QString::number(steps.at(i))));
    QVERIFY(steps.first() > 0.0);
    QVERIFY2(steps.last() > 0.99 && steps.last() <= 1.0, qPrintable(QString::number(steps.last())));
}

// ---------------------------------------------------------------------------
// Import
// ---------------------------------------------------------------------------

void ProfileSongTest::analyseReadsWavPeaksDurationAndInfoTags_data()
{
    QTest::addColumn<int>("bits");
    QTest::addColumn<bool>("isFloat");
    QTest::addColumn<int>("rate");
    QTest::addColumn<int>("channels");
    QTest::addColumn<double>("seconds");
    QTest::addColumn<QByteArray>("titleTag");
    QTest::addColumn<QByteArray>("artistTag");
    QTest::addColumn<bool>("listAfterData");
    QTest::addColumn<QString>("title");
    QTest::addColumn<QString>("artist");
    QTest::addColumn<QString>("label");
    QTest::addColumn<qint64>("windowStart");

    const QString dot = u" "_s + QChar(0x00B7) + u" "_s;
    QTest::newRow("8-bit mono 22.05 kHz, no tags")
        << 8 << false << 22'050 << 1 << 50.0 << QByteArray() << QByteArray() << false << u"song-8-bit"_s << QString()
        << (u"WAV"_s + dot + u"0:50"_s + dot + u"22.05 kHz mono"_s) << qint64(3'000);
    QTest::newRow("16-bit stereo 44.1 kHz, INFO before data")
        << 16 << false << 44'100 << 2 << 50.0 << QByteArray("Paper Planes") << QByteArray("M.I.A.") << false
        << u"Paper Planes"_s << u"M.I.A."_s << (u"WAV"_s + dot + u"0:50"_s + dot + u"44.1 kHz stereo"_s)
        << qint64(3'000);
    QTest::newRow("24-bit stereo 48 kHz, UTF-8 INFO after data")
        << 24 << false << 48'000 << 2 << 50.0 << QByteArray("Caf\xc3\xa9 del Mar") << QByteArray("Energy 52")
        << true << (u"Caf"_s + QChar(0xE9) + u" del Mar"_s) << u"Energy 52"_s
        << (u"WAV"_s + dot + u"0:50"_s + dot + u"48 kHz stereo"_s) << qint64(3'000);
    QTest::newRow("float mono 44.1 kHz, Latin-1 INFO")
        << 32 << true << 44'100 << 1 << 5.0 << QByteArray("Caf\xe9") << QByteArray() << false
        << (u"Caf"_s + QChar(0xE9)) << QString() << (u"WAV"_s + dot + u"0:05"_s + dot + u"44.1 kHz mono"_s)
        << qint64(0);
    QTest::newRow("16-bit mono, a long and ragged title")
        << 16 << false << 44'100 << 1 << 5.0 << QByteArray("  Too   long " + QByteArray(70, 'x'))
        << QByteArray("A\tB") << false << (u"Too long "_s + QString(51, u'x')) << u"A B"_s
        << (u"WAV"_s + dot + u"0:05"_s + dot + u"44.1 kHz mono"_s) << qint64(0);
}

void ProfileSongTest::analyseReadsWavPeaksDurationAndInfoTags()
{
    QFETCH(int, bits);
    QFETCH(bool, isFloat);
    QFETCH(int, rate);
    QFETCH(int, channels);
    QFETCH(double, seconds);
    QFETCH(QByteArray, titleTag);
    QFETCH(QByteArray, artistTag);
    QFETCH(bool, listAfterData);
    QFETCH(QString, title);
    QFETCH(QString, artist);
    QFETCH(QString, label);
    QFETCH(qint64, windowStart);

    // Silence for the first 6%, then a quarter of full scale, then three
    // quarters from half way: the waveform must show all three, in order.
    const WavAudio audio = synth(rate, channels, seconds, [seconds](int, double t) {
        const double at = t / seconds;
        if (at < 0.06)
            return 0.0;
        return (at < 0.5 ? 0.25 : 0.75) * sine(440.0, t);
    });
    WavSpec spec;
    spec.bits = bits;
    spec.isFloat = isFloat;
    spec.title = titleTag;
    spec.artist = artistTag;
    spec.listAfterData = listAfterData;
    spec.junkChunk = true;
    const QString name = titleTag.isEmpty() ? u"song-%1-bit.wav"_s.arg(bits) : u"tagged.wav"_s;
    const QString path = writeFile(name, makeWav(audio, spec));

    SongImporter importer;
    QSignalSpy analysed(&importer, &SongImporter::analysed);
    QSignalSpy failed(&importer, &SongImporter::failed);
    importer.analyse(path);
    QVERIFY(importer.busy());
    QVERIFY(analysed.wait(20'000));
    QCOMPARE(failed.count(), 0);
    QVERIFY(!importer.busy());

    const auto info = analysed.at(0).at(0).value<SongSourceInfo>();
    QCOMPARE(info.fileName, name);
    QCOMPARE(info.durationMs, qint64(std::llround(seconds * 1000)));
    QCOMPARE(info.sampleRate, rate);
    QCOMPARE(info.channels, channels);
    QCOMPARE(info.formatLabel, label);
    QCOMPARE(info.title, title);
    QCOMPARE(info.artist, artist);
    QVERIFY2(std::abs(info.defaultWindowStartMs - windowStart) <= 10,
             qPrintable(QString::number(info.defaultWindowStartMs)));

    // Buckets 7, 59 and 60 straddle a change (the 10 ms blocks do not line
    // up exactly with it at 22.05 kHz), so only the ones clear of it are held
    // to a level.
    QCOMPARE(info.peaks.size(), 120);
    for (int bucket = 0; bucket < 120; ++bucket) {
        const int peak = info.peaks.at(bucket);
        if (bucket <= 6)
            QVERIFY2(peak == 0, qPrintable(u"bucket %1: %2"_s.arg(bucket).arg(peak)));
        else if (bucket >= 8 && bucket <= 58)
            QVERIFY2(std::abs(peak - 64) <= 2, qPrintable(u"bucket %1: %2"_s.arg(bucket).arg(peak)));
        else if (bucket >= 61)
            QVERIFY2(std::abs(peak - 191) <= 2, qPrintable(u"bucket %1: %2"_s.arg(bucket).arg(peak)));
    }
}

void ProfileSongTest::encodeWindowSlicesTheChosenPart()
{
    // 300 Hz, then 600 Hz from 20 s, then 1200 Hz from 40 s.
    const auto sections = [](int, double t) {
        return 0.3 * sine(t < 20.0 ? 300.0 : t < 40.0 ? 600.0 : 1200.0, t);
    };
    const QString path = writeFile(u"sections.wav"_s, makeWav(synth(songRate, 1, 60.0, sections)));

    SongImporter importer;
    QSignalSpy encoded(&importer, &SongImporter::encoded);
    QSignalSpy failed(&importer, &SongImporter::failed);
    importer.encodeWindow(path, 10'000);
    QVERIFY(encoded.wait(30'000));
    QCOMPARE(failed.count(), 0);
    QCOMPARE(encoded.at(0).at(1).toLongLong(), qint64(45'000));
    QCOMPARE(encoded.at(0).at(2).toLongLong(), qint64(10'000));
    QVector<qint16> out = decodeAll(encoded.at(0).at(0).toByteArray());
    QCOMPARE(out.size(), qsizetype(45) * songRate);
    QVERIFY(dominates(out, 1, 0, 2.0, 1.0, songRate, 300.0, {600.0, 1200.0})); // source 12 s
    QVERIFY(dominates(out, 1, 0, 12.0, 1.0, songRate, 600.0, {300.0, 1200.0})); // source 22 s
    QVERIFY(dominates(out, 1, 0, 32.0, 1.0, songRate, 1200.0, {300.0, 600.0})); // source 42 s
    // Music on both sides of the window: both of its edges fade over 0.5 s.
    double steady = rmsMs(out, 1, 0, 2'000, 3'000);
    QVERIFY(rmsMs(out, 1, 0, 0, 50) < 0.1 * steady);
    QVERIFY(rmsMs(out, 1, 0, 45'000 - 50, 45'000) < 0.1 * steady);

    // A window past the end is pulled back so it fits: [15 s, 60 s), whose
    // end is the song's own, so only the start gets the long fade.
    encoded.clear();
    importer.encodeWindow(path, 30'000);
    QVERIFY(encoded.wait(30'000));
    QCOMPARE(encoded.at(0).at(2).toLongLong(), qint64(15'000));
    out = decodeAll(encoded.at(0).at(0).toByteArray());
    QVERIFY(dominates(out, 1, 0, 1.0, 1.0, songRate, 300.0, {600.0, 1200.0})); // source 16 s
    steady = rmsMs(out, 1, 0, 40'000, 41'000);
    QVERIFY(rmsMs(out, 1, 0, 0, 50) < 0.1 * steady);
    QVERIFY(rmsMs(out, 1, 0, 45'000 - 70, 45'000 - 20) > 0.9 * steady);

    // A song shorter than a window is the window: the start is 0, and with
    // only silence before its first note the start fades briefly too.
    const QString shortPath = writeFile(u"short.wav"_s, makeWav(synth(songRate, 1, 20.0, [](int, double t) {
        return t < 1.0 ? 0.0 : 0.3 * sine(440.0, t);
    })));
    encoded.clear();
    importer.encodeWindow(shortPath, 5'000);
    QVERIFY(encoded.wait(30'000));
    QCOMPARE(encoded.at(0).at(1).toLongLong(), qint64(20'000));
    QCOMPARE(encoded.at(0).at(2).toLongLong(), qint64(0));
    QCOMPARE(failed.count(), 0);
}

void ProfileSongTest::newerWindowCancelsTheOlderEncode()
{
    const QString path = writeFile(u"noise.wav"_s, makeWav(noise(60.0, songRate, 2, 0.3)));
    SongImporter importer;
    QSignalSpy encoded(&importer, &SongImporter::encoded);
    QSignalSpy failed(&importer, &SongImporter::failed);
    // Every encode waits on the worker until the test lets it go, so "under
    // way" and "done" are the test's to say, not the CPU's speed. (Bounded,
    // so a failing test cannot leave the worker, and the importer's
    // destructor, waiting for good.)
    QSemaphore started;
    QSemaphore go;
    importer.setEncodeHookForTesting([&started, &go] {
        started.release();
        (void)go.tryAcquire(1, 30'000);
    });
    // Everything the worker produced has been posted; deliver it.
    const auto drain = [&importer] {
        importer.waitForIdleForTesting();
        QCoreApplication::sendPostedEvents(&importer, QEvent::MetaCall);
    };

    importer.encodeWindow(path, 0);
    QVERIFY(importer.busy());
    QVERIFY(started.tryAcquire(1, 30'000)); // the first encode is under way
    importer.encodeWindow(path, 15'000);
    QVERIFY(importer.busy());
    go.release(); // the first one runs on, stale now
    // One worker: the second is under way only once the first has finished
    // and posted whatever it had.
    QVERIFY(started.tryAcquire(1, 30'000));
    go.release();
    drain();
    QCOMPARE(encoded.count(), 1); // only the newer window's
    QCOMPARE(encoded.at(0).at(2).toLongLong(), qint64(15'000));
    QCOMPARE(failed.count(), 0);
    QVERIFY(!importer.busy());

    // cancel() drops the running one and says nothing.
    encoded.clear();
    importer.encodeWindow(path, 5'000);
    QVERIFY(started.tryAcquire(1, 30'000));
    importer.cancel();
    QVERIFY(!importer.busy());
    go.release();
    drain();
    QCOMPARE(encoded.count(), 0);
    QCOMPARE(failed.count(), 0);

    // The same encode, not cancelled, does report: the silence above was
    // cancel()'s doing.
    importer.encodeWindow(path, 5'000);
    QVERIFY(started.tryAcquire(1, 30'000));
    go.release();
    drain();
    QCOMPARE(encoded.count(), 1);
    QCOMPARE(encoded.at(0).at(2).toLongLong(), qint64(5'000));
}

void ProfileSongTest::workerThatThrowsFailsInsteadOfAborting()
{
    // An exception leaving a pool thread is std::terminate. Whatever a job
    // throws (out of memory on a hostile file, say) must become a failure.
    const QString path = writeFile(u"thrown.wav"_s, makeWav(tone(440.0, 5.0)));
    SongImporter importer;
    QSignalSpy failed(&importer, &SongImporter::failed);
    QSignalSpy encoded(&importer, &SongImporter::encoded);
    importer.setEncodeHookForTesting([] { throw std::bad_alloc(); });
    importer.encodeWindow(path, 0);
    QVERIFY(failed.wait(30'000));
    QCOMPARE(failed.at(0).at(0).value<SongImportError>(), SongImportError::EncodeFailed);
    QCOMPARE(encoded.count(), 0);
    QVERIFY(!importer.busy());

    // And the importer works on.
    importer.setEncodeHookForTesting({});
    importer.encodeWindow(path, 0);
    QVERIFY(encoded.wait(30'000));
    QCOMPARE(failed.count(), 1);
}

void ProfileSongTest::importRefusesUnreadableAndOversizedFiles()
{
    const auto expectFailure = [](SongImporter &importer, const std::function<void()> &start,
                                  SongImportError expected) {
        QSignalSpy failed(&importer, &SongImporter::failed);
        QSignalSpy analysed(&importer, &SongImporter::analysed);
        QSignalSpy encoded(&importer, &SongImporter::encoded);
        start();
        // Never inside the call itself: the caller sets its own state first.
        QCOMPARE(failed.count(), 0);
        QVERIFY(failed.wait(20'000));
        QCOMPARE(failed.at(0).at(0).value<SongImportError>(), expected);
        QCOMPARE(failed.at(0).at(1).toString(), songImportErrorText(expected));
        QVERIFY(!songImportErrorText(expected).isEmpty());
        QCOMPARE(analysed.count(), 0);
        QCOMPARE(encoded.count(), 0);
        QVERIFY(!importer.busy());
    };

    SongImporter importer;
    const QString missing = m_dir.filePath(u"missing.wav"_s);
    expectFailure(importer, [&] { importer.analyse(missing); }, SongImportError::FileMissing);
    expectFailure(importer, [&] { importer.encodeWindow(missing, 0); }, SongImportError::FileMissing);
    expectFailure(importer, [&] { importer.analyse(m_dir.path()); }, SongImportError::FileMissing);

    const QString good = writeFile(u"good.wav"_s, makeWav(tone(440.0, 5.0)));
    SongImportLimits tiny;
    tiny.maxFileBytes = 1'024;
    SongImporter strict(tiny);
    expectFailure(strict, [&] { strict.analyse(good); }, SongImportError::FileTooLarge);
    expectFailure(strict, [&] { strict.encodeWindow(good, 0); }, SongImportError::FileTooLarge);

    // RIFF/WAVE with a format chunk but no data chunk.
    QByteArray noData = makeWav(tone(440.0, 1.0));
    noData.truncate(noData.indexOf("data"));
    expectFailure(importer, [&] { importer.analyse(writeFile(u"nodata.wav"_s, noData)); },
                  SongImportError::DecodeFailed);
    // IMA ADPCM: a WAV, but an encoding the reader does not decode.
    WavSpec adpcm;
    adpcm.formatTag = 0x0011;
    const QString adpcmPath = writeFile(u"adpcm.wav"_s, makeWav(tone(440.0, 1.0), adpcm));
    expectFailure(importer, [&] { importer.analyse(adpcmPath); }, SongImportError::UnsupportedFormat);
    // A sample rate no real file has: 2^31 Hz turned negative as an int and
    // aborted the process from the worker; 768 kHz + 1 is just past the bound.
    for (const quint32 rate : {0x8000'0000U, 0xFFFF'FFFFU, quint32(WavFile::maxSampleRate) + 1}) {
        QByteArray absurd = makeWav(tone(440.0, 1.0));
        qToLittleEndian(rate, absurd.data() + 24); // the fmt chunk's rate field
        const QString absurdPath = writeFile(u"absurd-rate.wav"_s, absurd);
        expectFailure(importer, [&] { importer.analyse(absurdPath); }, SongImportError::UnsupportedFormat);
        expectFailure(importer, [&] { importer.encodeWindow(absurdPath, 0); }, SongImportError::UnsupportedFormat);
    }

    // Readable files that can never become a song say so at once.
    expectFailure(importer, [&] { importer.analyse(writeFile(u"blip.wav"_s, makeWav(tone(440.0, 0.5)))); },
                  SongImportError::TooShort);
    const QString silent = writeFile(u"silent.wav"_s, makeWav(synth(songRate, 2, 5.0, [](int, double) {
        return 0.0;
    })));
    expectFailure(importer, [&] { importer.analyse(silent); }, SongImportError::Silent);
    expectFailure(importer, [&] { importer.encodeWindow(silent, 0); }, SongImportError::Silent);

    // Not a WAV: without a Multimedia backend WAV is all there is; with one,
    // the decoder refuses the junk.
    QByteArray junk(64 * 1024, '\0');
    std::mt19937 generator(3);
    for (char &byte : junk)
        byte = char(generator());
    const QString junkPath = writeFile(u"junk.mp3"_s, junk);
    if (!SongImporter::canDecodeCompressed()) {
        expectFailure(importer, [&] { importer.analyse(junkPath); }, SongImportError::DecoderUnavailable);
    } else {
        // How the backend words its refusal varies; a hung backend is cut off
        // by the pass's own timeout. What must hold: a failure, never a song.
        SongImportLimits limits;
        limits.timeoutMs = 10'000;
        SongImporter decoding(limits);
        QSignalSpy failed(&decoding, &SongImporter::failed);
        QSignalSpy analysed(&decoding, &SongImporter::analysed);
        decoding.analyse(junkPath);
        QCOMPARE(failed.count(), 0);
        QVERIFY(failed.wait(20'000));
        const auto error = failed.at(0).at(0).value<SongImportError>();
        QVERIFY2(error == SongImportError::UnsupportedFormat || error == SongImportError::DecodeFailed
                     || error == SongImportError::TimedOut,
                 qPrintable(QString::number(int(error))));
        QCOMPARE(analysed.count(), 0);
        QVERIFY(!decoding.busy());
    }
}

void ProfileSongTest::importCompressedWhenDecoderAvailable()
{
    if (!SongImporter::canDecodeCompressed())
        QSKIP("No Qt Multimedia decoder backend on this machine");

    // The WAV reader is bypassed, so this is QAudioDecoder's path end to end.
    const QString path = writeFile(u"through-decoder.wav"_s, makeWav(synth(44'100, 1, 50.0, [](int, double t) {
        return t < 2.0 ? 0.0 : 0.4 * sine(440.0, t);
    })));
    SongImporter importer;
    importer.setForceDecoderForTesting(true);
    QSignalSpy analysed(&importer, &SongImporter::analysed);
    QSignalSpy encoded(&importer, &SongImporter::encoded);
    QSignalSpy failed(&importer, &SongImporter::failed);
    importer.analyse(path);
    QVERIFY(analysed.wait(30'000));
    QCOMPARE(failed.count(), 0);
    const auto info = analysed.at(0).at(0).value<SongSourceInfo>();
    QVERIFY2(std::abs(info.durationMs - 50'000) <= 50, qPrintable(QString::number(info.durationMs)));
    QCOMPARE(info.sampleRate, 44'100);
    QCOMPARE(info.channels, 1);
    QVERIFY2(info.formatLabel.startsWith(u"WAV "_s + QChar(0x00B7) + u" 0:50 "_s + QChar(0x00B7) + u" 44.1 kHz mono"_s),
             qPrintable(info.formatLabel));
    QVERIFY(!info.title.isEmpty());
    QVERIFY2(std::abs(info.defaultWindowStartMs - 2'000) <= 30, qPrintable(QString::number(info.defaultWindowStartMs)));
    QCOMPARE(info.peaks.size(), 120);
    QCOMPARE(info.peaks.at(0), quint8(0));
    QVERIFY(std::abs(int(info.peaks.at(60)) - 102) <= 3); // 0.4 of full scale

    importer.encodeWindow(path, 3'000);
    QVERIFY(encoded.wait(30'000));
    QCOMPARE(failed.count(), 0);
    QCOMPARE(encoded.at(0).at(1).toLongLong(), qint64(45'000));
    QCOMPARE(encoded.at(0).at(2).toLongLong(), qint64(3'000));
    const QByteArray bytes = encoded.at(0).at(0).toByteArray();
    const std::optional<SongContainer> song = decodeSongContainer(bytes);
    QVERIFY(song);
    QCOMPARE(song->channels, 1); // asked for stereo, folded back to the file's mono
    const QVector<qint16> window = decodeAll(bytes);
    QVERIFY(dominates(window, 1, 0, 5.0, 1.0, songRate, 440.0, {415.3, 466.2, 479.0}));
    // The tone plays on both sides of [3 s, 48 s): both edges get the long fade.
    const double steady = rmsMs(window, 1, 0, 5'000, 6'000);
    QVERIFY(rmsMs(window, 1, 0, 0, 50) < 0.1 * steady);
    QVERIFY(rmsMs(window, 1, 0, 225, 275) > 0.3 * steady);
    QVERIFY(rmsMs(window, 1, 0, 45'000 - 50, 45'000) < 0.1 * steady);

    // Without an analysis first the length is unknown; a window that runs
    // past the end is noticed at the end and decoded again where it fits.
    SongImporter fresh;
    fresh.setForceDecoderForTesting(true);
    QSignalSpy freshEncoded(&fresh, &SongImporter::encoded);
    fresh.encodeWindow(path, 30'000);
    QVERIFY(freshEncoded.wait(30'000));
    QCOMPARE(freshEncoded.at(0).at(1).toLongLong(), qint64(45'000));
    QVERIFY2(std::abs(freshEncoded.at(0).at(2).toLongLong() - 5'000) <= 50,
             qPrintable(QString::number(freshEncoded.at(0).at(2).toLongLong())));

    // A real stereo file stays stereo.
    const QString stereoPath = writeFile(u"stereo-through-decoder.wav"_s,
                                         makeWav(synth(songRate, 2, 6.0, [](int channel, double t) {
                                             return 0.3 * sine(channel == 0 ? 440.0 : 660.0, t);
                                         })));
    encoded.clear();
    importer.encodeWindow(stereoPath, 0);
    QVERIFY(encoded.wait(30'000));
    QCOMPARE(encoded.at(0).at(1).toLongLong(), qint64(6'000));
    const std::optional<SongContainer> stereo = decodeSongContainer(encoded.at(0).at(0).toByteArray());
    QVERIFY(stereo);
    QCOMPARE(stereo->channels, 2);
}

void ProfileSongTest::analyseKeepsOnlyTheSourceLimit()
{
    // Only the first maxSourceMs can be picked from, so nothing after it is
    // measured, drawn or offered as a window.
    const QString path = writeFile(u"long.wav"_s, makeWav(tone(440.0, 30.0, 22'050)));
    SongImportLimits limits;
    limits.maxSourceMs = 20'000;
    SongImporter importer(limits);
    QSignalSpy analysed(&importer, &SongImporter::analysed);
    QSignalSpy encoded(&importer, &SongImporter::encoded);
    importer.analyse(path);
    QVERIFY(analysed.wait(20'000));
    const auto info = analysed.at(0).at(0).value<SongSourceInfo>();
    QCOMPARE(info.durationMs, qint64(20'000));
    QCOMPARE(info.peaks.size(), qsizetype(120));
    QVERIFY(info.formatLabel.contains(u"0:20"_s));
    importer.encodeWindow(path, 10'000);
    QVERIFY(encoded.wait(30'000));
    QCOMPARE(encoded.at(0).at(1).toLongLong(), qint64(20'000));
    QCOMPARE(encoded.at(0).at(2).toLongLong(), qint64(0));
}

void ProfileSongTest::encodeWholeKeepsTheFirstFiveMinutesOfAWav()
{
    // 5:20 at 8 kHz: 440 Hz, then 880 Hz from 4:30.
    const QString path = writeFile(u"long-note.wav"_s, makeWav(synth(8'000, 1, 320.0, [](int, double t) {
        return 0.3 * sine(t < 270.0 ? 440.0 : 880.0, t);
    })));
    SongImporter importer;
    QSignalSpy whole(&importer, &SongImporter::encodedWhole);
    QSignalSpy failed(&importer, &SongImporter::failed);
    QVector<qreal> progress;
    connect(&importer, &SongImporter::progressChanged, this, [&progress](qreal done) { progress.push_back(done); });
    importer.encodeWhole(path, chatSongEncodeOptions(), AttachmentLimits::maxPeaks);
    QVERIFY(importer.busy());
    QVERIFY(whole.wait(120'000));
    QCOMPARE(failed.count(), 0);
    QVERIFY(!importer.busy());

    const auto song = whole.at(0).at(0).value<EncodedSong>();
    QVERIFY(song.trimmed);
    QCOMPARE(song.durationMs, AttachmentLimits::maxAudioMs);
    QVERIFY(song.container.size() <= AttachmentLimits::maxAudioBytes);
    const std::optional<SongContainer> container = decodeSongContainer(song.container, chatSongLimits());
    QVERIFY(container);
    QCOMPARE(container->channels, 1);
    QCOMPARE(container->totalSamples, qint64(300) * songRate);
    // Four and a half minutes in, the note has changed.
    SongDecoder decoder(*container, chatSongLimits());
    decoder.seekToSample(qint64(280) * songRate);
    QVector<qint16> later;
    while (later.size() < songRate && !decoder.atEnd())
        later += decoder.next();
    QVERIFY(dominates(later, 1, 0, 0.0, 1.0, songRate, 880.0, {440.0, 830.6, 932.3}));

    // An even tone draws as even bars, lifted to the top.
    QCOMPARE(song.peaks.size(), AttachmentLimits::maxPeaks);
    for (const quint8 peak : song.peaks)
        QVERIFY2(peak >= 250, qPrintable(QString::number(peak)));

    // From 0 to 1, never back, and moving during the encode, not only at
    // its end.
    QVERIFY(progress.size() >= 10);
    QCOMPARE(progress.first(), 0.0);
    QCOMPARE(progress.last(), 1.0);
    for (qsizetype i = 1; i < progress.size(); ++i)
        QVERIFY(progress.at(i) >= progress.at(i - 1));
    QVERIFY(std::count_if(progress.cbegin(), progress.cend(), [](qreal done) { return done > 0.2 && done < 0.9; })
            >= 5);

    // A file shorter than the limit is kept whole, and says it was not cut.
    const QString shortPath = writeFile(u"short-note.wav"_s, makeWav(tone(440.0, 20.0, 22'050, 2, 0.3)));
    importer.encodeWhole(shortPath, chatSongEncodeOptions(), AttachmentLimits::maxPeaks);
    QVERIFY(whole.wait(60'000));
    const auto shortSong = whole.at(1).at(0).value<EncodedSong>();
    QVERIFY(!shortSong.trimmed);
    QCOMPARE(shortSong.durationMs, qint64(20'000));
    QCOMPARE(decodeSongContainer(shortSong.container, chatSongLimits())->channels, 2);
}

void ProfileSongTest::encodeWholeTakesAShortQuietVoiceNote()
{
    // 300 ms of voice at -20 dBFS, then 300 ms of room: too short for a
    // profile song, a voice note in a chat.
    const QString path = writeFile(u"note.wav"_s, makeWav(synth(16'000, 1, 0.6, [](int, double t) {
        return t < 0.3 ? dbToAmplitude(-20.0) * sine(300.0, t) : 0.0;
    })));
    SongImporter importer;
    QSignalSpy whole(&importer, &SongImporter::encodedWhole);
    QSignalSpy failed(&importer, &SongImporter::failed);
    importer.encodeWhole(path, chatSongEncodeOptions(), AttachmentLimits::maxPeaks);
    QVERIFY(whole.wait(30'000));
    QCOMPARE(failed.count(), 0);
    const auto song = whole.at(0).at(0).value<EncodedSong>();
    QVERIFY(!song.trimmed);
    QCOMPARE(song.durationMs, qint64(600));
    // The voice reaches the top of the waveform; the room stays flat.
    QCOMPARE(song.peaks.size(), AttachmentLimits::maxPeaks);
    QVERIFY(song.peaks.at(10) >= 250);
    QCOMPARE(song.peaks.last(), quint8(0));

    // At -40 dBFS the waveform is lifted by at most 24 dB, so hiss can never
    // pass for sound: its bars stop at about a sixth of the height.
    const QString faint = writeFile(u"faint.wav"_s, makeWav(synth(16'000, 1, 1.0, [](int, double t) {
        return dbToAmplitude(-40.0) * sine(300.0, t);
    })));
    importer.encodeWhole(faint, chatSongEncodeOptions(), AttachmentLimits::maxPeaks);
    QVERIFY(whole.wait(30'000));
    const auto faintSong = whole.at(1).at(0).value<EncodedSong>();
    const int expected = int(std::lround(dbToAmplitude(-40.0) * 16.0 * 255.0));
    QVERIFY2(std::abs(int(faintSong.peaks.at(48)) - expected) <= 2,
             qPrintable(QString::number(faintSong.peaks.at(48))));
}

void ProfileSongTest::encodeWholeFindsTheSamplesBehindOtherChunks()
{
    // 300 KB of other chunks between the format and the samples (cover art
    // does this): the head is found by walking the chunks, not by reading.
    QByteArray bytes = makeWav(tone(440.0, 12.0, 22'050, 1, 0.3));
    QByteArray padding;
    appendChunk(padding, "junk", QByteArray(300'000, 'j'));
    bytes.insert(bytes.indexOf("data"), padding);
    const QString path = writeFile(u"padded.wav"_s, bytes);
    SongEncodeOptions options = chatSongEncodeOptions();
    options.maxDurationMs = 10'000;
    SongImporter importer;
    QSignalSpy whole(&importer, &SongImporter::encodedWhole);
    QSignalSpy failed(&importer, &SongImporter::failed);
    importer.encodeWhole(path, options, 24);
    QVERIFY(whole.wait(30'000));
    QCOMPARE(failed.count(), 0);
    const auto song = whole.at(0).at(0).value<EncodedSong>();
    QVERIFY(song.trimmed);
    QCOMPARE(song.durationMs, qint64(10'000));
    QCOMPARE(song.peaks.size(), 24);
    const QVector<qint16> out = decodeAll(song.container);
    QVERIFY(dominates(out, 1, 0, 2.0, 1.0, songRate, 440.0, {415.3, 466.2}));
}

void ProfileSongTest::encodeWholeRefusesSilence()
{
    SongImporter importer;
    QSignalSpy failed(&importer, &SongImporter::failed);
    QSignalSpy whole(&importer, &SongImporter::encodedWhole);
    const QString silent = writeFile(u"whole-silent.wav"_s, makeWav(synth(songRate, 2, 3.0, [](int, double) {
        return 0.0;
    })));
    importer.encodeWhole(silent, chatSongEncodeOptions(), AttachmentLimits::maxPeaks);
    QCOMPARE(failed.count(), 0); // never inside the call
    QVERIFY(failed.wait(30'000));
    QCOMPARE(failed.at(0).at(0).value<SongImportError>(), SongImportError::Silent);
    QCOMPARE(whole.count(), 0);
    QVERIFY(!importer.busy());

    importer.encodeWhole(m_dir.filePath(u"not-there.wav"_s), chatSongEncodeOptions(), AttachmentLimits::maxPeaks);
    QVERIFY(failed.wait(30'000));
    QCOMPARE(failed.at(1).at(0).value<SongImportError>(), SongImportError::FileMissing);
}

void ProfileSongTest::encodeWholeStreamsAWavTooDenseToReadWhole()
{
    // A recording whose first five minutes are more than the importer reads
    // into memory (32-bit float at 96 kHz runs past it before then) goes
    // through the decoder rather than being refused as too large.
    const QString path = writeFile(u"dense.wav"_s, makeWav(tone(440.0, 10.0, 44'100, 1, 0.3)));
    SongImportLimits limits;
    limits.maxWavReadBytes = 200'000; // stands in for 192 MiB: this file is about 880 KB
    SongImporter importer(limits);
    QSignalSpy whole(&importer, &SongImporter::encodedWhole);
    QSignalSpy failed(&importer, &SongImporter::failed);
    importer.encodeWhole(path, chatSongEncodeOptions(), AttachmentLimits::maxPeaks);
    if (!SongImporter::canDecodeCompressed()) {
        // With nothing to stream it, it is too large as before.
        QVERIFY(failed.wait(30'000));
        QCOMPARE(failed.at(0).at(0).value<SongImportError>(), SongImportError::FileTooLarge);
        return;
    }
    QVERIFY(whole.wait(60'000));
    QCOMPARE(failed.count(), 0);
    const auto song = whole.at(0).at(0).value<EncodedSong>();
    QVERIFY(!song.trimmed);
    QVERIFY2(std::abs(song.durationMs - 10'000) <= 30, qPrintable(QString::number(song.durationMs)));
}

void ProfileSongTest::encodeWholeThroughTheDecoder()
{
    if (!SongImporter::canDecodeCompressed())
        QSKIP("No Qt Multimedia decoder backend on this machine");

    // The decoder's path: 50 s in, at most 30 s kept.
    const QString path = writeFile(u"whole-through-decoder.wav"_s,
                                   makeWav(tone(440.0, 50.0, 44'100, 1, 0.3)));
    SongEncodeOptions options = chatSongEncodeOptions();
    options.maxDurationMs = 30'000;
    SongImporter importer;
    importer.setForceDecoderForTesting(true);
    QSignalSpy whole(&importer, &SongImporter::encodedWhole);
    QSignalSpy failed(&importer, &SongImporter::failed);
    QVector<qreal> progress;
    connect(&importer, &SongImporter::progressChanged, this, [&progress](qreal done) { progress.push_back(done); });
    importer.encodeWhole(path, options, AttachmentLimits::maxPeaks);
    QVERIFY(whole.wait(60'000));
    QCOMPARE(failed.count(), 0);
    const auto song = whole.at(0).at(0).value<EncodedSong>();
    QVERIFY(song.trimmed);
    QCOMPARE(song.durationMs, qint64(30'000));
    const std::optional<SongContainer> container = decodeSongContainer(song.container, chatSongLimits());
    QVERIFY(container);
    QCOMPARE(container->channels, 1); // asked for stereo, folded back to the file's mono
    QCOMPARE(song.peaks.size(), AttachmentLimits::maxPeaks);
    for (qsizetype i = 1; i < progress.size(); ++i)
        QVERIFY(progress.at(i) >= progress.at(i - 1));
    QCOMPARE(progress.last(), 1.0);

    // A file that fits is not cut.
    const QString shortPath = writeFile(u"whole-short-through-decoder.wav"_s,
                                        makeWav(tone(440.0, 8.0, 44'100, 2, 0.3)));
    importer.encodeWhole(shortPath, options, AttachmentLimits::maxPeaks);
    QVERIFY(whole.wait(60'000));
    const auto shortSong = whole.at(1).at(0).value<EncodedSong>();
    QVERIFY(!shortSong.trimmed);
    QVERIFY2(std::abs(shortSong.durationMs - 8'000) <= 30, qPrintable(QString::number(shortSong.durationMs)));
}

// ---------------------------------------------------------------------------
// Playback
// ---------------------------------------------------------------------------

void ProfileSongTest::songStreamProducesDecodedPcmAndEnds()
{
    const std::optional<SongContainer> song = decodeSongContainer(m_monoSong);
    QVERIFY(song);
    const QVector<qint16> decoded = decodeAll(m_monoSong);
    const qint64 total = song->totalSamples;

    // The sink's own rate and layout: every frame, at the playback gain.
    {
        SongStream stream(*song, pcmFormat(songRate, 1));
        QVERIFY(stream.isValid());
        QVERIFY(stream.isOpen() && stream.isSequential());
        QCOMPARE(stream.framesPlayed(), 0);
        const QVector<qint16> out = asS16(readAll(stream));
        QCOMPARE(out.size(), total);
        QVERIFY(stream.finished());
        QVERIFY(!stream.fadedOut());
        QCOMPARE(stream.framesPlayed(), total);
        QCOMPARE(stream.bytesAvailable(), 0);
        QVERIFY(stream.read(4096).isEmpty());
        const double ratio = rmsMs(out, 1, 0, 500, 2'500) / rmsMs(decoded, 1, 0, 500, 2'500);
        QVERIFY2(std::abs(ratio - specPlaybackGain) < 0.02, qPrintable(QString::number(ratio)));
        QVERIFY(dominates(out, 1, 0, 1.0, 1.0, songRate, 440.0, {415.3, 466.2}));
    }
    // 44.1 kHz stereo: resampled, the mono song on both sides, same pitch.
    {
        SongStream stream(*song, pcmFormat(44'100, 2));
        const QVector<qint16> out = asS16(readAll(stream));
        const qsizetype frames = out.size() / 2;
        QVERIFY2(std::abs(frames - qsizetype(total * 44'100 / songRate)) <= 2, qPrintable(QString::number(frames)));
        for (qsizetype frame = 0; frame < frames; ++frame)
            QCOMPARE(out.at(frame * 2), out.at(frame * 2 + 1));
        QVERIFY(dominates(out, 2, 0, 1.0, 1.0, 44'100, 440.0, {415.3, 466.2, 405.0}));
        QCOMPARE(stream.framesPlayed(), total);
    }
    // A float sink gets floats in [-1, 1] at the same level.
    {
        SongStream stream(*song, pcmFormat(songRate, 2, QAudioFormat::Float));
        const QByteArray bytes = readAll(stream);
        QCOMPARE(bytes.size(), qsizetype(total) * 2 * qsizetype(sizeof(float)));
        std::vector<float> values(std::size_t(bytes.size()) / sizeof(float));
        std::memcpy(values.data(), bytes.constData(), bytes.size());
        double sum = 0.0;
        for (std::size_t i = std::size_t(songRate) * 2; i < std::size_t(songRate) * 4; i += 2) {
            QVERIFY(std::abs(values[i]) <= 1.0F);
            sum += double(values[i]) * values[i];
        }
        const double level = std::sqrt(sum / songRate);
        const double expected = rmsMs(decoded, 1, 0, 1'000, 2'000) * specPlaybackGain;
        QVERIFY2(std::abs(level / expected - 1.0) < 0.03, qPrintable(QString::number(level / expected)));
    }
    // Starting part way plays the rest.
    {
        SongStream stream(*song, pcmFormat(songRate, 1), songRate);
        QCOMPARE(stream.framesPlayed(), qint64(songRate));
        QCOMPARE(asS16(readAll(stream)).size(), total - songRate);
    }
    // A fade-out ramps down over its length and then ends the stream early.
    {
        SongStream stream(*song, pcmFormat(songRate, 1));
        QCOMPARE(asS16(stream.read(songRate * 2)).size(), songRate); // 1 s
        stream.startFadeOut(100);
        const QVector<qint16> fade = asS16(readAll(stream));
        QCOMPARE(fade.size(), qsizetype(songRate / 10));
        QVERIFY(stream.finished());
        QVERIFY(stream.fadedOut());
        QVERIFY(rmsMs(fade, 1, 0, 90, 100) < 0.25 * rmsMs(fade, 1, 0, 0, 10));
    }
}

void ProfileSongTest::playbackGuardTamesAFullScaleSquareWave()
{
    // A hostile song: a 0 dBFS square wave, encoded without normalisation.
    SongEncodeOptions hostile;
    hostile.targetLoudnessDb = 0.0;
    hostile.peakCeilingDb = 0.0;
    const WavAudio square = synth(songRate, 1, 4.0, [](int, double t) { return sine(200.0, t) >= 0 ? 1.0 : -1.0; });
    const QByteArray bytes = encodeOrFail(square, false, false, hostile);
    const std::optional<SongContainer> song = decodeSongContainer(bytes);
    QVERIFY(song);
    const QVector<qint16> decoded = decodeAll(bytes);
    QVERIFY2(amplitudeToDb(rmsMs(decoded, 1, 0, 1'500, 3'500)) > -2.0, "the song itself must be full scale");

    SongStream stream(*song, pcmFormat(songRate, 1));
    const QVector<qint16> out = asS16(readAll(stream));
    const double steadyDb = amplitudeToDb(rmsMs(out, 1, 0, 1'500, 3'500));
    // -14 dBFS short-term ceiling, then the 0.56 playback gain (-5 dB):
    // about -19 dBFS instead of -5.
    const double expectedDb = specRmsCeilingDb + amplitudeToDb(specPlaybackGain);
    QVERIFY2(steadyDb < expectedDb + 0.5 && steadyDb > expectedDb - 2.0, qPrintable(QString::number(steadyDb)));
    const double peakLimit = dbToAmplitude(specPeakCeilingDb) * specPlaybackGain;
    QVERIFY2(peakOf(out) <= peakLimit + 2.0 / 32768.0, qPrintable(QString::number(peakOf(out))));

    // An ordinary song (-16 dBFS) passes the guard untouched.
    const std::optional<SongContainer> normal = decodeSongContainer(m_monoSong);
    SongStream plain(*normal, pcmFormat(songRate, 1));
    const QVector<qint16> plainOut = asS16(readAll(plain));
    const QVector<qint16> plainDecoded = decodeAll(m_monoSong);
    const double change = amplitudeToDb(rmsMs(plainOut, 1, 0, 500, 2'500) / rmsMs(plainDecoded, 1, 0, 500, 2'500))
        - amplitudeToDb(specPlaybackGain);
    QVERIFY2(std::abs(change) < 0.3, qPrintable(QString::number(change)));
}

void ProfileSongTest::setSongKeyWithTheSameKeyIsANoOp()
{
    SongLibrary::instance().put(keyA, m_monoSong);
    SongLibrary::instance().put(keyB, m_stereoSong);
    SongPlayer player;
    QSignalSpy sources(&player, &SongPlayer::sourceChanged);
    player.setSongKey(keyA);
    QCOMPARE(sources.count(), 1);
    QVERIFY(player.valid());
    QCOMPARE(player.durationMs(), qint64(3'000));
    player.setSongKey(keyA);
    QCOMPARE(sources.count(), 1);

    player.play();
    QVERIFY(player.playing());
    QCOMPARE(m_outputs.size(), std::size_t(1));
    QCOMPARE(pull(*m_outputs[0], 500).size(), qsizetype(songRate / 2));
    QTRY_VERIFY(player.positionMs() >= 400);
    const qint64 position = player.positionMs();

    // A page refresh sets the same key, and the controller may put the same
    // song again: neither interrupts it.
    player.setSongKey(keyA);
    SongLibrary::instance().put(keyA, m_monoSong);
    QCOMPARE(sources.count(), 1);
    QVERIFY(player.playing());
    QCOMPARE(m_outputs.size(), std::size_t(1));
    QVERIFY(!m_outputs[0]->stopped);
    QVERIFY(player.positionMs() >= position);
    QCOMPARE(pull(*m_outputs[0], 100).size(), qsizetype(songRate / 10));

    // A different key stops (fading the old voice out) and rewinds.
    player.setSongKey(keyB);
    QCOMPARE(sources.count(), 2);
    QVERIFY(!player.playing());
    QCOMPARE(player.positionMs(), 0);
    QCOMPARE(player.durationMs(), qint64(2'000));
    QTRY_VERIFY(m_outputs[0]->destroyed);

    // A page whose song has not arrived yet: invalid until it does, then
    // valid, and still silent.
    player.setSongKey(keyC);
    QCOMPARE(sources.count(), 3);
    QVERIFY(!player.valid());
    SongLibrary::instance().put(keyC, m_monoSong);
    QCOMPARE(sources.count(), 4);
    QVERIFY(player.valid());
    QVERIFY(!player.playing());
    QCOMPARE(m_factoryCalls, 1);
}

void ProfileSongTest::onlyOnePlayerPlaysAtATime()
{
    SongLibrary::instance().put(keyA, m_monoSong);
    SongPlayer first;
    SongPlayer second;
    first.setSongKey(keyA);
    second.setSongKey(keyA);

    first.play();
    QVERIFY(first.playing());
    pull(*m_outputs[0], 500);
    QTRY_VERIFY(first.positionMs() >= 400);

    second.play();
    QVERIFY(second.playing());
    QVERIFY(!first.playing());
    // The first fades out (150 ms) and keeps its place: it was paused.
    const QVector<qint16> tail = pull(*m_outputs[0], 400);
    QCOMPARE(tail.size(), qsizetype(songRate * SongPlayer::fadeOutMs / 1000));
    QVERIFY(first.positionMs() >= 400);
    QTRY_VERIFY(m_outputs[0]->destroyed);
    QVERIFY(!m_outputs[1]->destroyed);

    first.play();
    QVERIFY(first.playing());
    QVERIFY(!second.playing());
    QCOMPARE(m_outputs.size(), std::size_t(3));
}

void ProfileSongTest::inactivePlayerFadesAndStops()
{
    SongLibrary::instance().put(keyA, m_monoSong);
    SongPlayer player;
    player.setSongKey(keyA);
    player.play();
    FakeOutputState &output = *m_outputs.at(0);
    const QVector<qint16> playing = pull(output, 1'000);
    QTRY_VERIFY(player.positionMs() >= 900);
    const double level = rmsMs(playing, 1, 0, 800, 1'000);

    QSignalSpy states(&player, &SongPlayer::stateChanged);
    player.setActive(false);
    QVERIFY(!player.playing());
    QCOMPARE(states.count(), 1);
    QCOMPARE(player.positionMs(), 0);

    // 150 ms of fade, loud at first and silent at the end, then nothing.
    const QVector<qint16> fade = pull(output, 300);
    QCOMPARE(fade.size(), qsizetype(songRate * SongPlayer::fadeOutMs / 1000));
    QVERIFY(rmsMs(fade, 1, 0, 0, 10) > 0.8 * level);
    QVERIFY(rmsMs(fade, 1, 0, 130, 150) < 0.2 * level);
    QTRY_VERIFY(output.destroyed);

    // Covered or hidden, it will not start.
    player.play();
    QVERIFY(!player.playing());
    QCOMPARE(m_factoryCalls, 1);

    player.setActive(true);
    player.play();
    QVERIFY(player.playing());
    QCOMPARE(player.positionMs(), 0);
}

void ProfileSongTest::playerNeverStartsByItself()
{
    SongPlayer player;
    QSignalSpy states(&player, &SongPlayer::stateChanged);
    bool everPlayed = false;
    connect(&player, &SongPlayer::stateChanged, this, [&] { everPlayed = everPlayed || player.playing(); });

    // Everything that happens around a page on screen, except a press of play.
    player.setSongKey(keyA);
    SongLibrary::instance().put(keyA, m_monoSong); // the song arrives
    QVERIFY(player.valid());
    player.setActive(false);
    player.setActive(true);
    player.setSuspended(true);
    player.setSuspended(false);
    player.seek(1'000);
    player.seekBy(SongPlayer::seekStepMs);
    player.setSongKey(keyB);
    player.setSongKey(keyA);
    SongLibrary::instance().put(keyB, m_stereoSong);
    QTest::qWait(300);

    QVERIFY(!player.playing());
    QVERIFY(!everPlayed);
    QCOMPARE(m_factoryCalls, 0); // not even an output was opened
    QVERIFY(states.count() > 0);
}

void ProfileSongTest::playerRefusesWhileSuspended()
{
    SongLibrary::instance().put(keyA, m_monoSong);
    SongPlayer player;
    player.setSongKey(keyA);
    player.setSuspended(true);
    player.play();
    QVERIFY(!player.playing());
    QCOMPARE(m_factoryCalls, 0);

    player.setSuspended(false);
    player.play();
    QVERIFY(player.playing());
    pull(*m_outputs[0], 800);
    QTRY_VERIFY(player.positionMs() >= 700);

    // A call rings: the song fades out and keeps its place.
    player.setSuspended(true);
    QVERIFY(!player.playing());
    const qint64 kept = player.positionMs();
    QVERIFY(kept >= 700);
    QCOMPARE(pull(*m_outputs[0], 400).size(), qsizetype(songRate * SongPlayer::fadeOutMs / 1000));
    QTRY_VERIFY(m_outputs[0]->destroyed);
    QCOMPARE(player.positionMs(), kept);

    // The call ends: nothing resumes until the user presses play.
    player.setSuspended(false);
    QTest::qWait(150);
    QVERIFY(!player.playing());
    QCOMPARE(m_factoryCalls, 1);
    player.play();
    QVERIFY(player.playing());
    QCOMPARE(player.positionMs(), kept);
    const QVector<qint16> resumed = pull(*m_outputs[1], 100);
    QCOMPARE(resumed.size(), qsizetype(songRate / 10));
}

void ProfileSongTest::playerWithoutDeviceReportsError()
{
    // The real system-output path, given the null device a machine without
    // speakers reports.
    SongPlayer::setOutputFactoryForTesting(
        [](int channels, QString &error) { return SongPlayer::openSystemOutput(QAudioDevice(), channels, error); });
    SongLibrary::instance().put(keyA, m_monoSong);
    SongPlayer player;
    player.setSongKey(keyA);
    QSignalSpy states(&player, &SongPlayer::stateChanged);
    player.play();
    QVERIFY(!player.playing());
    QCOMPARE(player.error(), u"No audio output"_s);
    QCOMPARE(states.count(), 1);

    // Once there is an output again, play works and the error clears.
    installFakeOutput();
    player.play();
    QVERIFY(player.playing());
    QVERIFY(player.error().isEmpty());
}

void ProfileSongTest::playerStopsAndRewindsAtTheEnd()
{
    // ARCH §5.5: "at the end: stop, position 0".
    SongLibrary::instance().put(keyA, m_monoSong);
    SongPlayer player;
    player.setSongKey(keyA);
    player.play();
    QVERIFY(player.playing());
    QSignalSpy states(&player, &SongPlayer::stateChanged);
    FakeOutputState &output = *m_outputs.at(0);
    QCOMPARE(pullToTheEnd(output), qsizetype(3) * songRate); // the whole 3 s song
    QTRY_VERIFY(!player.playing());
    QCOMPARE(player.positionMs(), 0);
    QCOMPARE(states.count(), 1);
    QVERIFY(player.error().isEmpty());
    // The device plays out what it holds, then goes.
    QTRY_VERIFY(output.destroyed);

    // Play again starts over, on a new output.
    player.play();
    QVERIFY(player.playing());
    QCOMPARE(m_factoryCalls, 2);
    QCOMPARE(player.positionMs(), 0);
    QCOMPARE(pull(*m_outputs.at(1), 100).size(), qsizetype(songRate / 10));
}

void ProfileSongTest::outputFailureStopsThePlayerAndSaysSo()
{
    SongLibrary::instance().put(keyA, m_monoSong);
    SongPlayer player;
    player.setSongKey(keyA);
    player.play();
    FakeOutputState &output = *m_outputs.at(0);
    pull(output, 1'000);
    QTRY_VERIFY(player.positionMs() >= 900);

    // The headphones are pulled out mid-song.
    QSignalSpy states(&player, &SongPlayer::stateChanged);
    emit output.output->failed(u"The audio output stopped working."_s);
    QTRY_VERIFY(!player.playing());
    QCOMPARE(player.error(), u"The audio output stopped working."_s);
    QVERIFY(player.positionMs() >= 900); // where it stopped
    QCOMPARE(states.count(), 1);
    QVERIFY(output.stopped);
    QVERIFY(output.destroyed);

    // Pressing play again tries a fresh output and clears the error.
    player.play();
    QVERIFY(player.playing());
    QVERIFY(player.error().isEmpty());
    QCOMPARE(m_factoryCalls, 2);
}

void ProfileSongTest::outputFailureAfterTheSongEndedIsHarmless()
{
    // The device fails after pulling the song's last frame but before the
    // next position tick noticed the end (a sink may report a device error
    // as the stream runs dry).
    SongLibrary::instance().put(keyA, m_monoSong);
    SongPlayer player;
    player.setSongKey(keyA);
    player.play();
    FakeOutputState &output = *m_outputs.at(0);
    QCOMPARE(pullToTheEnd(output), qsizetype(3) * songRate);
    QVERIFY(player.playing()); // no tick has run: the event loop has not
    QSignalSpy states(&player, &SongPlayer::stateChanged);
    emit output.output->failed(u"The audio output stopped working."_s);
    // Only the queued failure, never the position timer, runs here.
    QCoreApplication::sendPostedEvents(&player, QEvent::MetaCall);

    // The song is over as if it had ended normally: stopped, rewound, and
    // with nothing to report; the failed device is let go at once.
    QVERIFY(!player.playing());
    QCOMPARE(player.positionMs(), 0);
    QVERIFY(player.error().isEmpty());
    QCOMPARE(states.count(), 1);
    QVERIFY(output.destroyed);

    player.play();
    QVERIFY(player.playing());
    QCOMPARE(m_factoryCalls, 2);
}

void ProfileSongTest::songLibraryKeepsTheThreeLatestSongs()
{
    SongLibrary &library = SongLibrary::instance();
    const QString keyD = QString(64, u'd');
    library.put(keyA, m_monoSong);
    library.put(keyB, m_stereoSong);
    library.put(keyC, m_monoSong);
    library.put(keyA, m_monoSong); // put again: now the most recent
    library.put(keyD, m_stereoSong);
    QCOMPARE(library.get(keyA), m_monoSong);
    QVERIFY(library.get(keyB).isEmpty()); // the least recently put went
    QCOMPARE(library.get(keyC), m_monoSong);
    QCOMPARE(library.get(keyD), m_stereoSong);
    library.put(QString(), m_monoSong);
    library.put(keyB, QByteArray());
    QVERIFY(library.get(keyB).isEmpty());
    QCOMPARE(library.get(keyA), m_monoSong);

    // A player keeps the song it loaded when the library lets it go.
    SongPlayer player;
    player.setSongKey(keyC);
    QVERIFY(player.valid());
    library.release(keyC);
    QVERIFY(library.get(keyC).isEmpty());
    player.play();
    QVERIFY(player.playing());
    QCOMPARE(pull(*m_outputs.at(0), 200).size(), qsizetype(songRate / 5));
    library.clear();
    QVERIFY(library.get(keyA).isEmpty());
}

void ProfileSongTest::fiveMinuteSongPlaysToTheEndAndSeeks()
{
    const std::optional<SongContainer> song = decodeSongContainer(chatSong(), chatSongLimits());
    QVERIFY(song);
    // A stream held to a profile song's limits refuses it outright, rather
    // than playing 45 s of it and stopping.
    {
        SongStream refused(*song, pcmFormat(songRate, 2));
        QVERIFY(!refused.isValid());
        QVERIFY(refused.finished());
    }
    // Held to a chat's, every frame of the five minutes plays.
    {
        SongStream stream(*song, pcmFormat(songRate, 2), 0, chatSongLimits());
        QVERIFY(stream.isValid());
        QCOMPARE(countFrames(stream, 2), qint64(300) * songRate);
        QVERIFY(stream.finished());
        QCOMPARE(stream.framesPlayed(), qint64(300) * songRate);
    }
    // From 4:30: the last half minute, where the left note is 880 Hz.
    {
        SongStream stream(*song, pcmFormat(songRate, 2), qint64(270) * songRate, chatSongLimits());
        QCOMPARE(stream.framesPlayed(), qint64(270) * songRate);
        const QVector<qint16> rest = asS16(readAll(stream));
        QCOMPARE(rest.size(), qsizetype(30) * songRate * 2);
        QVERIFY(dominates(rest, 2, 0, 1.0, 1.0, songRate, 880.0, {440.0, 660.0}));
        QVERIFY(dominates(rest, 2, 1, 1.0, 1.0, songRate, 660.0, {440.0, 880.0}));
    }
    // And a seek back into the first part mid-stream.
    {
        SongStream stream(*song, pcmFormat(44'100, 2), 0, chatSongLimits());
        (void)stream.read(44'100 * 4); // a second
        stream.seekToSample(qint64(60) * songRate);
        const QVector<qint16> minute = asS16(stream.read(44'100 * 4 * 2));
        QCOMPARE(minute.size(), qsizetype(44'100) * 2 * 2);
        QVERIFY(dominates(minute, 2, 0, 0.5, 1.0, 44'100, 440.0, {880.0, 660.0}));
    }
}

void ProfileSongTest::longFormPlayerPlaysAChatSong()
{
    SongLibrary::instance().put(keyA, chatSong());
    SongPlayer player;
    player.setSongKey(keyA);
    // A profile's player refuses a five-minute song.
    QVERIFY(!player.valid());
    QCOMPARE(player.durationMs(), 0);

    QSignalSpy sources(&player, &SongPlayer::sourceChanged);
    player.setLongForm(true);
    QVERIFY(player.longForm());
    QCOMPARE(sources.count(), 1);
    QVERIFY(player.valid());
    QCOMPARE(player.durationMs(), AttachmentLimits::maxAudioMs);
    player.setLongForm(true);
    QCOMPARE(sources.count(), 1);

    player.play();
    QVERIFY(player.playing());
    player.seek(270'000);
    QCOMPARE(player.positionMs(), qint64(270'000));
    FakeOutputState &output = *m_outputs.at(0);
    QCOMPARE(output.format.channelCount(), 2);
    QCOMPARE(pullToTheEnd(output), qsizetype(30) * songRate);
    QTRY_VERIFY(!player.playing());
    QCOMPARE(player.positionMs(), 0);
    QVERIFY(player.error().isEmpty());

    // Playing again, then going back to a profile's limits: the song stops
    // (as a new key would stop it) and is refused.
    player.play();
    QVERIFY(player.playing());
    player.setLongForm(false);
    QVERIFY(!player.playing());
    QVERIFY(!player.valid());
    QCOMPARE(sources.count(), 2);
    player.play();
    QVERIFY(!player.playing());
}

QTEST_MAIN(ProfileSongTest)
#include "tst_profilesong.moc"
