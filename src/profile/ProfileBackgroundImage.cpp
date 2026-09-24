#include "profile/ProfileBackgroundImage.h"

#include <QBuffer>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QThreadPool>

#include <algorithm>
#include <atomic>
#include <vector>

namespace OpenChat {

namespace {

using Ret = Result<ProcessedBackground, ProfileImageError>;

// The same bounded decode as profile pictures: a hostile header cannot
// reserve more than this, and big photos are scaled while decoding.
constexpr int decodeAllocationLimitMb = 512;
constexpr int ladderSides[] = {1600, 1280, 1024, 800, 640};
constexpr int ladderQualities[] = {82, 74, 66, 58, 50, 42};

// Baseline (sequential) JPEG: every decoder reads it in one pass, and it
// cannot carry the scan count a progressive "scan bomb" would. The image
// written is always a fresh one, so no text, colour profile or EXIF from the
// source travels with it.
QByteArray encodeBaselineJpeg(const QImage &image, int quality)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly))
        return {};
    QImageWriter writer(&buffer, "jpeg");
    writer.setQuality(quality);
    writer.setProgressiveScanWrite(false);
    writer.setOptimizedWrite(true); // optimal Huffman tables: smaller, still baseline
    if (!writer.write(image))
        return {};
    return bytes;
}

} // namespace

Result<ProcessedBackground, ProfileImageError> processProfileBackground(const QImage &source, const QColor &matte,
                                                                        const ProfileBackgroundLimits &limits,
                                                                        ProgressFn progress, CancelFn cancelled)
{
    if (source.isNull())
        return Ret::failure(ProfileImageError::Unreadable);
    if (source.width() < limits.minSide || source.height() < limits.minSide)
        return Ret::failure(ProfileImageError::TooSmall);
    if (source.width() > limits.maxSide || source.height() > limits.maxSide)
        return Ret::failure(ProfileImageError::TooLarge);

    // Composite onto the page's base colour: JPEG has no alpha, and the base
    // is what shows around a Fit or Center picture anyway. Drawing into a
    // fresh image also leaves every piece of the source's metadata behind.
    QImage pixels = source;
    pixels.setDevicePixelRatio(1.0);
    QImage flat(pixels.size(), QImage::Format_RGB32);
    flat.fill(QColor(matte.rgb()));
    {
        QPainter painter(&flat);
        painter.drawImage(0, 0, pixels);
    }

    const int sourceLong = std::max(flat.width(), flat.height());
    std::vector<int> sides{std::min(limits.maxLongSide, sourceLong)};
    for (const int side : ladderSides) {
        if (side < sides.front())
            sides.push_back(side);
    }
    const int steps = int(sides.size() * std::size(ladderQualities));
    int step = 0;
    for (const int side : sides) {
        // Aspect kept, never enlarged.
        const QImage scaled = side >= sourceLong
                                  ? flat
                                  : flat.scaled(side, side, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        for (const int quality : ladderQualities) {
            if (cancelled && cancelled())
                return Ret::failure(ProfileImageError::EncodeFailed);
            const QByteArray jpeg = encodeBaselineJpeg(scaled, quality);
            if (jpeg.isEmpty())
                return Ret::failure(ProfileImageError::EncodeFailed);
            ++step;
            if (jpeg.size() <= limits.maxOutputBytes) {
                if (progress)
                    progress(1.0);
                return Ret::success({jpeg, scaled.size()});
            }
            if (progress)
                progress(qreal(step) / steps);
        }
    }
    return Ret::failure(ProfileImageError::EncodeFailed);
}

Result<ProcessedBackground, ProfileImageError> processProfileBackgroundFile(const QString &path, const QColor &matte,
                                                                            const ProfileBackgroundLimits &limits,
                                                                            ProgressFn progress, CancelFn cancelled)
{
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile() || !info.isReadable())
        return Ret::failure(ProfileImageError::FileMissing);
    if (info.size() > limits.maxFileBytes)
        return Ret::failure(ProfileImageError::FileTooLarge);

    QImageReader reader(path);
    reader.setAutoTransform(true); // honour EXIF orientation
    reader.setAllocationLimit(decodeAllocationLimitMb);
    if (!reader.canRead())
        return Ret::failure(ProfileImageError::Unreadable);

    // Refuse absurd dimensions from the header alone, before decoding.
    const QSize size = reader.size();
    if (size.isValid()) {
        if (size.width() < limits.minSide || size.height() < limits.minSide)
            return Ret::failure(ProfileImageError::TooSmall);
        if (size.width() > limits.maxSide || size.height() > limits.maxSide)
            return Ret::failure(ProfileImageError::TooLarge);
        // A photo far beyond the output is decoded straight down to four
        // times the target where the format can (JPEG's DCT scaling).
        const int longest = std::max(size.width(), size.height());
        const int decodeLong = limits.maxLongSide * 4;
        if (longest > decodeLong) {
            const qreal factor = qreal(decodeLong) / longest;
            reader.setScaledSize(QSize(std::max(1, int(size.width() * factor)),
                                       std::max(1, int(size.height() * factor))));
        }
    }
    if (cancelled && cancelled())
        return Ret::failure(ProfileImageError::EncodeFailed);

    // Animated formats: read() gives the first frame.
    QImage image;
    if (!reader.read(&image) || image.isNull())
        return Ret::failure(ProfileImageError::Unreadable);
    // A decode-time downscale may take an extreme panorama's short side below
    // minSide; only the header check above applies that bound.
    ProfileBackgroundLimits decoded = limits;
    decoded.minSide = std::min(limits.minSide, std::min(image.width(), image.height()));
    return processProfileBackground(image, matte, decoded, std::move(progress), std::move(cancelled));
}

QString profileBackgroundErrorText(ProfileImageError error)
{
    switch (error) {
    case ProfileImageError::FileMissing:
        return QStringLiteral("That file could not be opened.");
    case ProfileImageError::FileTooLarge:
        return QStringLiteral("That file is too large for a background (25 MB max).");
    case ProfileImageError::Unreadable:
        return QStringLiteral("That file is not a picture OpenChat can read.");
    case ProfileImageError::TooSmall:
        return QStringLiteral("That picture is too small (at least 32 × 32 pixels).");
    case ProfileImageError::TooLarge:
        return QStringLiteral("That picture is too large (at most 12,000 pixels a side).");
    case ProfileImageError::EncodeFailed:
        return QStringLiteral("That picture could not be made small enough to share.");
    }
    return QStringLiteral("That picture could not be used.");
}

// Shared by the importer and its worker. The worker reaches the importer only
// through `owner`, under the mutex, so an importer that is destroyed or moves
// on to a newer file is never touched by an old run.
struct ProfileBackgroundImporter::Job final {
    QMutex mutex;
    ProfileBackgroundImporter *owner = nullptr;
    std::atomic_bool cancelled{false};

    // Runs `apply` on the owner's thread, if the owner still wants this job.
    template<typename Apply>
    static void post(const std::shared_ptr<Job> &job, Apply apply)
    {
        QMutexLocker locker(&job->mutex);
        ProfileBackgroundImporter *owner = job->owner;
        if (!owner || job->cancelled)
            return;
        QMetaObject::invokeMethod(
            owner,
            [owner, job, apply] {
                if (owner->m_job == job)
                    apply(owner);
            },
            Qt::QueuedConnection);
    }
};

ProfileBackgroundImporter::ProfileBackgroundImporter(QObject *parent) : QObject(parent) {}

ProfileBackgroundImporter::~ProfileBackgroundImporter()
{
    detach();
}

void ProfileBackgroundImporter::detach()
{
    if (!m_job)
        return;
    {
        QMutexLocker locker(&m_job->mutex);
        m_job->cancelled = true;
        m_job->owner = nullptr;
    }
    m_job.reset();
}

void ProfileBackgroundImporter::setProgress(qreal progress)
{
    if (qFuzzyCompare(m_progress + 1.0, progress + 1.0))
        return;
    m_progress = progress;
    emit progressChanged(progress);
}

void ProfileBackgroundImporter::start(const QString &path, const QColor &matte)
{
    detach();
    auto job = std::make_shared<Job>();
    job->owner = this;
    m_job = job;
    setProgress(0);
    QThreadPool::globalInstance()->start([job, path, matte, hook = m_workHook] {
        if (hook)
            hook();
        const auto result = processProfileBackgroundFile(
            path, matte, {},
            [job](qreal value) { Job::post(job, [value](ProfileBackgroundImporter *owner) { owner->setProgress(value); }); },
            [job] { return job->cancelled.load(); });
        if (result) {
            const ProcessedBackground processed = result.value();
            Job::post(job, [processed](ProfileBackgroundImporter *owner) {
                owner->m_job.reset();
                owner->setProgress(1.0);
                emit owner->finished(processed.jpeg, processed.size);
            });
        } else {
            const ProfileImageError error = result.error();
            Job::post(job, [error](ProfileBackgroundImporter *owner) {
                owner->m_job.reset();
                owner->setProgress(0);
                emit owner->failed(error, profileBackgroundErrorText(error));
            });
        }
    });
}

void ProfileBackgroundImporter::setWorkHookForTesting(std::function<void()> hook)
{
    m_workHook = std::move(hook);
}

void ProfileBackgroundImporter::cancel()
{
    if (!m_job)
        return;
    detach();
    setProgress(0);
}

} // namespace OpenChat
