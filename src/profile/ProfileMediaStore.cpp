#include "profile/ProfileMediaStore.h"

#include "diagnostics/Logging.h"
#include "domain/ProfilePageCodec.h"

#include <QBuffer>
#include <QImageReader>
#include <QMutexLocker>
#include <QThreadPool>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace OpenChat {

namespace {

const QString keyPrefix = QStringLiteral("pagebg:");

// WCAG relative luminance per 8-bit channel value, for ranking pixels.
const std::array<double, 256> &linearTable()
{
    static const std::array<double, 256> table = [] {
        std::array<double, 256> values{};
        for (int i = 0; i < 256; ++i) {
            const double c = i / 255.0;
            values[std::size_t(i)] = c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
        }
        return values;
    }();
    return table;
}

double luminance(QRgb pixel)
{
    const auto &table = linearTable();
    return 0.2126 * table[std::size_t(qRed(pixel))] + 0.7152 * table[std::size_t(qGreen(pixel))]
           + 0.0722 * table[std::size_t(qBlue(pixel))];
}

// The JPEG's header size, or an invalid size when the reader cannot tell.
QSize headerSize(const QByteArray &jpeg)
{
    QBuffer buffer;
    buffer.setData(jpeg);
    if (!buffer.open(QIODevice::ReadOnly))
        return {};
    QImageReader reader(&buffer, "JPEG");
    return reader.size();
}

QImage decodeJpeg(const QByteArray &jpeg)
{
    QBuffer buffer;
    buffer.setData(jpeg);
    if (!buffer.open(QIODevice::ReadOnly))
        return {};
    QImageReader reader(&buffer, "JPEG");
    reader.setAllocationLimit(ProfileMediaStore::decodeAllocationLimitMb);
    const QSize size = reader.size();
    if (!size.isValid() || size.width() < 1 || size.height() < 1
        || size.width() > ProfileMediaStore::maxDecodedSide || size.height() > ProfileMediaStore::maxDecodedSide)
        return {};
    QImage image;
    if (!reader.read(&image) || image.isNull())
        return {};
    // One format for the statistics and the texture upload, whatever the
    // JPEG's colour space was (greyscale JPEGs decode to Grayscale8).
    return image.convertToFormat(QImage::Format_RGB32);
}

} // namespace

ProfileMediaStore &ProfileMediaStore::instance()
{
    static auto *store = new ProfileMediaStore;
    return *store;
}

ProfileMediaStore::ProfileMediaStore() = default;

QString ProfileMediaStore::imageKeyFor(const QByteArray &sha256)
{
    return keyPrefix + QString::fromLatin1(sha256.toHex());
}

ImageStats ProfileMediaStore::computeStats(const QImage &source)
{
    ImageStats stats;
    if (source.isNull())
        return stats;
    // Nearest-neighbour on purpose: averaging would turn a fine black-and-
    // white texture into flat grey and hide the extremes text must survive.
    constexpr int side = 256;
    constexpr int tile = 8;
    const QImage small = source.scaled(side, side, Qt::IgnoreAspectRatio, Qt::FastTransformation)
                             .convertToFormat(QImage::Format_RGB32);
    std::vector<std::pair<double, QRgb>> minima;
    std::vector<std::pair<double, QRgb>> maxima;
    minima.reserve((side / tile) * (side / tile));
    maxima.reserve(minima.capacity());
    double sumR = 0;
    double sumG = 0;
    double sumB = 0;
    for (int ty = 0; ty < side; ty += tile) {
        for (int tx = 0; tx < side; tx += tile) {
            std::pair<double, QRgb> lowest{2.0, 0};
            std::pair<double, QRgb> highest{-1.0, 0};
            for (int y = ty; y < ty + tile; ++y) {
                const auto *line = reinterpret_cast<const QRgb *>(small.constScanLine(y));
                for (int x = tx; x < tx + tile; ++x) {
                    const QRgb pixel = line[x];
                    const double lum = luminance(pixel);
                    if (lum < lowest.first)
                        lowest = {lum, pixel};
                    if (lum > highest.first)
                        highest = {lum, pixel};
                    sumR += qRed(pixel);
                    sumG += qGreen(pixel);
                    sumB += qBlue(pixel);
                }
            }
            minima.push_back(lowest);
            maxima.push_back(highest);
        }
    }
    const auto byLuminance = [](const auto &a, const auto &b) { return a.first < b.first; };
    std::sort(minima.begin(), minima.end(), byLuminance);
    std::sort(maxima.begin(), maxima.end(), byLuminance);
    const auto percentile = [](const std::vector<std::pair<double, QRgb>> &sorted, double p) {
        const auto index = std::size_t(std::lround(p * double(sorted.size() - 1)));
        return QColor(sorted[index].second);
    };
    stats.darkest = percentile(minima, 0.05);
    stats.lightest = percentile(maxima, 0.95);
    const double count = double(side) * side;
    stats.average = QColor(int(std::lround(sumR / count)), int(std::lround(sumG / count)),
                           int(std::lround(sumB / count)));
    return stats;
}

QString ProfileMediaStore::requestImage(const QByteArray &jpeg)
{
    // Everything that can be refused cheaply is refused here, before a
    // worker spends time on it: the size, the JPEG magic, the scan count (a
    // progressive "scan bomb" re-reads the whole image per scan) and the
    // header's dimensions.
    if (jpeg.isEmpty() || jpeg.size() > maxBackgroundImageBytes)
        return {};
    if (jpeg.size() < 3 || quint8(jpeg[0]) != 0xFF || quint8(jpeg[1]) != 0xD8 || quint8(jpeg[2]) != 0xFF)
        return {};
    const int scans = jpegScanCount(jpeg);
    if (scans < 1 || scans > maxJpegScans)
        return {};
    const QSize size = headerSize(jpeg);
    if (!size.isValid() || size.width() < 1 || size.height() < 1 || size.width() > maxDecodedSide
        || size.height() > maxDecodedSide)
        return {};

    const QString key = imageKeyFor(pageMediaHash(jpeg));
    auto it = m_entries.find(key);
    if (it != m_entries.end()) {
        if (it->failed)
            return {};
        it->inUse = true;
        touch(*it);
        if (it->image || it->pending)
            return key;
        // Its pixels were evicted while the page kept using it: decode again.
    } else {
        it = m_entries.insert(key, Entry{});
    }
    it->pending = true;
    it->inUse = true;
    it->generation = ++m_generation;
    touch(*it);

    std::function<void()> hook;
    {
        QMutexLocker locker(&m_hookMutex);
        hook = m_decodeHook;
    }
    const quint64 generation = it->generation;
    QThreadPool::globalInstance()->start([this, key, generation, jpeg, hook] {
        if (hook)
            hook();
        const QImage image = decodeJpeg(jpeg);
        const ImageStats stats = computeStats(image);
        // The store lives as long as the process, so `this` is safe here.
        QMetaObject::invokeMethod(
            this, [this, key, generation, image, stats] { finishDecode(key, generation, image, stats); },
            Qt::QueuedConnection);
    });
    return key;
}

void ProfileMediaStore::finishDecode(const QString &key, quint64 generation, const QImage &image,
                                     const ImageStats &stats)
{
    const auto it = m_entries.find(key);
    // Released, cleared or asked for again since: this result is stale.
    if (it == m_entries.end() || !it->pending || it->generation != generation)
        return;
    it->pending = false;
    if (image.isNull()) {
        // It passed the header checks but will not decode: remember that, so
        // asking again is refused instead of retried.
        qCWarning(mediaLog) << "profile background did not decode:" << key;
        it->failed = true;
        return;
    }
    it->image = image;
    it->stats = stats;
    touch(*it);
    enforceBudget(key);
    emit imageReady(key);
}

std::optional<QImage> ProfileMediaStore::image(const QString &key) const
{
    const auto it = m_entries.constFind(key);
    if (it == m_entries.constEnd() || !it->image)
        return std::nullopt;
    return it->image;
}

std::optional<ImageStats> ProfileMediaStore::stats(const QString &key) const
{
    const auto it = m_entries.constFind(key);
    if (it == m_entries.constEnd())
        return std::nullopt;
    return it->stats;
}

void ProfileMediaStore::release(const QString &key)
{
    const auto it = m_entries.find(key);
    if (it == m_entries.end())
        return;
    if (it->pending || it->failed || !m_keepDecoded || !it->image) {
        m_entries.erase(it);
        return;
    }
    it->inUse = false;
}

void ProfileMediaStore::setKeepDecoded(bool keep)
{
    if (m_keepDecoded == keep)
        return;
    m_keepDecoded = keep;
    if (!keep) {
        for (auto it = m_entries.begin(); it != m_entries.end();) {
            if (!it->inUse)
                it = m_entries.erase(it);
            else
                ++it;
        }
        enforceBudget(QString());
    }
}

void ProfileMediaStore::touch(Entry &entry)
{
    entry.lastUse = ++m_useClock;
}

void ProfileMediaStore::enforceBudget(const QString &keep)
{
    const int budget = m_keepDecoded ? maxDecodedImages : 1;
    for (;;) {
        int decoded = 0;
        auto oldest = m_entries.end();
        for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
            if (!it->image)
                continue;
            ++decoded;
            if (it.key() != keep && (oldest == m_entries.end() || it->lastUse < oldest->lastUse))
                oldest = it;
        }
        if (decoded <= budget || oldest == m_entries.end())
            return;
        if (oldest->inUse)
            oldest->image.reset(); // the statistics stay while the page uses it
        else
            m_entries.erase(oldest);
    }
}

void ProfileMediaStore::clear()
{
    m_entries.clear();
}

void ProfileMediaStore::resetForTesting()
{
    clear();
    m_keepDecoded = true;
    QMutexLocker locker(&m_hookMutex);
    m_decodeHook = {};
}

void ProfileMediaStore::setDecodeHookForTesting(std::function<void()> hook)
{
    QMutexLocker locker(&m_hookMutex);
    m_decodeHook = std::move(hook);
}

int ProfileMediaStore::decodedCount() const
{
    return int(std::count_if(m_entries.cbegin(), m_entries.cend(), [](const Entry &entry) {
        return entry.image.has_value();
    }));
}

} // namespace OpenChat
