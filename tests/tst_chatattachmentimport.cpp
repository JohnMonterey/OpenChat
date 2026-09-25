#include "domain/Attachment.h"
#include "domain/ClipContainer.h"
#include "domain/ProfilePageCodec.h"
#include "domain/SongContainer.h"
#include "media/WavFile.h"
#include "profile/ChatAttachmentImport.h"
#include "profile/ClipCodec.h"

#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QPainter>
#include <QProcess>
#include <QRandomGenerator>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>
#include <functional>
#include <numbers>
#include <optional>

using namespace OpenChat;
using namespace Qt::StringLiterals;

namespace {

// What one import came to: the attachment or the card's sentence, and every
// progress value on the way.
struct Outcome {
    std::optional<PreparedAttachment> attachment;
    QString failure;
    QVector<qreal> progress;
    QVector<qint64> progressAtMs;
    int results = 0; // finished and failed together: exactly one, ever
};

// Starts an import and waits for its one result, then a little longer to be
// sure nothing follows it.
Outcome importWith(const std::function<void(ChatAttachmentImporter &)> &start, int timeoutMs = 60'000)
{
    ChatAttachmentImporter importer;
    Outcome outcome;
    QElapsedTimer clock;
    clock.start();
    QObject::connect(&importer, &ChatAttachmentImporter::progressChanged, &importer, [&](qreal done) {
        outcome.progress.push_back(done);
        outcome.progressAtMs.push_back(clock.elapsed());
    });
    QObject::connect(&importer, &ChatAttachmentImporter::finished, &importer,
                     [&](const PreparedAttachment &attachment) {
                         outcome.attachment = attachment;
                         ++outcome.results;
                     });
    QObject::connect(&importer, &ChatAttachmentImporter::failed, &importer, [&](const QString &message) {
        outcome.failure = message;
        ++outcome.results;
    });
    start(importer);
    if (!QTest::qWaitFor([&] { return outcome.results > 0; }, timeoutMs)) {
        outcome.failure = u"the import never finished"_s;
        return outcome;
    }
    if (importer.busy())
        outcome.failure = u"still busy after its result"_s;
    QTest::qWait(50);
    return outcome;
}

Outcome importFile(const QString &path, int timeoutMs = 60'000)
{
    return importWith([&path](ChatAttachmentImporter &importer) { importer.start(path); }, timeoutMs);
}

// A receiver's whole check, once the sender has minted a key.
bool receiversAccept(const PreparedAttachment &attachment)
{
    AttachmentDescriptor keyed = attachment.descriptor;
    keyed.key = QByteArray(AttachmentLimits::keyBytes, '\x5a');
    return isValidDescriptor(keyed) && attachmentBlobIsValid(keyed, attachment.blob);
}

bool progressIsOrderly(const QVector<qreal> &progress)
{
    if (progress.isEmpty() || progress.first() != 0.0 || progress.last() != 1.0)
        return false;
    for (qsizetype i = 1; i < progress.size(); ++i) {
        if (progress.at(i) <= progress.at(i - 1))
            return false;
    }
    return true;
}

// A photo worth compressing: a gradient with a little grain.
QImage photo(QSize size)
{
    QImage image(size, QImage::Format_RGB32);
    QPainter painter(&image);
    QLinearGradient gradient(0, 0, size.width(), size.height());
    gradient.setColorAt(0, QColor(30, 90, 200));
    gradient.setColorAt(1, QColor(240, 180, 40));
    painter.fillRect(image.rect(), gradient);
    QRandomGenerator generator(11);
    for (int i = 0; i < size.width() * size.height() / 50; ++i)
        painter.fillRect(QRect(generator.bounded(size.width()), generator.bounded(size.height()), 2, 2),
                         QColor::fromRgb(generator.generate()));
    return image;
}

QByteArray randomBytes(qsizetype count, quint32 seed)
{
    QByteArray bytes(count, Qt::Uninitialized);
    QRandomGenerator generator(seed);
    generator.fillRange(reinterpret_cast<quint32 *>(bytes.data()), count / 4);
    return bytes;
}

WavAudio tone(int rate, int channels, double seconds, double amplitude = 0.3)
{
    WavAudio audio;
    audio.sampleRate = rate;
    audio.channels = channels;
    const auto frames = qsizetype(std::llround(seconds * rate));
    audio.samples.resize(frames * channels);
    for (qsizetype frame = 0; frame < frames; ++frame) {
        for (int channel = 0; channel < channels; ++channel) {
            const double t = double(frame) / rate;
            const double frequency = 440.0 + 220.0 * channel;
            audio.samples[frame * channels + channel] =
                qint16(std::lround(amplitude * 32767.0 * std::sin(2.0 * std::numbers::pi * frequency * t)));
        }
    }
    return audio;
}

} // namespace

class ChatAttachmentImportTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void guessesTheKindFromTheNameAlone_data();
    void guessesTheKindFromTheNameAlone();
    void photosAreOnlyWhatThisComputerCanRead();
    void photoBecomesABaselineJpegWithAPreview();
    void smallPhotoKeepsItsSizeOnWhite();
    void pastedPictureBecomesAPhoto();
    void unreadablePhotoIsSentAsAFile();
    void unreadablePhotoTooLargeForAFileIsRefused();
    void audioBecomesOneSongWithAWaveform();
    void longAudioKeepsTheFirstFiveMinutes();
    void silentAudioIsSentAsAFile();
    void videoBecomesASequenceWithAPoster();
    void longVideoKeepsTheFirstMinute();
    void brokenVideoIsSentAsAFile();
    void fileIsSentAsItIs();
    void filesOverSixteenMegabytesAreRefused();
    void emptyMissingAndFolderPathsAreRefused();
    void cancelStopsAJobWithoutAWord();
    void aNewStartReplacesTheJobUnderWay();
    void videoSupportFollowsTheCodec();

private:
    QString writeFile(const QString &name, const QByteArray &bytes);
    QString writeWav(const QString &name, const WavAudio &audio);
    // Writes a test video with ffmpeg; skips the test (and returns an empty
    // path) when this machine cannot.
    QString writeVideo(const QString &name, int seconds, const QString &size);

    QTemporaryDir m_dir;
};

void ChatAttachmentImportTest::initTestCase()
{
    QVERIFY(m_dir.isValid());
}

QString ChatAttachmentImportTest::writeFile(const QString &name, const QByteArray &bytes)
{
    const QString path = m_dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(bytes) != bytes.size())
        qFatal("cannot write %s", qPrintable(path));
    return path;
}

QString ChatAttachmentImportTest::writeWav(const QString &name, const WavAudio &audio)
{
    const QString path = m_dir.filePath(name);
    if (!WavFile::writeFile(path, audio))
        qFatal("cannot write %s", qPrintable(path));
    return path;
}

QString ChatAttachmentImportTest::writeVideo(const QString &name, int seconds, const QString &size)
{
    if (!clipCodecAvailable()) {
        QTest::qSkip("This build has no libvpx", __FILE__, __LINE__);
        return {};
    }
    const QString ffmpeg = QStandardPaths::findExecutable(u"ffmpeg"_s);
    if (ffmpeg.isEmpty()) {
        QTest::qSkip("No ffmpeg to write a test video", __FILE__, __LINE__);
        return {};
    }
    const QString path = m_dir.filePath(name);
    const QString length = QString::number(seconds);
    QProcess process;
    process.start(ffmpeg, {u"-loglevel"_s, u"error"_s, u"-y"_s, u"-f"_s, u"lavfi"_s, u"-i"_s,
                           u"testsrc2=size="_s + size + u":rate=30:duration="_s + length, u"-f"_s, u"lavfi"_s,
                           u"-i"_s, u"sine=frequency=440:duration="_s + length, u"-c:v"_s, u"libx264"_s,
                           u"-pix_fmt"_s, u"yuv420p"_s, u"-c:a"_s, u"aac"_s, u"-shortest"_s, path});
    if (!process.waitForFinished(120'000) || process.exitCode() != 0) {
        QTest::qSkip("ffmpeg could not write the test video", __FILE__, __LINE__);
        return {};
    }
    return path;
}

void ChatAttachmentImportTest::guessesTheKindFromTheNameAlone_data()
{
    QTest::addColumn<QString>("path");
    QTest::addColumn<int>("kind");
    const QList<std::pair<QString, AttachmentKind>> rows{
        {u"photo.jpg"_s, AttachmentKind::Image},     {u"PHOTO.JPEG"_s, AttachmentKind::Image},
        {u"shot.png"_s, AttachmentKind::Image},      {u"clip.mp4"_s, AttachmentKind::Video},
        {u"clip.m4v"_s, AttachmentKind::Video},      {u"clip.MOV"_s, AttachmentKind::Video},
        {u"clip.webm"_s, AttachmentKind::Video},     {u"clip.mkv"_s, AttachmentKind::Video},
        {u"clip.avi"_s, AttachmentKind::Video},      {u"clip.wmv"_s, AttachmentKind::Video},
        {u"song.wav"_s, AttachmentKind::Audio},      {u"song.mp3"_s, AttachmentKind::Audio},
        {u"song.m4a"_s, AttachmentKind::Audio},      {u"song.aac"_s, AttachmentKind::Audio},
        {u"song.ogg"_s, AttachmentKind::Audio},      {u"song.oga"_s, AttachmentKind::Audio},
        {u"song.opus"_s, AttachmentKind::Audio},     {u"song.FLAC"_s, AttachmentKind::Audio},
        // Animated or unusual pictures go as they are.
        {u"funny.gif"_s, AttachmentKind::File},      {u"phone.heic"_s, AttachmentKind::File},
        {u"report.pdf"_s, AttachmentKind::File},     {u"archive.tar.gz"_s, AttachmentKind::File},
        {u"no-extension"_s, AttachmentKind::File},   {u"trailing.jpg.exe"_s, AttachmentKind::File},
        {u"/some/dir.png/readme"_s, AttachmentKind::File}, {QString(), AttachmentKind::File},
    };
    for (const auto &[path, kind] : rows)
        QTest::newRow(qPrintable(path.isEmpty() ? u"(empty)"_s : path)) << path << int(kind);
}

void ChatAttachmentImportTest::guessesTheKindFromTheNameAlone()
{
    QFETCH(QString, path);
    QFETCH(int, kind);
    QCOMPARE(int(guessAttachmentKind(path)), kind);
}

void ChatAttachmentImportTest::photosAreOnlyWhatThisComputerCanRead()
{
    // JPEG and PNG always; WebP, BMP and TIFF only with a plugin to read them,
    // or they go as files rather than failing as photos.
    const QList<QByteArray> readable = QImageReader::supportedImageFormats();
    const QStringList suffixes = photoSuffixes();
    QVERIFY(suffixes.startsWith(QStringLiteral("jpg")));
    QVERIFY(suffixes.contains(QStringLiteral("png")));
    for (const QString &suffix : {u"webp"_s, u"bmp"_s, u"tif"_s, u"tiff"_s}) {
        QCOMPARE(suffixes.contains(suffix), readable.contains(suffix.toLatin1()));
        QCOMPARE(guessAttachmentKind(u"picture."_s + suffix),
                 readable.contains(suffix.toLatin1()) ? AttachmentKind::Image : AttachmentKind::File);
    }
    QCOMPARE(photoFormatsHint().startsWith(u"JPG, PNG"_s), true);
    QCOMPARE(photoFormatsHint().contains(u"WebP"_s), readable.contains("webp"));
}

void ChatAttachmentImportTest::photoBecomesABaselineJpegWithAPreview()
{
    // A camera-sized PNG with a name no receiver should see as it is.
    const QString path = m_dir.filePath(u"beach: day 1.png"_s);
    QVERIFY(photo({3'000, 2'000}).save(path, "PNG"));
    const Outcome outcome = importFile(path);
    QVERIFY2(outcome.attachment, qPrintable(outcome.failure));
    QCOMPARE(outcome.results, 1);
    const PreparedAttachment &attachment = *outcome.attachment;
    const AttachmentDescriptor &descriptor = attachment.descriptor;

    QCOMPARE(descriptor.kind, AttachmentKind::Image);
    QCOMPARE(descriptor.width, AttachmentLimits::maxImageLongSide);
    QCOMPARE(descriptor.height, 1'365);
    QCOMPARE(descriptor.byteCount, attachment.blob.size());
    QVERIFY(descriptor.byteCount <= AttachmentLimits::maxImageBytes);
    QCOMPARE(descriptor.sha256, pageMediaHash(attachment.blob));
    QCOMPARE(descriptor.partCount, attachmentPartCount(descriptor.byteCount));
    QCOMPARE(descriptor.mimeType, u"image/jpeg"_s);
    QCOMPARE(descriptor.fileName, u"beach_ day 1.jpg"_s);
    QCOMPARE(descriptor.durationMs, qint64(0));
    QVERIFY(descriptor.peaks.isEmpty());
    // The sender mints these.
    QVERIFY(descriptor.key.isEmpty());
    QVERIFY(attachment.notice.isEmpty());

    // Baseline: one scan, whatever the source was; exactly the size it says.
    QCOMPARE(jpegScanCount(attachment.blob), 1);
    QCOMPARE(jpegFrameSize(attachment.blob), std::make_optional(std::pair(2'048, 1'365)));
    QCOMPARE(QImage::fromData(attachment.blob, "JPEG").size(), QSize(2'048, 1'365));
    QVERIFY(receiversAccept(attachment));

    // The preview: 320 px on the long side, small enough for its frame.
    QVERIFY(descriptor.hasPreview);
    QVERIFY(previewIsAcceptable(attachment.preview));
    QCOMPARE(jpegFrameSize(attachment.preview), std::make_optional(std::pair(320, 213)));
    QCOMPARE(jpegScanCount(attachment.preview), 1);

    QVERIFY(progressIsOrderly(outcome.progress));
}

void ChatAttachmentImportTest::smallPhotoKeepsItsSizeOnWhite()
{
    // Never enlarged, and transparency lands on white (a JPEG has no alpha).
    QImage clear(40, 30, QImage::Format_ARGB32);
    clear.fill(Qt::transparent);
    const QString path = m_dir.filePath(u"clear.png"_s);
    QVERIFY(clear.save(path, "PNG"));
    const Outcome outcome = importFile(path);
    QVERIFY2(outcome.attachment, qPrintable(outcome.failure));
    const PreparedAttachment &attachment = *outcome.attachment;
    QCOMPARE(attachment.descriptor.width, 40);
    QCOMPARE(attachment.descriptor.height, 30);
    const QImage decoded = QImage::fromData(attachment.blob, "JPEG");
    QCOMPARE(decoded.size(), QSize(40, 30));
    QVERIFY(qGray(decoded.pixel(20, 15)) >= 250);
    QVERIFY(previewIsAcceptable(attachment.preview));
    QCOMPARE(jpegFrameSize(attachment.preview), std::make_optional(std::pair(40, 30)));
    QVERIFY(receiversAccept(attachment));

    // A single pixel is still a photo.
    const QString dot = m_dir.filePath(u"dot.bmp"_s);
    QVERIFY(photo({1, 1}).save(dot, "BMP"));
    const Outcome tiny = importFile(dot);
    QVERIFY2(tiny.attachment, qPrintable(tiny.failure));
    QCOMPARE(tiny.attachment->descriptor.kind, AttachmentKind::Image);
    QCOMPARE(tiny.attachment->descriptor.width, 1);
    QCOMPARE(tiny.attachment->descriptor.fileName, u"dot.jpg"_s);
}

void ChatAttachmentImportTest::pastedPictureBecomesAPhoto()
{
    const QImage picture = photo({800, 600});
    const Outcome outcome = importWith(
        [&](ChatAttachmentImporter &importer) {
            importer.startImage(picture, u"Screenshot 2026-09-25"_s);
            QCOMPARE(importer.kind(), AttachmentKind::Image);
            QVERIFY(importer.busy());
        });
    QVERIFY2(outcome.attachment, qPrintable(outcome.failure));
    QCOMPARE(outcome.attachment->descriptor.kind, AttachmentKind::Image);
    QCOMPARE(outcome.attachment->descriptor.fileName, u"Screenshot 2026-09-25.jpg"_s);
    QCOMPARE(outcome.attachment->descriptor.width, 800);
    QCOMPARE(outcome.attachment->descriptor.height, 600);
    QVERIFY(outcome.attachment->descriptor.hasPreview);
    QVERIFY(receiversAccept(*outcome.attachment));

    // A name that already says JPEG is not said twice; no name still is one.
    const Outcome named =
        importWith([&](ChatAttachmentImporter &importer) { importer.startImage(picture, u"cat.JPEG"_s); });
    QVERIFY(named.attachment);
    QCOMPARE(named.attachment->descriptor.fileName, u"cat.jpg"_s);
    const Outcome unnamed =
        importWith([&](ChatAttachmentImporter &importer) { importer.startImage(picture, u"  "_s); });
    QVERIFY(unnamed.attachment);
    QCOMPARE(unnamed.attachment->descriptor.fileName, u"Pasted picture.jpg"_s);

    // Nothing to paste: there is no file to fall back on.
    const Outcome nothing =
        importWith([](ChatAttachmentImporter &importer) { importer.startImage(QImage(), u"empty"_s); });
    QVERIFY(!nothing.attachment);
    QVERIFY(!nothing.failure.isEmpty());
    QCOMPARE(nothing.results, 1);
}

void ChatAttachmentImportTest::unreadablePhotoIsSentAsAFile()
{
    const QByteArray bytes = randomBytes(5'000, 3);
    const QString path = writeFile(u"broken.png"_s, bytes);
    const Outcome outcome = importWith([&](ChatAttachmentImporter &importer) {
        importer.start(path);
        QCOMPARE(importer.kind(), AttachmentKind::Image);
    });
    QVERIFY2(outcome.attachment, qPrintable(outcome.failure));
    const PreparedAttachment &attachment = *outcome.attachment;
    QCOMPARE(attachment.descriptor.kind, AttachmentKind::File);
    QCOMPARE(attachment.blob, bytes);
    QCOMPARE(attachment.descriptor.mimeType, u"image/png"_s);
    QCOMPARE(attachment.descriptor.fileName, u"broken.png"_s);
    QVERIFY(!attachment.descriptor.hasPreview);
    QVERIFY(attachment.preview.isEmpty());
    QCOMPARE(attachment.notice, u"Couldn't prepare this as a photo — it will be sent as a file."_s);
    QVERIFY(receiversAccept(attachment));
    QVERIFY(progressIsOrderly(outcome.progress));

    // The importer says what it became.
    ChatAttachmentImporter importer;
    QSignalSpy finished(&importer, &ChatAttachmentImporter::finished);
    importer.start(path);
    QVERIFY(finished.wait(30'000));
    QCOMPARE(importer.kind(), AttachmentKind::File);
}

void ChatAttachmentImportTest::unreadablePhotoTooLargeForAFileIsRefused()
{
    // Not a picture, and too large to go as a file: the card says why the
    // photo failed, which is what the user can act on.
    const QString path = writeFile(u"huge.png"_s, randomBytes(AttachmentLimits::maxFileBytes + 1'000, 4));
    const Outcome outcome = importFile(path);
    QVERIFY(!outcome.attachment);
    QCOMPARE(outcome.failure, u"That file is not a picture OpenChat can read."_s);
    QCOMPARE(outcome.results, 1);
}

void ChatAttachmentImportTest::audioBecomesOneSongWithAWaveform()
{
    const QString path = writeWav(u"voice note.wav"_s, tone(44'100, 2, 3.0));
    const Outcome outcome = importFile(path);
    QVERIFY2(outcome.attachment, qPrintable(outcome.failure));
    const PreparedAttachment &attachment = *outcome.attachment;
    const AttachmentDescriptor &descriptor = attachment.descriptor;
    QCOMPARE(descriptor.kind, AttachmentKind::Audio);
    QCOMPARE(descriptor.durationMs, qint64(3'000));
    QCOMPARE(descriptor.fileName, u"voice note.wav"_s);
    QVERIFY(descriptor.mimeType.isEmpty()); // OpenChat's own container, not a WAV
    QCOMPARE(descriptor.width, 0);
    QVERIFY(!descriptor.hasPreview);
    QCOMPARE(descriptor.peaks.size(), AttachmentLimits::maxPeaks);
    QVERIFY(std::all_of(descriptor.peaks.cbegin(), descriptor.peaks.cend(),
                        [](char peak) { return quint8(peak) >= 250; }));
    QVERIFY(attachment.notice.isEmpty());
    const std::optional<SongContainer> song = decodeSongContainer(attachment.blob, chatSongLimits());
    QVERIFY(song);
    QCOMPARE(song->channels, 2);
    QCOMPARE(song->durationMs(), qint64(3'000));
    QVERIFY(receiversAccept(attachment));
    QVERIFY(progressIsOrderly(outcome.progress));

    // A voice note of under a second passes too.
    const Outcome blip = importFile(writeWav(u"blip.wav"_s, tone(16'000, 1, 0.5)));
    QVERIFY2(blip.attachment, qPrintable(blip.failure));
    QCOMPARE(blip.attachment->descriptor.kind, AttachmentKind::Audio);
    QCOMPARE(blip.attachment->descriptor.durationMs, qint64(500));
}

void ChatAttachmentImportTest::longAudioKeepsTheFirstFiveMinutes()
{
    const QString path = writeWav(u"lecture.wav"_s, tone(8'000, 1, 320.0));
    const Outcome outcome = importFile(path, 180'000);
    QVERIFY2(outcome.attachment, qPrintable(outcome.failure));
    const PreparedAttachment &attachment = *outcome.attachment;
    QCOMPARE(attachment.descriptor.kind, AttachmentKind::Audio);
    QCOMPARE(attachment.descriptor.durationMs, AttachmentLimits::maxAudioMs);
    QCOMPARE(attachment.notice, u"Only the first 5 minutes will be sent."_s);
    QVERIFY(attachment.blob.size() <= AttachmentLimits::maxAudioBytes);
    QVERIFY(receiversAccept(attachment));

    // Seconds of work, reported along the way at most ten times a second:
    // the one exception is the last, which comes the moment it is true.
    QVERIFY(progressIsOrderly(outcome.progress));
    QVERIFY2(outcome.progress.size() >= 6, qPrintable(QString::number(outcome.progress.size())));
    for (qsizetype i = 1; i + 1 < outcome.progress.size(); ++i) {
        const qint64 gap = outcome.progressAtMs.at(i) - outcome.progressAtMs.at(i - 1);
        QVERIFY2(gap >= 90, qPrintable(QString::number(gap)));
    }
}

void ChatAttachmentImportTest::silentAudioIsSentAsAFile()
{
    WavAudio silence;
    silence.sampleRate = 22'050;
    silence.channels = 1;
    silence.samples.resize(22'050 * 2);
    const QString path = writeWav(u"room tone.wav"_s, silence);
    const Outcome outcome = importFile(path);
    QVERIFY2(outcome.attachment, qPrintable(outcome.failure));
    QCOMPARE(outcome.attachment->descriptor.kind, AttachmentKind::File);
    QCOMPARE(outcome.attachment->notice, u"Couldn't prepare this as audio — it will be sent as a file."_s);
    QVERIFY2(outcome.attachment->descriptor.mimeType.startsWith(u"audio/"_s),
             qPrintable(outcome.attachment->descriptor.mimeType));
    QVERIFY(outcome.attachment->descriptor.peaks.isEmpty());
    QVERIFY(receiversAccept(*outcome.attachment));
}

void ChatAttachmentImportTest::videoBecomesASequenceWithAPoster()
{
    const QString path = writeVideo(u"holiday.mp4"_s, 7, u"640x360"_s);
    if (path.isEmpty())
        return; // skipped
    const Outcome outcome = importFile(path, 120'000);
    if (!outcome.attachment && outcome.failure.contains(u"can't be opened"_s))
        QSKIP("No Qt Multimedia backend reads MP4 here");
    QVERIFY2(outcome.attachment, qPrintable(outcome.failure));
    const PreparedAttachment &attachment = *outcome.attachment;
    const AttachmentDescriptor &descriptor = attachment.descriptor;
    QCOMPARE(descriptor.kind, AttachmentKind::Video);
    QCOMPARE(descriptor.width, 480);
    QCOMPARE(descriptor.height, 270);
    QCOMPARE(descriptor.durationMs, qint64(7'000));
    QCOMPARE(descriptor.fileName, u"holiday.mp4"_s);
    QVERIFY(descriptor.mimeType.isEmpty());
    QVERIFY(attachment.notice.isEmpty());
    const std::optional<QVector<QByteArray>> segments = decodeVideoSequence(attachment.blob);
    QVERIFY(segments);
    QCOMPARE(segments->size(), 2);
    QVector<ClipContainer> clips;
    for (const QByteArray &segment : *segments)
        clips.push_back(*decodeClipContainer(segment));
    QVERIFY(clipSoundtrack(clips, chatSongLimits())); // the sine came along
    QVERIFY(descriptor.hasPreview);
    QVERIFY(previewIsAcceptable(attachment.preview));
    QCOMPARE(jpegFrameSize(attachment.preview), std::make_optional(std::pair(320, 180)));
    QVERIFY(receiversAccept(attachment));
    QVERIFY(progressIsOrderly(outcome.progress));

    // Cancelled part way, a video says nothing at all.
    ChatAttachmentImporter importer;
    QSignalSpy finished(&importer, &ChatAttachmentImporter::finished);
    QSignalSpy failed(&importer, &ChatAttachmentImporter::failed);
    importer.start(path);
    QTest::qWait(300);
    importer.cancel();
    QVERIFY(!importer.busy());
    QTest::qWait(3'000);
    QCOMPARE(finished.count() + failed.count(), 0);
}

void ChatAttachmentImportTest::longVideoKeepsTheFirstMinute()
{
    const QString path = writeVideo(u"long.mp4"_s, 65, u"320x180"_s);
    if (path.isEmpty())
        return; // skipped
    const Outcome outcome = importFile(path, 240'000);
    if (!outcome.attachment && outcome.failure.contains(u"can't be opened"_s))
        QSKIP("No Qt Multimedia backend reads MP4 here");
    QVERIFY2(outcome.attachment, qPrintable(outcome.failure));
    const PreparedAttachment &attachment = *outcome.attachment;
    QCOMPARE(attachment.descriptor.kind, AttachmentKind::Video);
    QCOMPARE(attachment.descriptor.durationMs, AttachmentLimits::maxVideoMs);
    QCOMPARE(attachment.notice, u"Only the first minute will be sent."_s);
    QCOMPARE(attachment.descriptor.width, 320);
    QCOMPARE(attachment.descriptor.height, 180);
    const std::optional<QVector<QByteArray>> segments = decodeVideoSequence(attachment.blob);
    QVERIFY(segments);
    QCOMPARE(segments->size(), 12);
    QVERIFY(attachment.blob.size() <= AttachmentLimits::maxVideoBytes);
    QVERIFY(receiversAccept(attachment));
    // A minute of sound: more than a profile song may hold, all of it kept.
    QVector<ClipContainer> clips;
    for (const QByteArray &segment : *segments)
        clips.push_back(*decodeClipContainer(segment));
    QVERIFY(!clipSoundtrack(clips));
    const auto soundtrack = clipSoundtrack(clips, chatSongLimits());
    QVERIFY(soundtrack);
    QCOMPARE(soundtrack->durationMs(), AttachmentLimits::maxVideoMs);
}

void ChatAttachmentImportTest::brokenVideoIsSentAsAFile()
{
    const QByteArray bytes = randomBytes(64 * 1'024, 5);
    const QString path = writeFile(u"broken.mp4"_s, bytes);
    const Outcome outcome = importFile(path, 60'000);
    QVERIFY2(outcome.attachment, qPrintable(outcome.failure));
    QCOMPARE(outcome.attachment->descriptor.kind, AttachmentKind::File);
    QCOMPARE(outcome.attachment->blob, bytes);
    QCOMPARE(outcome.attachment->descriptor.mimeType, u"video/mp4"_s);
    QCOMPARE(outcome.attachment->notice, u"Couldn't prepare this as a video — it will be sent as a file."_s);
    QVERIFY(receiversAccept(*outcome.attachment));
}

void ChatAttachmentImportTest::fileIsSentAsItIs()
{
    const Outcome text = importFile(writeFile(u"notes.txt"_s, "hello\n"));
    QVERIFY2(text.attachment, qPrintable(text.failure));
    const AttachmentDescriptor &descriptor = text.attachment->descriptor;
    QCOMPARE(descriptor.kind, AttachmentKind::File);
    QCOMPARE(text.attachment->blob, QByteArray("hello\n"));
    QCOMPARE(descriptor.byteCount, qint64(6));
    QCOMPARE(descriptor.partCount, 1);
    QCOMPARE(descriptor.mimeType, u"text/plain"_s);
    QCOMPARE(descriptor.fileName, u"notes.txt"_s);
    QCOMPARE(descriptor.sha256, pageMediaHash("hello\n"));
    QVERIFY(text.attachment->notice.isEmpty());
    QVERIFY(receiversAccept(*text.attachment));
    QVERIFY(progressIsOrderly(text.progress));

    // Bytes OpenChat knows nothing of, over several parts; the type comes
    // from the name, never from what is inside.
    const QByteArray binary = randomBytes(500'000, 6);
    const Outcome data = importFile(writeFile(u"data.bin"_s, binary));
    QVERIFY(data.attachment);
    QCOMPARE(data.attachment->blob, binary);
    QCOMPARE(data.attachment->descriptor.partCount, 3);
    QCOMPARE(data.attachment->descriptor.mimeType, u"application/octet-stream"_s);
    const Outcome disguised = importFile(writeFile(u"picture-inside.txt"_s, QByteArray("\x89PNG\r\n\x1a\n", 8)));
    QVERIFY(disguised.attachment);
    QCOMPARE(disguised.attachment->descriptor.mimeType, u"text/plain"_s);

    // A name a receiver's file system could trip on is cleaned here too.
    const Outcome odd = importFile(writeFile(u"report<final>?.txt"_s, "x"));
    QVERIFY(odd.attachment);
    QCOMPARE(odd.attachment->descriptor.fileName, u"report_final__.txt"_s);
    QVERIFY(receiversAccept(*odd.attachment));
}

void ChatAttachmentImportTest::filesOverSixteenMegabytesAreRefused()
{
    // Exactly the cap goes, in exactly the most parts there are.
    const QString exact = m_dir.filePath(u"exact.bin"_s);
    {
        QFile file(exact);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.resize(AttachmentLimits::maxFileBytes));
    }
    const Outcome fits = importFile(exact);
    QVERIFY2(fits.attachment, qPrintable(fits.failure));
    QCOMPARE(fits.attachment->descriptor.byteCount, AttachmentLimits::maxFileBytes);
    QCOMPARE(fits.attachment->descriptor.partCount, AttachmentLimits::maxParts);
    QVERIFY(receiversAccept(*fits.attachment));

    // One byte more does not.
    const QString over = m_dir.filePath(u"over.bin"_s);
    {
        QFile file(over);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.resize(AttachmentLimits::maxFileBytes + 1));
    }
    const Outcome refused = importFile(over);
    QVERIFY(!refused.attachment);
    QCOMPARE(refused.failure, u"Files up to 16 MB can be sent."_s);
    QCOMPARE(refused.results, 1);
}

void ChatAttachmentImportTest::emptyMissingAndFolderPathsAreRefused()
{
    const Outcome empty = importFile(writeFile(u"empty.txt"_s, QByteArray()));
    QVERIFY(!empty.attachment);
    QCOMPARE(empty.failure, u"That file is empty."_s);

    for (const QString &path : {m_dir.filePath(u"not-there.txt"_s), m_dir.filePath(u"not-there.png"_s),
                                m_dir.filePath(u"not-there.wav"_s), m_dir.path()}) {
        const Outcome missing = importFile(path);
        QVERIFY(!missing.attachment);
        QCOMPARE(missing.failure, u"That file can't be opened."_s);
        QCOMPARE(missing.results, 1);
    }
}

void ChatAttachmentImportTest::cancelStopsAJobWithoutAWord()
{
    const QString path = writeWav(u"cancelled.wav"_s, tone(8'000, 1, 200.0));
    ChatAttachmentImporter importer;
    QSignalSpy finished(&importer, &ChatAttachmentImporter::finished);
    QSignalSpy failed(&importer, &ChatAttachmentImporter::failed);
    QSignalSpy progress(&importer, &ChatAttachmentImporter::progressChanged);
    importer.start(path);
    QVERIFY(importer.busy());
    QCOMPARE(importer.kind(), AttachmentKind::Audio);
    QTRY_VERIFY(progress.count() >= 2); // under way
    importer.cancel();
    QVERIFY(!importer.busy());
    const qsizetype reported = progress.count();
    QTest::qWait(3'000);
    QCOMPARE(finished.count(), 0);
    QCOMPARE(failed.count(), 0);
    QCOMPARE(progress.count(), reported);
    importer.cancel(); // twice is harmless

    // The importer works on.
    importer.start(writeFile(u"after.txt"_s, "after"));
    QVERIFY(finished.wait(30'000));
    QCOMPARE(finished.at(0).at(0).value<PreparedAttachment>().blob, QByteArray("after"));
    QCOMPARE(failed.count(), 0);
}

void ChatAttachmentImportTest::aNewStartReplacesTheJobUnderWay()
{
    const QString photoPath = m_dir.filePath(u"replaced.png"_s);
    QVERIFY(photo({2'500, 1'800}).save(photoPath, "PNG"));
    ChatAttachmentImporter importer;
    QSignalSpy finished(&importer, &ChatAttachmentImporter::finished);
    QSignalSpy failed(&importer, &ChatAttachmentImporter::failed);
    importer.start(photoPath);
    importer.start(writeWav(u"replacing.wav"_s, tone(22'050, 1, 1.0)));
    QCOMPARE(importer.kind(), AttachmentKind::Audio);
    QVERIFY(finished.wait(60'000));
    QTest::qWait(1'000);
    QCOMPARE(finished.count(), 1);
    QCOMPARE(failed.count(), 0);
    QCOMPARE(finished.at(0).at(0).value<PreparedAttachment>().descriptor.kind, AttachmentKind::Audio);
}

void ChatAttachmentImportTest::videoSupportFollowsTheCodec()
{
    QCOMPARE(ChatAttachmentImporter::videoSupported(), clipCodecAvailable());
}

QTEST_MAIN(ChatAttachmentImportTest)
#include "tst_chatattachmentimport.moc"
