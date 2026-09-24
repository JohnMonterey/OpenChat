#include "domain/ClipContainer.h"
#include "domain/ProfilePageCodec.h"
#include "media/SongCodec.h"
#include "profile/ClipCodec.h"
#include "profile/ClipImport.h"

#include <QPainter>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>

using namespace OpenChat;

namespace {

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

private:
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
