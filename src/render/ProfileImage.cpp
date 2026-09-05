#include "render/ProfileImage.h"

#include <QBuffer>
#include <QFileInfo>
#include <QImageReader>
#include <QPainter>

#include <algorithm>

namespace OpenChat {

namespace {

using Ret = Result<QByteArray, ProfileImageError>;

// Decoding memory for the largest accepted source: maxSide² at 4 bytes a pixel
// is ~576 MB for 12,000 px, which is too much to allow blindly. The reader is
// asked to scale on decode wherever the format supports it (JPEG does), and
// the allocation cap keeps a hostile header from reserving more than this.
constexpr int decodeAllocationLimitMb = 512;

QByteArray encodeJpeg(const QImage &image, int quality)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly))
        return {};
    if (!image.save(&buffer, "JPEG", quality))
        return {};
    return bytes;
}

QImage cropSquare(const QImage &image)
{
    const int side = std::min(image.width(), image.height());
    const int x = (image.width() - side) / 2;
    const int y = (image.height() - side) / 2;
    return image.copy(x, y, side, side);
}

} // namespace

Result<QByteArray, ProfileImageError> processProfileImage(const QImage &source,
                                                          const ProfileImageLimits &limits)
{
    if (source.isNull())
        return Ret::failure(ProfileImageError::Unreadable);
    if (source.width() < limits.minSide || source.height() < limits.minSide)
        return Ret::failure(ProfileImageError::TooSmall);
    if (source.width() > limits.maxSide || source.height() > limits.maxSide)
        return Ret::failure(ProfileImageError::TooLarge);

    // JPEG has no alpha: composite onto white first so a transparent PNG does
    // not come out black.
    QImage square = cropSquare(source).convertToFormat(QImage::Format_RGB32);
    if (square.hasAlphaChannel() || source.hasAlphaChannel()) {
        QImage opaque(square.size(), QImage::Format_RGB32);
        opaque.fill(Qt::white);
        QPainter painter(&opaque);
        painter.drawImage(0, 0, cropSquare(source));
        painter.end();
        square = opaque;
    }

    // Try the requested side first, then shrink; at each side walk the quality
    // down. The first encoding under the bound wins, so a small simple picture
    // keeps full quality and a busy one gives up detail before pixels. A source
    // smaller than the output side is never enlarged.
    const int outputSide = std::min(limits.outputSide, square.width());
    const int sides[] = {outputSide, outputSide * 3 / 4, outputSide / 2, outputSide / 4};
    const int qualities[] = {85, 75, 65, 55, 45, 35};
    for (const int side : sides) {
        if (side < 8)
            break;
        const QImage scaled =
            square.scaled(side, side, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        for (const int quality : qualities) {
            const QByteArray jpeg = encodeJpeg(scaled, quality);
            if (jpeg.isEmpty())
                return Ret::failure(ProfileImageError::EncodeFailed);
            if (jpeg.size() <= limits.maxOutputBytes)
                return Ret::success(jpeg);
        }
    }
    return Ret::failure(ProfileImageError::EncodeFailed);
}

Result<QByteArray, ProfileImageError> processProfileImageFile(const QString &path,
                                                              const ProfileImageLimits &limits)
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

    // The header alone says how big the picture is: refuse absurd dimensions
    // before a single pixel is decoded.
    const QSize size = reader.size();
    if (size.isValid()) {
        if (size.width() < limits.minSide || size.height() < limits.minSide)
            return Ret::failure(ProfileImageError::TooSmall);
        if (size.width() > limits.maxSide || size.height() > limits.maxSide)
            return Ret::failure(ProfileImageError::TooLarge);
        // Decode a large photo straight down towards the output size where the
        // decoder can (JPEG's DCT scaling); other formats ignore this and decode
        // at full size, bounded above by maxSide.
        const int shortest = std::min(size.width(), size.height());
        if (shortest > limits.outputSide * 4) {
            const qreal factor = qreal(limits.outputSide * 4) / shortest;
            reader.setScaledSize(QSize(std::max(1, int(size.width() * factor)),
                                       std::max(1, int(size.height() * factor))));
        }
    }

    QImage image;
    if (!reader.read(&image) || image.isNull())
        return Ret::failure(ProfileImageError::Unreadable);
    // After a decode-time downscale the image can legitimately be smaller than
    // minSide, so only the header check above applies that bound.
    ProfileImageLimits decoded = limits;
    decoded.minSide = std::min(limits.minSide, std::min(image.width(), image.height()));
    return processProfileImage(image, decoded);
}

QString profileImageErrorText(ProfileImageError error)
{
    switch (error) {
    case ProfileImageError::FileMissing:
        return QStringLiteral("That file could not be opened.");
    case ProfileImageError::FileTooLarge:
        return QStringLiteral("That file is too large for a profile picture (25 MB max).");
    case ProfileImageError::Unreadable:
        return QStringLiteral("That file is not an image OpenChat can read.");
    case ProfileImageError::TooSmall:
        return QStringLiteral("That image is too small (at least 16 × 16 pixels).");
    case ProfileImageError::TooLarge:
        return QStringLiteral("That image is too large (at most 12,000 pixels a side).");
    case ProfileImageError::EncodeFailed:
        return QStringLiteral("That image could not be prepared as a profile picture.");
    }
    return QStringLiteral("That image could not be used.");
}

} // namespace OpenChat
