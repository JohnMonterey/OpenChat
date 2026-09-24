#include "profile/ProfilePanelMedia.h"

#include "domain/ProfilePageCodec.h"

#include <QBuffer>
#include <QImageReader>
#include <QMutexLocker>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QQuickWindow>
#include <QThreadPool>

#include <algorithm>
#include <cmath>

namespace OpenChat {

namespace {

[[nodiscard]] QString decodeId(const QString &key, int side)
{
    return key + u'/' + QString::number(side);
}

// Decodes a panel picture no larger than `side` on its long edge. The bytes
// passed the arrival checks (a JPEG with 1 to 32 scans), but the header may
// still claim anything, so the reader's allocation is capped too.
[[nodiscard]] QImage decodePicture(const QByteArray &bytes, int side)
{
    if (bytes.size() > maxPanelImageBytes || jpegScanCount(bytes) < 1)
        return {};
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, "jpeg");
    reader.setAllocationLimit(PanelMediaLibrary::decodeAllocationLimitMb);
    const QSize size = reader.size();
    if (!size.isValid() || size.isEmpty() || size.width() > 4 * PanelMediaLibrary::maxDecodedSide
        || size.height() > 4 * PanelMediaLibrary::maxDecodedSide)
        return {};
    const int longSide = std::max(size.width(), size.height());
    if (longSide > side)
        reader.setScaledSize(size.scaled(side, side, Qt::KeepAspectRatio));
    QImage image = reader.read();
    if (image.isNull())
        return {};
    return image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

} // namespace

PanelMediaLibrary &PanelMediaLibrary::instance()
{
    static auto *library = new PanelMediaLibrary; // lives as long as the process
    return *library;
}

int PanelMediaLibrary::bucketFor(int longSide)
{
    if (longSide <= 320)
        return 320;
    if (longSide <= 640)
        return 640;
    return maxDecodedSide;
}

void PanelMediaLibrary::put(const QString &key, const QByteArray &bytes)
{
    if (key.isEmpty() || bytes.isEmpty())
        return;
    bool fresh = false;
    {
        QMutexLocker locker(&m_mutex);
        auto it = m_bytes.find(key);
        if (it == m_bytes.end()) {
            m_bytes.insert(key, {bytes, 1});
            fresh = true;
        } else {
            it->second += 1;
        }
    }
    if (fresh)
        emit arrived(key);
}

void PanelMediaLibrary::release(const QString &key)
{
    bool gone = false;
    {
        QMutexLocker locker(&m_mutex);
        auto it = m_bytes.find(key);
        if (it == m_bytes.end())
            return;
        if (--it->second <= 0) {
            m_bytes.erase(it);
            gone = true;
        }
    }
    if (!gone)
        return;
    // Its pictures go too: nothing may show a blob no page holds.
    for (auto it = m_decoded.begin(); it != m_decoded.end();) {
        if (it->key == key) {
            m_decodedBytes -= it->image.sizeInBytes();
            it = m_decoded.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = m_pending.begin(); it != m_pending.end();) {
        if (it.key().startsWith(key + u'/'))
            it = m_pending.erase(it);
        else
            ++it;
    }
}

QByteArray PanelMediaLibrary::get(const QString &key) const
{
    QMutexLocker locker(&m_mutex);
    const auto it = m_bytes.constFind(key);
    return it == m_bytes.constEnd() ? QByteArray() : it->first;
}

bool PanelMediaLibrary::contains(const QString &key) const
{
    QMutexLocker locker(&m_mutex);
    return m_bytes.contains(key);
}

void PanelMediaLibrary::clear()
{
    {
        QMutexLocker locker(&m_mutex);
        m_bytes.clear();
    }
    m_decoded.clear();
    m_decodedBytes = 0;
    m_pending.clear();
    ++m_generation;
}

void PanelMediaLibrary::setDecodedBudget(qint64 bytes)
{
    m_budget = std::max<qint64>(bytes, 1);
    enforceBudget();
}

QImage PanelMediaLibrary::image(const QString &key, int longSide)
{
    const int side = bucketFor(longSide);
    // The smallest decode that is at least as large, else the largest there is.
    const Decoded *best = nullptr;
    for (auto it = m_decoded.begin(); it != m_decoded.end(); ++it) {
        if (it->key != key)
            continue;
        if (it->side == side) {
            m_decoded.splice(m_decoded.begin(), m_decoded, it); // most recently used
            return m_decoded.front().image;
        }
        if (!best || it->side > best->side)
            best = &*it;
    }
    const QString id = decodeId(key, side);
    const QByteArray bytes = get(key);
    if (!bytes.isEmpty() && !m_pending.contains(id)) {
        const quint64 generation = ++m_generation;
        m_pending.insert(id, generation);
        QPointer<PanelMediaLibrary> self(this);
        QThreadPool::globalInstance()->start([self, key, side, generation, bytes] {
            const QImage decodedImage = decodePicture(bytes, side);
            QMetaObject::invokeMethod(
                self.data(),
                [self, key, side, generation, decodedImage] {
                    if (self)
                        self->finishDecode(key, side, generation, decodedImage);
                },
                Qt::QueuedConnection);
        });
    }
    // Meanwhile a decode of another size stands in for it.
    return best ? best->image : QImage();
}

void PanelMediaLibrary::finishDecode(const QString &key, int side, quint64 generation, const QImage &image)
{
    const QString id = decodeId(key, side);
    if (m_pending.value(id) != generation)
        return; // released (or cleared) since: not wanted any more
    m_pending.remove(id);
    if (image.isNull() || !contains(key))
        return;
    m_decoded.push_front({key, side, image});
    m_decodedBytes += image.sizeInBytes();
    enforceBudget();
    emit decoded(key);
}

void PanelMediaLibrary::enforceBudget()
{
    // The newest decode always stays, however large.
    while (m_decodedBytes > m_budget && m_decoded.size() > 1) {
        m_decodedBytes -= m_decoded.back().image.sizeInBytes();
        m_decoded.pop_back();
    }
}

// ---------------------------------------------------------------------------

ProfilePanelImage::ProfilePanelImage(QQuickItem *parent)
    : QQuickPaintedItem(parent)
{
    setAntialiasing(true);
    connect(&PanelMediaLibrary::instance(), &PanelMediaLibrary::arrived, this, [this](const QString &key) {
        if (key == m_key)
            fetch();
    });
    connect(&PanelMediaLibrary::instance(), &PanelMediaLibrary::decoded, this, [this](const QString &key) {
        if (key == m_key)
            fetch();
    });
}

void ProfilePanelImage::setMediaKey(const QString &key)
{
    if (key == m_key)
        return;
    m_key = key;
    const bool wasReady = ready();
    m_image = {};
    emit mediaKeyChanged();
    fetch();
    if (wasReady != ready())
        emit readyChanged();
    update();
}

void ProfilePanelImage::setCrop(bool crop)
{
    if (crop == m_crop)
        return;
    m_crop = crop;
    emit cropChanged();
    update();
}

void ProfilePanelImage::setRadius(qreal radius)
{
    if (qFuzzyCompare(radius, m_radius))
        return;
    m_radius = radius;
    emit radiusChanged();
    update();
}

void ProfilePanelImage::setCircle(bool circle)
{
    if (circle == m_circle)
        return;
    m_circle = circle;
    emit circleChanged();
    update();
}

qreal ProfilePanelImage::sourceAspect() const
{
    return m_image.isNull() || m_image.height() == 0 ? 0 : qreal(m_image.width()) / m_image.height();
}

void ProfilePanelImage::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size())
        fetch();
}

void ProfilePanelImage::fetch()
{
    if (m_key.isEmpty() || width() <= 0 || height() <= 0)
        return;
    const qreal dpr = window() ? window()->effectiveDevicePixelRatio() : 1.0;
    const int longSide = int(std::ceil(std::max(width(), height()) * dpr));
    const QImage image = PanelMediaLibrary::instance().image(m_key, longSide);
    if (image.isNull() || image.cacheKey() == m_image.cacheKey())
        return;
    const bool wasReady = ready();
    m_image = image;
    if (!wasReady)
        emit readyChanged();
    update();
}

void ProfilePanelImage::paint(QPainter *painter)
{
    if (m_image.isNull())
        return;
    const QRectF bounds(0, 0, width(), height());
    const QSizeF source = m_image.size();
    QRectF target;
    QRectF clip = bounds;
    if (m_crop || m_circle) {
        // Cover: the picture fills the item, cut evenly on the long axis.
        const QSizeF scaled = source.scaled(bounds.size(), Qt::KeepAspectRatioByExpanding);
        target = QRectF(QPointF((bounds.width() - scaled.width()) / 2, (bounds.height() - scaled.height()) / 2),
                        scaled);
    } else {
        const QSizeF scaled = source.scaled(bounds.size(), Qt::KeepAspectRatio);
        target = QRectF(QPointF((bounds.width() - scaled.width()) / 2, (bounds.height() - scaled.height()) / 2),
                        scaled);
        clip = target;
    }
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter->setRenderHint(QPainter::Antialiasing, true);
    QPainterPath path;
    if (m_circle) {
        const qreal side = std::min(bounds.width(), bounds.height());
        path.addEllipse(QRectF((bounds.width() - side) / 2, (bounds.height() - side) / 2, side, side));
    } else if (m_radius > 0) {
        path.addRoundedRect(clip, m_radius, m_radius);
    } else {
        path.addRect(clip);
    }
    painter->setClipPath(path);
    painter->drawImage(target, m_image);
}

} // namespace OpenChat
