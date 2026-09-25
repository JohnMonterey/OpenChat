#include "profile/ChatAttachmentImport.h"

#include "domain/ProfilePageCodec.h"
#include "domain/SongContainer.h"
#include "media/SongCodec.h"
#include "profile/ClipCodec.h"
#include "profile/ClipImport.h"
#include "profile/ProfileBackgroundImage.h"
#include "profile/SongImport.h"

#include <QBuffer>
#include <QColor>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QMimeDatabase>
#include <QMutex>
#include <QMutexLocker>
#include <QStringList>
#include <QThread>
#include <QThreadPool>
#include <QTimer>

#include <algorithm>
#include <atomic>

namespace OpenChat {

namespace {

// At most ten progress updates a second reach the card.
constexpr int progressIntervalMs = 100;
// A file is read in pieces this large, so a cancel is heard and progress moves.
constexpr qint64 fileChunkBytes = 1024 * 1024;
// A preview is tried at each long side and each quality in turn until one
// fits AttachmentLimits::maxPreviewBytes.
constexpr int previewSides[] = {AttachmentLimits::maxPreviewSide, 240, 160};
constexpr int previewQualities[] = {72, 62, 52, 42, 32};
// Where each kind's conversion ends and its checks and hashing begin.
constexpr qreal imageStageEnd = 0.9;
constexpr qreal videoStageEnd = 0.95;
constexpr qreal audioStageEnd = 0.97;

// A compressed audio file is read only as far as the attachment reaches, so
// even a long one may be picked; a WAV is read only that far too.
constexpr qint64 maxAudioSourceBytes = 1024LL * 1024 * 1024;
// A five-minute decode on a slow machine, with room to spare.
constexpr int audioDecodeTimeoutMs = 120'000;

// The heavy work of every chat import (photo re-encodes, video segments,
// previews, hashing) runs here: its own pool, at low priority and two
// threads at most, so the pictures a chat shows (decoded on the global pool)
// and the window never wait behind a video. It is waited for at exit, when
// every importer has already cancelled what it had running.
class ImportPool final : public QThreadPool
{
public:
    ImportPool()
    {
        setMaxThreadCount(std::clamp(QThread::idealThreadCount() / 2, 1, 2));
        setThreadPriority(QThread::LowPriority);
    }
};

Q_GLOBAL_STATIC(ImportPool, importPool)

[[nodiscard]] QString couldNotOpenText()
{
    return QStringLiteral("That file can't be opened.");
}

[[nodiscard]] QString fileTooLargeText()
{
    return QStringLiteral("Files up to 16 MB can be sent.");
}

[[nodiscard]] QString fallbackNotice(AttachmentKind kind)
{
    switch (kind) {
    case AttachmentKind::Image:
        return QStringLiteral("Couldn't prepare this as a photo — it will be sent as a file.");
    case AttachmentKind::Video:
        return QStringLiteral("Couldn't prepare this as a video — it will be sent as a file.");
    case AttachmentKind::Audio:
        return QStringLiteral("Couldn't prepare this as audio — it will be sent as a file.");
    case AttachmentKind::File:
        break;
    }
    return {};
}

// A song import's error as the attachment card says it (songImportErrorText
// speaks of profile songs).
[[nodiscard]] QString audioErrorText(SongImportError error)
{
    switch (error) {
    case SongImportError::FileMissing:
        return couldNotOpenText();
    case SongImportError::FileTooLarge:
        return QStringLiteral("That audio file is too large to send.");
    case SongImportError::UnsupportedFormat:
        return QStringLiteral("OpenChat can't read this kind of audio here.");
    case SongImportError::DecoderUnavailable:
        return QStringLiteral("This computer can only prepare WAV audio.");
    case SongImportError::DecodeFailed:
        return QStringLiteral("That file is damaged or holds no sound.");
    case SongImportError::Silent:
        return QStringLiteral("That recording is silent.");
    case SongImportError::TooShort:
        return QStringLiteral("That recording is too short to send.");
    case SongImportError::EncodeFailed:
        return QStringLiteral("That audio couldn't be prepared.");
    case SongImportError::TimedOut:
        return QStringLiteral("Reading that file took too long.");
    }
    return QStringLiteral("That audio couldn't be prepared.");
}

[[nodiscard]] ClipEncodeOptions chatClipOptions()
{
    ClipEncodeOptions options;
    options.maxDurationMs = AttachmentLimits::maxVideoMs;
    return options;
}

[[nodiscard]] ClipImporter::Limits chatClipLimits()
{
    ClipImporter::Limits limits;
    limits.maxQueuedSegments = 2;
    return limits;
}

[[nodiscard]] SongImportLimits chatSongImportLimits()
{
    SongImportLimits limits;
    limits.maxFileBytes = maxAudioSourceBytes;
    limits.timeoutMs = audioDecodeTimeoutMs;
    limits.peakBuckets = AttachmentLimits::maxPeaks;
    return limits;
}

// A picture decoded at no more than `side` on its long edge (JPEG scales
// while it decodes), for a preview.
[[nodiscard]] QImage decodeForPreview(const QByteArray &jpeg, int side)
{
    QBuffer buffer;
    buffer.setData(jpeg);
    if (!buffer.open(QIODevice::ReadOnly))
        return {};
    QImageReader reader(&buffer, "jpeg");
    reader.setAllocationLimit(64);
    const QSize size = reader.size();
    if (!size.isValid() || size.isEmpty() || size.width() > AttachmentLimits::maxImageDimension
        || size.height() > AttachmentLimits::maxImageDimension)
        return {};
    if (std::max(size.width(), size.height()) > side)
        reader.setScaledSize(size.scaled(side, side, Qt::KeepAspectRatio).expandedTo(QSize(1, 1)));
    return reader.read();
}

// The preview a receiver shows while the rest arrives: a baseline JPEG of
// at most maxPreviewSide within maxPreviewBytes, exactly what
// previewIsAcceptable() lets through; empty when nothing fits.
[[nodiscard]] QByteArray makePreview(const QImage &source)
{
    if (source.isNull())
        return {};
    const QImage flat = source.convertToFormat(QImage::Format_RGB32);
    const int longSide = std::max(flat.width(), flat.height());
    for (const int side : previewSides) {
        const QImage scaled =
            longSide <= side ? flat : flat.scaled(side, side, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        if (scaled.isNull())
            continue;
        for (const int quality : previewQualities) {
            const QByteArray jpeg = encodeBaselineJpeg(scaled, quality);
            if (!jpeg.isEmpty() && previewIsAcceptable(jpeg))
                return jpeg;
        }
    }
    return {};
}

// `blob` as `kind`: the descriptor filled in for it (all but the id and key,
// which the sender mints), checked the way every receiver will check it, so
// nothing leaves that a receiver would refuse.
[[nodiscard]] std::optional<PreparedAttachment> prepared(AttachmentKind kind, QByteArray blob, QByteArray preview,
                                                         const QString &fileName, const QString &mimeType)
{
    if (blob.isEmpty() || blob.size() > attachmentByteCap(kind))
        return std::nullopt;
    PreparedAttachment attachment;
    AttachmentDescriptor &descriptor = attachment.descriptor;
    descriptor.kind = kind;
    descriptor.byteCount = blob.size();
    descriptor.sha256 = pageMediaHash(blob);
    descriptor.partCount = attachmentPartCount(descriptor.byteCount);
    descriptor.fileName = sanitizeAttachmentFileName(fileName);
    descriptor.mimeType = isValidMimeType(mimeType) ? mimeType : QString();
    if (!preview.isEmpty() && !previewIsAcceptable(preview))
        preview.clear();
    descriptor.hasPreview = !preview.isEmpty();
    attachment.blob = std::move(blob);
    attachment.preview = std::move(preview);
    return attachment;
}

// Whether every receiver will take `attachment` once the sender has minted
// its key: the descriptor's own rules, then the blob against it.
[[nodiscard]] bool receiversAccept(const PreparedAttachment &attachment)
{
    AttachmentDescriptor keyed = attachment.descriptor;
    keyed.key = QByteArray(AttachmentLimits::keyBytes, '\0');
    return attachmentBlobIsValid(keyed, attachment.blob);
}

} // namespace

QStringList photoSuffixes()
{
    static const QStringList suffixes = [] {
        const QList<QByteArray> readable = QImageReader::supportedImageFormats();
        QStringList offered;
        for (const char *suffix : {"jpg", "jpeg", "png", "webp", "bmp", "tif", "tiff"}) {
            if (readable.contains(QByteArray(suffix)))
                offered.append(QString::fromLatin1(suffix));
        }
        return offered;
    }();
    return suffixes;
}

QString photoFormatsHint()
{
    const QStringList suffixes = photoSuffixes();
    QStringList names{QStringLiteral("JPG"), QStringLiteral("PNG")};
    if (suffixes.contains(QStringLiteral("webp")))
        names.append(QStringLiteral("WebP"));
    return names.join(QStringLiteral(", "));
}

AttachmentKind guessAttachmentKind(const QString &path)
{
    const QStringList images = photoSuffixes();
    static const QStringList videos{QStringLiteral("mp4"), QStringLiteral("m4v"), QStringLiteral("mov"),
                                    QStringLiteral("webm"), QStringLiteral("mkv"), QStringLiteral("avi"),
                                    QStringLiteral("wmv")};
    static const QStringList audio{QStringLiteral("wav"), QStringLiteral("mp3"), QStringLiteral("m4a"),
                                   QStringLiteral("aac"), QStringLiteral("ogg"), QStringLiteral("oga"),
                                   QStringLiteral("opus"), QStringLiteral("flac")};
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (images.contains(suffix))
        return AttachmentKind::Image;
    if (videos.contains(suffix))
        return AttachmentKind::Video;
    if (audio.contains(suffix))
        return AttachmentKind::Audio;
    return AttachmentKind::File;
}

// ---------------------------------------------------------------------------
// Private: the job under way. Work on the pool reaches it only through a
// Job, under the Job's mutex, so work that was cancelled (or outlived its
// importer) posts nothing; results come back on the importer's thread.
// ---------------------------------------------------------------------------

class ChatAttachmentImporter::Private final
{
public:
    struct Job final {
        QMutex mutex;
        Private *owner = nullptr;
        std::atomic_bool cancelled{false};
    };

    explicit Private(ChatAttachmentImporter *importer)
        : q(importer)
    {
        m_flush.setSingleShot(true);
        QObject::connect(&m_flush, &QTimer::timeout, q, [this] { emitProgress(); });
    }

    // Runs `apply` on the importer's thread, if it still wants `job`.
    template<typename Apply>
    static void post(const std::shared_ptr<Job> &job, Apply apply)
    {
        QMutexLocker locker(&job->mutex);
        Private *owner = job->owner;
        if (owner == nullptr || job->cancelled)
            return;
        QMetaObject::invokeMethod(
            owner->q,
            [owner, job, apply] {
                if (owner->m_job == job)
                    apply(owner);
            },
            Qt::QueuedConnection);
    }

    // Everything a new start() or cancel() leaves behind: the work on the
    // pool, the video or audio importer, a progress update still pending.
    void stop()
    {
        if (m_job) {
            QMutexLocker locker(&m_job->mutex);
            m_job->cancelled = true;
            m_job->owner = nullptr;
        }
        m_job.reset();
        retire(m_clips);
        retire(m_songs);
        m_flush.stop();
        m_busy = false;
    }

    void begin(AttachmentKind kind, const QString &path)
    {
        stop();
        m_busy = true;
        m_kind = kind;
        m_path = path;
        m_progress = 0.0;
        m_reported = 0.0;
        m_sinceReport.start();
        emit q->progressChanged(0.0);
    }

    void startFile(const QString &path)
    {
        begin(AttachmentKind::File, path);
        runFile(QString(), fileTooLargeText(), 0.0);
    }

    void startImageFile(const QString &path)
    {
        begin(AttachmentKind::Image, path);
        const std::shared_ptr<Job> job = newJob();
        const QString name = QFileInfo(path).completeBaseName() + QStringLiteral(".jpg");
        importPool()->start([job, path, name] {
            std::optional<ProfileImageError> error;
            try {
                auto processed = processProfileBackgroundFile(
                    path, QColor(Qt::white), chatPhotoLimits(),
                    [job](qreal done) { post(job, [done](Private *d) { d->report(imageStageEnd * done); }); },
                    [job] { return job->cancelled.load(); });
                if (!processed) {
                    error = processed.error();
                } else if (std::optional<PreparedAttachment> photo = preparedPhoto(processed.value(), name)) {
                    post(job, [attachment = std::move(*photo)](Private *d) { d->succeed(attachment); });
                    return;
                } else {
                    error = ProfileImageError::EncodeFailed;
                }
            } catch (...) {
                error = ProfileImageError::EncodeFailed;
            }
            post(job, [error = *error](Private *d) {
                // A missing file is missing as a file too; anything else
                // may still go as one.
                if (error == ProfileImageError::FileMissing)
                    d->fail(chatPhotoErrorText(error));
                else
                    d->fallBackToFile(chatPhotoErrorText(error));
            });
        });
    }

    void startImage(const QImage &image, const QString &name)
    {
        begin(AttachmentKind::Image, QString());
        QString base = name.trimmed();
        for (const QString &suffix : {QStringLiteral(".jpg"), QStringLiteral(".jpeg")}) {
            if (base.endsWith(suffix, Qt::CaseInsensitive))
                base.chop(suffix.size());
        }
        if (base.trimmed().isEmpty())
            base = QStringLiteral("Pasted picture");
        const QString fileName = base + QStringLiteral(".jpg");
        const std::shared_ptr<Job> job = newJob();
        importPool()->start([job, image, fileName] {
            QString failure;
            try {
                auto processed = processProfileBackground(
                    image, QColor(Qt::white), chatPhotoLimits(),
                    [job](qreal done) { post(job, [done](Private *d) { d->report(imageStageEnd * done); }); },
                    [job] { return job->cancelled.load(); });
                if (!processed) {
                    failure = chatPhotoErrorText(processed.error());
                } else if (std::optional<PreparedAttachment> photo = preparedPhoto(processed.value(), fileName)) {
                    post(job, [attachment = std::move(*photo)](Private *d) { d->succeed(attachment); });
                    return;
                } else {
                    failure = chatPhotoErrorText(ProfileImageError::EncodeFailed);
                }
            } catch (...) {
                failure = chatPhotoErrorText(ProfileImageError::EncodeFailed);
            }
            post(job, [failure](Private *d) { d->fail(failure); });
        });
    }

    void startVideo(const QString &path)
    {
        begin(AttachmentKind::Video, path);
        m_clips = std::make_unique<ClipImporter>(chatClipOptions(), chatClipLimits());
        m_clips->setThreadPool(importPool());
        QObject::connect(m_clips.get(), &ClipImporter::progressChanged, q,
                         [this](qreal done) { report(videoStageEnd * done); });
        QObject::connect(m_clips.get(), &ClipImporter::finished, q,
                         [this](const ImportedClip &clip) { videoImported(clip); });
        QObject::connect(m_clips.get(), &ClipImporter::failed, q,
                         [this](const QString &message) { fallBackToFile(message); });
        m_clips->start(path);
    }

    void startAudio(const QString &path)
    {
        begin(AttachmentKind::Audio, path);
        m_songs = std::make_unique<SongImporter>(chatSongImportLimits());
        m_songs->setWorkerPriority(QThread::LowPriority);
        QObject::connect(m_songs.get(), &SongImporter::progressChanged, q,
                         [this](qreal done) { report(audioStageEnd * done); });
        QObject::connect(m_songs.get(), &SongImporter::encodedWhole, q,
                         [this](const EncodedSong &song) { audioEncoded(song); });
        QObject::connect(m_songs.get(), &SongImporter::failed, q,
                         [this](SongImportError error, const QString &) { fallBackToFile(audioErrorText(error)); });
        m_songs->encodeWhole(path, chatSongEncodeOptions(), AttachmentLimits::maxPeaks);
    }

    ChatAttachmentImporter *const q;
    AttachmentKind m_kind = AttachmentKind::File;
    bool m_busy = false;

private:
    template<typename Importer>
    static void retire(std::unique_ptr<Importer> &importer)
    {
        if (!importer)
            return;
        // Later, never inside one of its own signals; nothing it still
        // sends reaches this importer.
        importer->disconnect();
        importer->cancel();
        importer.release()->deleteLater();
    }

    [[nodiscard]] std::shared_ptr<Job> newJob()
    {
        auto job = std::make_shared<Job>();
        job->owner = this;
        m_job = job;
        return job;
    }

    // A re-encoded photo with its preview, from the JPEG itself so the two
    // always agree. On the pool.
    [[nodiscard]] static std::optional<PreparedAttachment> preparedPhoto(const ProcessedBackground &photo,
                                                                         const QString &fileName)
    {
        QByteArray preview = makePreview(decodeForPreview(photo.jpeg, 2 * AttachmentLimits::maxPreviewSide));
        std::optional<PreparedAttachment> attachment =
            prepared(AttachmentKind::Image, photo.jpeg, std::move(preview), fileName, QStringLiteral("image/jpeg"));
        if (!attachment)
            return std::nullopt;
        attachment->descriptor.width = photo.size.width();
        attachment->descriptor.height = photo.size.height();
        if (!receiversAccept(*attachment))
            return std::nullopt;
        return attachment;
    }

    void videoImported(const ImportedClip &clip)
    {
        retire(m_clips);
        const std::shared_ptr<Job> job = newJob();
        const QString fileName = QFileInfo(m_path).fileName();
        importPool()->start([job, clip, fileName] {
            std::optional<PreparedAttachment> video;
            try {
                qint64 durationMs = 0;
                for (const quint32 length : clip.durationsMs)
                    durationMs += length;
                if (!clip.segments.isEmpty() && clip.segments.size() <= AttachmentLimits::maxVideoSegments
                    && durationMs <= AttachmentLimits::maxVideoMs) {
                    QByteArray preview =
                        makePreview(decodeForPreview(clip.posterJpeg, 2 * AttachmentLimits::maxPreviewSide));
                    video = prepared(AttachmentKind::Video, encodeVideoSequence(clip.segments), std::move(preview),
                                     fileName, QString());
                }
                if (video) {
                    video->descriptor.width = clip.size.width();
                    video->descriptor.height = clip.size.height();
                    video->descriptor.durationMs = durationMs;
                    if (clip.trimmed)
                        video->notice = QStringLiteral("Only the first minute will be sent.");
                    if (!receiversAccept(*video))
                        video.reset();
                }
            } catch (...) {
                video.reset();
            }
            if (video) {
                post(job, [attachment = std::move(*video)](Private *d) { d->succeed(attachment); });
                return;
            }
            post(job, [](Private *d) { d->fallBackToFile(QStringLiteral("This video couldn't be prepared.")); });
        });
    }

    void audioEncoded(const EncodedSong &song)
    {
        retire(m_songs);
        const std::shared_ptr<Job> job = newJob();
        const QString fileName = QFileInfo(m_path).fileName();
        importPool()->start([job, song, fileName] {
            std::optional<PreparedAttachment> audio;
            try {
                audio = prepared(AttachmentKind::Audio, song.container, QByteArray(), fileName, QString());
                if (audio) {
                    audio->descriptor.durationMs = song.durationMs;
                    const QVector<quint8> &peaks = song.peaks;
                    audio->descriptor.peaks = QByteArray(reinterpret_cast<const char *>(peaks.constData()),
                                                         std::min<qsizetype>(peaks.size(), AttachmentLimits::maxPeaks));
                    if (song.trimmed)
                        audio->notice = QStringLiteral("Only the first 5 minutes will be sent.");
                    if (!receiversAccept(*audio))
                        audio.reset();
                }
            } catch (...) {
                audio.reset();
            }
            if (audio) {
                post(job, [attachment = std::move(*audio)](Private *d) { d->succeed(attachment); });
                return;
            }
            post(job, [](Private *d) { d->fallBackToFile(audioErrorText(SongImportError::EncodeFailed)); });
        });
    }

    // A photo, video or audio file that could not be converted goes as the
    // file it is, when it fits; `failure` is what the card says when it
    // does not.
    void fallBackToFile(const QString &failure)
    {
        retire(m_clips);
        retire(m_songs);
        const QString notice = fallbackNotice(m_kind);
        m_kind = AttachmentKind::File;
        runFile(notice, failure, m_progress);
    }

    // Reads the file at m_path as it is, reporting from `progressFrom` on.
    void runFile(const QString &notice, const QString &tooLarge, qreal progressFrom)
    {
        const std::shared_ptr<Job> job = newJob();
        // By name only: what a file claims to be inside is the receiver's
        // business, and OpenChat never opens it.
        const QString mimeType = QMimeDatabase().mimeTypeForFile(m_path, QMimeDatabase::MatchExtension).name();
        importPool()->start([job, path = m_path, notice, tooLarge, progressFrom, mimeType] {
            const auto failWith = [&job](const QString &message) {
                post(job, [message](Private *d) { d->fail(message); });
            };
            try {
                const QFileInfo info(path);
                QFile file(path);
                if (!info.isFile() || !file.open(QIODevice::ReadOnly)) {
                    failWith(couldNotOpenText());
                    return;
                }
                const qint64 size = file.size();
                if (size > AttachmentLimits::maxFileBytes) {
                    failWith(tooLarge);
                    return;
                }
                if (size < 1) {
                    failWith(QStringLiteral("That file is empty."));
                    return;
                }
                QByteArray blob;
                blob.reserve(size);
                // Never more than the cap, even from a file still growing.
                while (blob.size() <= AttachmentLimits::maxFileBytes) {
                    if (job->cancelled)
                        return;
                    const QByteArray chunk = file.read(fileChunkBytes);
                    if (chunk.isEmpty())
                        break;
                    blob += chunk;
                    const qreal done = qreal(std::min<qint64>(blob.size(), size)) / qreal(size);
                    post(job, [progressFrom, done](Private *d) {
                        d->report(progressFrom + (1.0 - progressFrom) * done * 0.95);
                    });
                }
                if (file.error() != QFileDevice::NoError) {
                    failWith(couldNotOpenText());
                    return;
                }
                if (blob.size() > AttachmentLimits::maxFileBytes) {
                    failWith(tooLarge);
                    return;
                }
                std::optional<PreparedAttachment> attachment =
                    prepared(AttachmentKind::File, std::move(blob), QByteArray(), info.fileName(), mimeType);
                if (!attachment || !receiversAccept(*attachment)) {
                    failWith(QStringLiteral("That file is empty."));
                    return;
                }
                attachment->notice = notice;
                post(job, [attachment = std::move(*attachment)](Private *d) { d->succeed(attachment); });
            } catch (...) {
                failWith(couldNotOpenText());
            }
        });
    }

    void succeed(const PreparedAttachment &attachment)
    {
        m_job.reset();
        m_busy = false;
        m_progress = 1.0;
        emitProgress();
        // Last: a receiver may start the next import, or delete this one.
        emit q->finished(attachment);
    }

    void fail(const QString &message)
    {
        m_job.reset();
        m_busy = false;
        m_flush.stop();
        emit q->failed(message);
    }

    // `value` of the whole job, never backwards, and at most every
    // progressIntervalMs; the latest value is sent when the interval ends.
    void report(qreal value)
    {
        if (!m_busy)
            return;
        value = std::clamp<qreal>(value, 0.0, 1.0);
        if (value <= m_progress)
            return;
        m_progress = value;
        const qint64 elapsed = m_sinceReport.elapsed();
        if (elapsed >= progressIntervalMs)
            emitProgress();
        else if (!m_flush.isActive())
            m_flush.start(int(progressIntervalMs - elapsed));
    }

    void emitProgress()
    {
        m_flush.stop();
        m_sinceReport.start();
        if (m_progress <= m_reported)
            return;
        m_reported = m_progress;
        emit q->progressChanged(m_progress);
    }

    QString m_path;
    std::shared_ptr<Job> m_job;
    std::unique_ptr<ClipImporter> m_clips;
    std::unique_ptr<SongImporter> m_songs;
    qreal m_progress = 0.0;
    qreal m_reported = 0.0;
    QElapsedTimer m_sinceReport;
    QTimer m_flush;
};

// ---------------------------------------------------------------------------

ChatAttachmentImporter::ChatAttachmentImporter(QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Private>(this))
{
    qRegisterMetaType<OpenChat::PreparedAttachment>();
}

ChatAttachmentImporter::~ChatAttachmentImporter()
{
    d->stop();
}

void ChatAttachmentImporter::start(const QString &path)
{
    switch (guessAttachmentKind(path)) {
    case AttachmentKind::Image:
        d->startImageFile(path);
        return;
    case AttachmentKind::Video:
        d->startVideo(path);
        return;
    case AttachmentKind::Audio:
        d->startAudio(path);
        return;
    case AttachmentKind::File:
        break;
    }
    d->startFile(path);
}

void ChatAttachmentImporter::startImage(const QImage &image, const QString &name)
{
    d->startImage(image, name);
}

void ChatAttachmentImporter::cancel()
{
    d->stop();
}

bool ChatAttachmentImporter::busy() const noexcept
{
    return d->m_busy;
}

AttachmentKind ChatAttachmentImporter::kind() const noexcept
{
    return d->m_kind;
}

bool ChatAttachmentImporter::videoSupported()
{
    return clipCodecAvailable();
}

} // namespace OpenChat
