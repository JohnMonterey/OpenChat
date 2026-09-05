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
        if (m_images.contains(key))
            return key;
    }

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
    if (!reader.read(&image) || image.isNull())
        return {};

    QMutexLocker locker(&m_mutex);
    m_images.insert(key, image);
    return key;
}

std::optional<QImage> AvatarStore::image(const QString &key) const
{
    if (!isBlobKey(key))
        return std::nullopt;
    QMutexLocker locker(&m_mutex);
    const auto it = m_images.constFind(key);
    if (it == m_images.cend())
        return std::nullopt;
    return it.value();
}

void AvatarStore::clear()
{
    QMutexLocker locker(&m_mutex);
    m_images.clear();
}

} // namespace OpenChat
