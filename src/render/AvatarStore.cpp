#include "render/AvatarStore.h"

#include "domain/ProfileUpdate.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QImageReader>

namespace OpenChat {

namespace {

constexpr int maxDecodedSide = 1024;
constexpr int decodeAllocationLimitMb = 64;
const QString blobPrefix = QStringLiteral("blob:");

// A null image unless `jpeg` is a JPEG within the size bounds.
QImage decode(const QByteArray &jpeg)
{
    QByteArray bytes = jpeg;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::ReadOnly))
        return {};
    QImageReader reader(&buffer, "JPEG");
    reader.setAllocationLimit(decodeAllocationLimitMb);
    const QSize size = reader.size();
    if (!size.isValid() || size.width() > maxDecodedSide || size.height() > maxDecodedSide
        || size.width() < 1 || size.height() < 1)
        return {};
    QImage image;
    if (!reader.read(&image))
        return {};
    return image;
}

} // namespace

AvatarStore &AvatarStore::instance()
{
    static AvatarStore store;
    return store;
}

bool AvatarStore::isBlobKey(const QString &key)
{
    return key.startsWith(blobPrefix);
}

QString AvatarStore::keyFor(const QByteArray &jpeg)
{
    if (jpeg.isEmpty())
        return {};
    return blobPrefix
        + QString::fromLatin1(
            QCryptographicHash::hash(jpeg, QCryptographicHash::Sha256).toHex().left(32));
}

QString AvatarStore::registerJpeg(const QByteArray &jpeg)
{
    if (jpeg.isEmpty() || jpeg.size() > maxAvatarJpegBytes)
        return {};
    const QString key = keyFor(jpeg);
    {
        QMutexLocker locker(&m_mutex);
        if (m_jpegs.contains(key))
            return key;
    }

    // Decoded once whatever is kept, so a picture that cannot be drawn is
    // refused here rather than failing every time it is painted.
    const QImage image = decode(jpeg);
    if (image.isNull())
        return {};

    QMutexLocker locker(&m_mutex);
    m_jpegs.insert(key, jpeg);
    if (m_keepDecoded)
        m_images.insert(key, image);
    return key;
}

std::optional<QImage> AvatarStore::image(const QString &key) const
{
    if (!isBlobKey(key))
        return std::nullopt;
    QByteArray jpeg;
    {
        QMutexLocker locker(&m_mutex);
        if (const auto it = m_images.constFind(key); it != m_images.cend())
            return it.value();
        const auto it = m_jpegs.constFind(key);
        if (it == m_jpegs.cend())
            return std::nullopt;
        jpeg = it.value();
    }

    const QImage image = decode(jpeg);
    if (image.isNull())
        return std::nullopt;
    QMutexLocker locker(&m_mutex);
    if (m_keepDecoded)
        m_images.insert(key, image);
    return image;
}

void AvatarStore::setKeepDecoded(bool keep)
{
    QMutexLocker locker(&m_mutex);
    m_keepDecoded = keep;
    if (!keep)
        m_images.clear();
}

void AvatarStore::clear()
{
    QMutexLocker locker(&m_mutex);
    m_jpegs.clear();
    m_images.clear();
}

} // namespace OpenChat
