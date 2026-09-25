#include "domain/ClipContainer.h"
#include "domain/ProfilePageCodec.h"
#include "domain/SongContainer.h"
#include "media/SongCodec.h"
#include "profile/ClipCodec.h"
#include "profile/ClipImport.h"
#include "profile/ProfileBackgroundImage.h"
#include "profile/ProfileClipPlayer.h"
#include "profile/ProfilePanelMedia.h"
#include "profile/SongPlayer.h"

#include <QBuffer>
#include <QImageWriter>
#include <QPainter>
#include <QProcess>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>
#include <memory>
#include <vector>

using namespace OpenChat;

namespace {

// An audio output the test pulls from itself, so a clip's sound clock moves
// exactly as far as the test says (and no sound card is needed).
struct FakeOutputState {
    QAudioFormat format;
    QIODevice *stream = nullptr;
};

class FakeSongOutput final : public SongOutput
{
public:
    explicit FakeSongOutput(std::shared_ptr<FakeOutputState> state)
        : m_state(std::move(state))
    {
    }
    ~FakeSongOutput() override { m_state->stream = nullptr; }
    [[nodiscard]] QAudioFormat format() const override { return m_state->format; }
    [[nodiscard]] int bufferMs() const override { return 0; }
    bool start(QIODevice *stream) override
    {
        m_state->stream = stream;
        return true;
    }
    void stop() override { m_state->stream = nullptr; }

private:
    std::shared_ptr<FakeOutputState> m_state;
};

// Frames pulled from an output's stream: `ms` of them, or what is left.
qint64 pullFrames(FakeOutputState &state, qint64 ms)
{
    if (state.stream == nullptr)
        return 0;
    const qint64 frameBytes = state.format.bytesPerFrame();
    qint64 frames = 0;
    qint64 wanted = ms * state.format.sampleRate() / 1000;
    while (wanted > 0) {
        const QByteArray chunk = state.stream->read(std::min<qint64>(wanted, 4'800) * frameBytes);
        if (chunk.isEmpty())
            break;
        frames += chunk.size() / frameBytes;
        wanted -= chunk.size() / frameBytes;
    }
    return frames;
}

QString keyOf(const QByteArray &bytes)
{
    return QString::fromLatin1(pageMediaHash(bytes).toHex());
}

// A picture that moves: a gradient and a square sliding across it.
QImage movingFrame(QSize size, int index)
{
    QImage image(size, QImage::Format_RGB32);
    QPainter painter(&image);
    QLinearGradient gradient(0, 0, size.width(), size.height());
    gradient.setColorAt(0, QColor::fromHsv((index * 7) % 360, 180, 230));
    gradient.setColorAt(1, QColor::fromHsv((index * 7 + 120) % 360, 200, 120));
    painter.fillRect(image.rect(), gradient);
    painter.fillRect(QRect((index * 9) % size.width(), size.height() / 3, 40, 40), Qt::white);
    return image;
}

WavAudio tone(int rate, int channels, qint64 ms)
{
    WavAudio audio;
    audio.sampleRate = rate;
    audio.channels = channels;
    const qint64 frames = ms * rate / 1000;
    for (qint64 i = 0; i < frames; ++i) {
        for (int c = 0; c < channels; ++c)
            audio.samples.push_back(qint16(9000 * std::sin(2 * M_PI * (440 + 110 * c) * double(i) / rate)));
    }
    return audio;
}

// A JPEG with its last scan repeated `copies` more times. libjpeg decodes a
// progressive file like that (it only warns); the scan count is what must
// refuse it.
QByteArray withRepeatedScan(const QByteArray &jpeg, int copies)
{
    const qsizetype lastScan = jpeg.lastIndexOf(QByteArrayView("\xFF\xDA", 2));
    const qsizetype end = jpeg.size() - 2; // the EOI
    if (lastScan < 0 || end <= lastScan)
        return {};
    const QByteArray scan = jpeg.mid(lastScan, end - lastScan);
    QByteArray out = jpeg.left(end);
    for (int i = 0; i < copies; ++i)
        out += scan;
    out += jpeg.right(2);
    return out;
}

QByteArray progressiveJpeg(const QImage &image)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    QImageWriter writer(&buffer, "jpeg");
    writer.setQuality(80);
    writer.setProgressiveScanWrite(true);
    return writer.write(image) ? bytes : QByteArray();
}

// Picture noise: nothing for a JPEG encoder to save.
QImage noisePicture(QSize size, quint32 seed)
{
    QImage image(size, QImage::Format_RGB32);
    QRandomGenerator generator(seed);
    for (int y = 0; y < size.height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < size.width(); ++x)
            line[x] = 0xFF000000U | (generator.generate() & 0x00FFFFFFU);
    }
    return image;
}

} // namespace

class ProfileClipTest final : public QObject
{
    Q_OBJECT

private slots:
    void frameSizeKeepsAspectAndEvenSides()
    {
        QCOMPARE(clipFrameSize({1920, 1080}, 480), QSize(480, 270));
        QCOMPARE(clipFrameSize({1080, 1920}, 480), QSize(270, 480));
        QCOMPARE(clipFrameSize({321, 241}, 480), QSize(320, 240)); // never enlarged, made even
        QCOMPARE(clipFrameSize({4000, 10}, 480), QSize(480, 16));   // never below the minimum
        QVERIFY(clipFrameSize({}, 480).isEmpty());
    }

    void clipRoundTripsThroughEveryStage()
    {
        if (!clipCodecAvailable())
            QSKIP("This build has no libvpx");
        const QSize size(320, 180);
        const int fps = 15;
        const QVector<qint64> durations{5'000, 5'000, 2'040};
        QVector<QVector<ClipContainer::Frame>> video;
        int frameIndex = 0;
        for (const qint64 length : durations) {
            QVector<QImage> frames;
            const int count = int((length * fps + 999) / 1000);
            for (int i = 0; i < count; ++i)
                frames.push_back(movingFrame(size, frameIndex++));
            const auto encoded = encodeClipVideo(frames, fps, {300, 200, 120}, 190 * 1024);
            QVERIFY(encoded);
            QCOMPARE(encoded->size(), frames.size());
            QVERIFY(encoded->first().key);
            video.push_back(*encoded);
        }
        const qint64 total = 12'040;
        const auto audio = encodeClipAudio(tone(44'100, 2, total), total, 40'000);
        QVERIFY(audio);
        QCOMPARE(audio->channels, 2);
        const QVector<QByteArray> segments = packClip(video, durations, size, fps, *audio, maxClipSegmentBytes);
        QCOMPARE(segments.size(), 3);

        QVector<ClipContainer> clips;
        for (const QByteArray &bytes : segments) {
            QVERIFY(bytes.size() <= maxClipSegmentBytes);
            // Each segment is valid on its own: exactly what arrival checks.
            QVERIFY(decodePageMedia(encodePageMedia({Profile::MediaKind::VideoSegmentMedia, pageMediaHash(bytes), bytes})));
            const auto clip = decodeClipContainer(bytes);
            QVERIFY(clip);
            clips.push_back(*clip);
        }
        QCOMPARE(clips.at(0).audioPreSkip, audio->preSkip);
        QCOMPARE(clips.at(1).audioPreSkip, 0);

        // The sound, glued back together, is one valid song.
        const auto soundtrack = clipSoundtrack(clips);
        QVERIFY(soundtrack);
        QCOMPARE(soundtrack->totalSamples, total * 48);
        SongDecoder decoder(*soundtrack);
        QVERIFY(decoder.isValid());
        qint64 decoded = 0;
        while (!decoder.atEnd())
            decoded += decoder.next().size() / soundtrack->channels;
        QCOMPARE(decoded, soundtrack->totalSamples);

        // And the pictures decode, at their size.
        ClipVideoDecoder pictures;
        QVERIFY(pictures.isValid());
        int shown = 0;
        for (const ClipContainer &clip : clips) {
            for (const ClipContainer::Frame &frame : clip.frames) {
                const QImage picture = pictures.decode(frame, true);
                QCOMPARE(picture.size(), size);
                ++shown;
            }
        }
        QCOMPARE(shown, frameIndex);
    }

    void silentClipHasNoSoundtrack()
    {
        if (!clipCodecAvailable())
            QSKIP("This build has no libvpx");
        const QSize size(160, 90);
        QVector<QImage> frames;
        for (int i = 0; i < 15; ++i)
            frames.push_back(movingFrame(size, i));
        const auto encoded = encodeClipVideo(frames, 15, {200}, 100 * 1024);
        QVERIFY(encoded);
        const auto silence = encodeClipAudio({}, 1'000, 32'000);
        QVERIFY(silence);
        QCOMPARE(silence->channels, 0);
        const QVector<QByteArray> segments = packClip({*encoded}, {1'000}, size, 15, *silence, maxClipSegmentBytes);
        QCOMPARE(segments.size(), 1);
        const auto clip = decodeClipContainer(segments.first());
        QVERIFY(clip);
        QCOMPARE(clip->audioChannels, 0);
        QVERIFY(!clipSoundtrack({*clip}));
    }

    void encoderGivesUpWhenNothingFits()
    {
        if (!clipCodecAvailable())
            QSKIP("This build has no libvpx");
        QVector<QImage> frames;
        for (int i = 0; i < 15; ++i)
            frames.push_back(movingFrame({320, 180}, i));
        QVERIFY(!encodeClipVideo(frames, 15, {300}, 64)); // 64 bytes: nothing fits
        QVERIFY(!encodeClipVideo(frames, 15, {300}, 100 * 1024, [] { return true; })); // cancelled
        frames.last() = movingFrame({322, 180}, 0);
        QVERIFY(!encodeClipVideo(frames, 15, {300}, 100 * 1024)); // sizes differ
    }

    void decoderSurvivesGarbage()
    {
        if (!clipCodecAvailable())
            QSKIP("This build has no libvpx");
        ClipVideoDecoder decoder;
        QVERIFY(decoder.isValid());
        QVERIFY(decoder.decode({true, QByteArray(500, '\x7f')}, true).isNull());
        QVERIFY(decoder.decode({false, QByteArray()}, true).isNull());
    }

    // The whole import, from a real video file, when this machine has ffmpeg
    // to write one and a Qt Multimedia backend to read it.
    void importsARealVideoFile()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const std::optional<ImportedClip> imported = importTestVideo(directory, 7);
        if (QTest::currentTestResolved()) // failed or skipped inside
            return;
        QVERIFY(imported);
        const ImportedClip &clip = *imported;
        QCOMPARE(clip.segments.size(), 2);
        QCOMPARE(clip.durationsMs, (QVector<quint32>{5'000, 2'000}));
        QCOMPARE(clip.size, QSize(480, 270));
        QVERIFY(!clip.posterJpeg.isEmpty());
        QVERIFY(!clip.trimmed); // the whole of it fitted
        QVector<ClipContainer> segments;
        for (const QByteArray &bytes : clip.segments) {
            const auto segment = decodeClipContainer(bytes);
            QVERIFY(segment);
            segments.push_back(*segment);
        }
        QVERIFY(clipSoundtrack(segments)); // the sine came along
    }

    // A long video keeps its first 30 seconds: six segments of five.
    void cutsLongVideosAtThirtySeconds()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const std::optional<ImportedClip> imported = importTestVideo(directory, 35);
        if (QTest::currentTestResolved()) // failed or skipped inside
            return;
        QVERIFY(imported);
        QVERIFY(imported->trimmed);
        QCOMPARE(imported->segments.size(), 6);
        QCOMPARE(imported->durationsMs, QVector<quint32>(6, 5'000));
        qint64 total = 0;
        for (const QByteArray &bytes : imported->segments) {
            QVERIFY(bytes.size() <= maxClipSegmentBytes);
            total += bytes.size();
        }
        // Well within the panels' 4 MiB, beside other pictures.
        QVERIFY2(total < 1'600'000, qPrintable(QString::number(total)));
    }

    // Past a profile song's 45 s a clip's sound is refused by a profile's
    // limits (the clip plays silent), and kept by a chat video's.
    void soundtrackOverFortyFiveSecondsNeedsLongForm()
    {
        if (!clipCodecAvailable())
            QSKIP("This build has no libvpx");
        const QVector<QByteArray> segments = makeClip(10, 5'000, true);
        QCOMPARE(segments.size(), 10);
        QVector<ClipContainer> clips;
        for (const QByteArray &bytes : segments)
            clips.push_back(*decodeClipContainer(bytes));
        QVERIFY(!clipSoundtrack(clips));
        const auto soundtrack = clipSoundtrack(clips, chatSongLimits());
        QVERIFY(soundtrack);
        QCOMPARE(soundtrack->totalSamples, qint64(50) * 48'000);
        SongDecoder decoder(*soundtrack, chatSongLimits());
        QVERIFY(decoder.isValid());
        qint64 decoded = 0;
        while (!decoder.atEnd())
            decoded += decoder.next().size() / soundtrack->channels;
        QCOMPARE(decoded, soundtrack->totalSamples);

        // The player: silent as a panel, with its sound as a chat's video.
        const QStringList keys = putSegments(segments);
        ProfileClipPlayer player;
        player.setSegmentKeys(keys);
        player.setPlaying(true);
        QVERIFY(player.playing());
        QCOMPARE(player.durationMs(), qint64(50'000));
        QVERIFY(!player.hasSound());
        QVERIFY(m_outputs.empty()); // nothing to play: no output opened
        player.setPlaying(false);

        QSignalSpy sources(&player, &ProfileClipPlayer::segmentKeysChanged);
        player.setLongForm(true);
        QVERIFY(player.longForm());
        QVERIFY(player.hasSound());
        QCOMPARE(sources.count(), 1);
        player.setPlaying(true);
        QCOMPARE(m_outputs.size(), std::size_t(1));
        // The sound is the clock: a second pulled is a second played.
        QCOMPARE(pullFrames(*m_outputs.at(0), 1'000), qint64(48'000));
        QTRY_VERIFY(player.positionMs() >= 750);

        // A seek lands on its segment's start (a keyframe), sound and all,
        // and the last five seconds play out to the end.
        QSignalSpy finished(&player, &ProfileClipPlayer::finished);
        player.seek(47'300);
        QCOMPARE(player.positionMs(), qint64(45'000));
        QVERIFY(player.playing());
        QCOMPARE(m_outputs.size(), std::size_t(2));
        QVERIFY(m_outputs.at(0)->stream == nullptr); // the old voice stopped
        QCOMPARE(pullFrames(*m_outputs.at(1), 10'000), qint64(5) * 48'000);
        QTRY_VERIFY(!player.playing());
        QCOMPARE(finished.count(), 1);
        releaseSegments(keys);
    }

    void clipPlayerPausesResumesAndSeeks()
    {
        if (!clipCodecAvailable())
            QSKIP("This build has no libvpx");
        // A silent clip, so the clock is the wall clock: 5 s, 5 s and 2 s.
        const QVector<QByteArray> segments = makeClip(3, 5'000, false, 2'000);
        const QStringList keys = putSegments(segments);
        ProfileClipPlayer player;
        player.setSegmentKeys(keys);
        QSignalSpy pictures(&player, &ProfileClipPlayer::hasPictureChanged);
        QVERIFY(!player.hasPicture());
        QVERIFY(!player.paused());

        player.setPlaying(true);
        QVERIFY(player.playing());
        QVERIFY(player.hasPicture()); // the first frame is up at once
        QCOMPARE(pictures.count(), 1);
        QCOMPARE(player.durationMs(), qint64(12'000));
        QTRY_VERIFY(player.positionMs() >= 500);

        // Paused: the place and the picture hold, and `playing` stays.
        QSignalSpy pausedChanges(&player, &ProfileClipPlayer::pausedChanged);
        player.setPaused(true);
        QCOMPARE(pausedChanges.count(), 1);
        QVERIFY(player.paused());
        QVERIFY(player.playing());
        const qint64 held = player.positionMs();
        QVERIFY(held >= 500);
        QTest::qWait(400);
        QCOMPARE(player.positionMs(), held);
        QVERIFY(player.hasPicture());

        // Resumed: on from the same place, not from the start.
        player.setPaused(false);
        QCOMPARE(player.positionMs(), held);
        QTRY_VERIFY(player.positionMs() >= held + 250);

        // A seek goes to the start of the segment it falls in.
        player.seek(7'400);
        QCOMPARE(player.positionMs(), qint64(5'000));
        QVERIFY(player.playing());
        QTRY_VERIFY(player.positionMs() >= 5'250);

        // Paused, a seek moves the place (and the picture) but not the clock.
        player.setPaused(true);
        player.seek(11'000);
        QCOMPARE(player.positionMs(), qint64(10'000));
        QTest::qWait(300);
        QCOMPARE(player.positionMs(), qint64(10'000));
        QVERIFY(player.hasPicture());
        QSignalSpy finished(&player, &ProfileClipPlayer::finished);
        player.setPaused(false);
        QTRY_VERIFY_WITH_TIMEOUT(!player.playing(), 10'000); // the last 2 s
        QCOMPARE(finished.count(), 1);

        // Stopped, a seek is where the next play starts; after that, the
        // start again.
        player.seek(6'000);
        QCOMPARE(player.positionMs(), qint64(5'000));
        player.setPlaying(true);
        QVERIFY(player.positionMs() >= 5'000);
        QTRY_VERIFY(player.positionMs() >= 5'250);
        player.setPlaying(false);
        player.setPlaying(true);
        QVERIFY(player.positionMs() < 1'000);
        player.setPlaying(false);

        // New keys: no picture until something is decoded again.
        player.setSegmentKeys({});
        QVERIFY(!player.hasPicture());
        releaseSegments(keys);
    }

    void clipPlayerRoundsItsCorners()
    {
        if (!clipCodecAvailable())
            QSKIP("This build has no libvpx");
        const QVector<QByteArray> segments = makeClip(1, 2'000, false);
        const QStringList keys = putSegments(segments);
        ProfileClipPlayer player;
        player.setSize(QSizeF(64, 36)); // the clip's own shape
        player.setSegmentKeys(keys);
        player.seek(0); // stopped: shows the first picture
        QVERIFY(player.hasPicture());
        const auto render = [&player] {
            QImage image(64, 36, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            player.paint(&painter);
            return image;
        };
        // Square by default, as a panel has always painted it.
        QImage square = render();
        QCOMPARE(qAlpha(square.pixel(0, 0)), 255);
        QCOMPARE(qAlpha(square.pixel(63, 35)), 255);

        QSignalSpy radii(&player, &ProfileClipPlayer::radiusChanged);
        player.setRadius(10);
        QCOMPARE(radii.count(), 1);
        QImage rounded = render();
        QCOMPARE(qAlpha(rounded.pixel(0, 0)), 0);
        QCOMPARE(qAlpha(rounded.pixel(63, 35)), 0);
        QCOMPARE(qAlpha(rounded.pixel(32, 18)), 255);
        QCOMPARE(qAlpha(rounded.pixel(32, 0)), 255); // only the corners are cut
        releaseSegments(keys);
    }

    void panelLibraryTakesChatPhotosButNoScanBombs()
    {
        // Decodes come at 320, 640 and 1280, and at 2048 only beyond 1280.
        QCOMPARE(PanelMediaLibrary::bucketFor(320), 320);
        QCOMPARE(PanelMediaLibrary::bucketFor(321), 640);
        QCOMPARE(PanelMediaLibrary::bucketFor(1'280), 1'280);
        QCOMPARE(PanelMediaLibrary::bucketFor(1'281), 2'048);
        QCOMPARE(PanelMediaLibrary::bucketFor(9'000), 2'048);

        PanelMediaLibrary &library = PanelMediaLibrary::instance();
        QSignalSpy decoded(&library, &PanelMediaLibrary::decoded);
        const auto decodes = [&](const QByteArray &bytes, int side) -> QImage {
            decoded.clear();
            const QString key = keyOf(bytes);
            library.put(key, bytes);
            QImage image = library.image(key, side);
            if (image.isNull()) {
                const auto isKey = [&] {
                    return std::any_of(decoded.cbegin(), decoded.cend(),
                                       [&](const QList<QVariant> &arguments) { return arguments.at(0) == key; });
                };
                // A refused picture never decodes: give it the time a good
                // one of the same size took, then look.
                (void)QTest::qWaitFor(isKey, 3'000);
                image = library.image(key, side);
            }
            library.release(key);
            return image;
        };

        // A chat photo: far past a panel picture's 224 KiB, within 2 MiB.
        const QByteArray photo = encodeBaselineJpeg(noisePicture({1'800, 1'200}, 5), 70);
        QVERIFY2(photo.size() > maxPanelImageBytes && photo.size() <= PanelMediaLibrary::maxPictureBytes,
                 qPrintable(QString::number(photo.size())));
        QCOMPARE(decodes(photo, 1'800).size(), QSize(1'800, 1'200)); // never scaled up to 2048
        QCOMPARE(decodes(photo, 1'000).size(), QSize(1'280, 853));
        // Past 2 MiB it is refused, whatever it is.
        const QByteArray huge = encodeBaselineJpeg(noisePicture({2'400, 2'000}, 6), 100);
        QVERIFY(huge.size() > PanelMediaLibrary::maxPictureBytes);
        QVERIFY(decodes(huge, 640).isNull());

        // A progressive picture with its last scan repeated: libjpeg reads
        // it (it only warns), so the scan count alone keeps it out.
        const QByteArray progressive = progressiveJpeg(movingFrame({320, 180}, 3));
        const int scans = jpegScanCount(progressive);
        QVERIFY2(scans > 1 && scans < 20, qPrintable(QString::number(scans)));
        const QByteArray repeated = withRepeatedScan(progressive, 30 - scans);
        QCOMPARE(jpegScanCount(repeated), 30);
        QCOMPARE(decodes(repeated, 320).size(), QSize(320, 180));
        const QByteArray bomb = withRepeatedScan(progressive, maxJpegScans + 1 - scans);
        QCOMPARE(jpegScanCount(bomb), maxJpegScans + 1);
        QVERIFY(decodes(bomb, 320).isNull());
    }

    void init()
    {
        m_outputs.clear();
        SongPlayer::setOutputFactoryForTesting([this](int channels, QString &) {
            auto state = std::make_shared<FakeOutputState>();
            state->format.setSampleRate(48'000);
            state->format.setChannelCount(channels);
            state->format.setSampleFormat(QAudioFormat::Int16);
            m_outputs.push_back(state);
            return std::make_unique<FakeSongOutput>(state);
        });
    }

    void cleanup() { SongPlayer::setOutputFactoryForTesting({}); }

private:
    // A clip of `count` segments of `segmentMs` (the last `lastMs` when
    // given), tiny and at 5 pictures a second so it encodes fast, with a
    // stereo tone throughout when `withSound`.
    QVector<QByteArray> makeClip(int count, qint64 segmentMs, bool withSound, qint64 lastMs = 0)
    {
        const QSize size(64, 36);
        const int fps = 5;
        QVector<qint64> durations(count, segmentMs);
        if (lastMs > 0)
            durations.last() = lastMs;
        QVector<QVector<ClipContainer::Frame>> video;
        qint64 total = 0;
        int index = 0;
        for (const qint64 length : std::as_const(durations)) {
            QVector<QImage> frames;
            for (int i = 0; i < int((length * fps + 999) / 1000); ++i)
                frames.push_back(movingFrame(size, index++));
            const auto encoded = encodeClipVideo(frames, fps, {120, 60}, 60 * 1024);
            if (!encoded)
                return {};
            video.push_back(*encoded);
            total += length;
        }
        const auto audio = encodeClipAudio(withSound ? tone(48'000, 2, total) : WavAudio{}, total, 40'000);
        if (!audio)
            return {};
        return packClip(video, durations, size, fps, *audio, maxClipSegmentBytes);
    }

    static QStringList putSegments(const QVector<QByteArray> &segments)
    {
        QStringList keys;
        for (const QByteArray &bytes : segments) {
            keys.push_back(keyOf(bytes));
            PanelMediaLibrary::instance().put(keys.last(), bytes);
        }
        return keys;
    }

    static void releaseSegments(const QStringList &keys)
    {
        for (const QString &key : keys)
            PanelMediaLibrary::instance().release(key);
    }

    std::vector<std::shared_ptr<FakeOutputState>> m_outputs;

    // Writes a test video of `seconds` with ffmpeg and imports it. Skips the
    // test when this machine cannot write or read one.
    std::optional<ImportedClip> importTestVideo(QTemporaryDir &directory, int seconds)
    {
        if (!clipCodecAvailable()) {
            QTest::qSkip("This build has no libvpx", __FILE__, __LINE__);
            return std::nullopt;
        }
        const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
        if (ffmpeg.isEmpty()) {
            QTest::qSkip("No ffmpeg to write a test video", __FILE__, __LINE__);
            return std::nullopt;
        }
        const QString path = directory.filePath(QStringLiteral("clip.mp4"));
        const QString length = QString::number(seconds);
        QProcess process;
        process.start(ffmpeg, {QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-f"),
                               QStringLiteral("lavfi"), QStringLiteral("-i"),
                               QStringLiteral("testsrc2=size=640x360:rate=30:duration=") + length,
                               QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
                               QStringLiteral("sine=frequency=440:duration=") + length, QStringLiteral("-c:v"),
                               QStringLiteral("libx264"), QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
                               QStringLiteral("-c:a"), QStringLiteral("aac"), QStringLiteral("-shortest"), path});
        if (!process.waitForFinished(120'000) || process.exitCode() != 0) {
            QTest::qSkip("ffmpeg could not write the test video", __FILE__, __LINE__);
            return std::nullopt;
        }
        ClipImporter importer;
        QSignalSpy finished(&importer, &ClipImporter::finished);
        QSignalSpy failed(&importer, &ClipImporter::failed);
        importer.start(path);
        if (!QTest::qWaitFor([&] { return !finished.isEmpty() || !failed.isEmpty(); }, 120'000)) {
            QTest::qFail("The import never finished", __FILE__, __LINE__);
            return std::nullopt;
        }
        if (!failed.isEmpty()) {
            const QString message = failed.first().first().toString();
            if (message.contains(QStringLiteral("can't be opened"))) {
                QTest::qSkip("No Qt Multimedia backend reads MP4 here", __FILE__, __LINE__);
                return std::nullopt;
            }
            QTest::qFail(qPrintable(message), __FILE__, __LINE__);
            return std::nullopt;
        }
        return finished.first().first().value<ImportedClip>();
    }
};

QTEST_MAIN(ProfileClipTest)
#include "tst_profileclip.moc"
