#pragma once

#include <QByteArray>
#include <QColor>
#include <QHash>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QString>

#include <functional>
#include <optional>

namespace OpenChat {

// What a background picture contributes to the page's readability samples
// (SPEC §8.3): its near-darkest and near-lightest colours and its mean.
struct ImageStats final {
    QColor darkest;
    QColor lightest;
    QColor average;

    friend bool operator==(const ImageStats &, const ImageStats &) = default;
};

// Decoded profile background pictures, by content. A contact's picture is
// hostile input that may be shown only rarely, so it is decoded off the GUI
// thread, only when a page will really show it, and kept within a small
// budget: at most four decoded pictures (least recently used first out), one
// under Low memory mode. Keys are "pagebg:" + the SHA-256 of the JPEG in hex,
// the same hash the page core names it by.
class ProfileMediaStore final : public QObject
{
    Q_OBJECT

public:
    // The process instance (never destroyed; GUI thread).
    static ProfileMediaStore &instance();

    static constexpr int maxDecodedSide = 2048;
    static constexpr int decodeAllocationLimitMb = 64;
    static constexpr int maxDecodedImages = 4;

    [[nodiscard]] static QString imageKeyFor(const QByteArray &sha256);

    // Checks the bytes (at most maxBackgroundImageBytes, JPEG magic, 1 to
    // maxJpegScans scans, a header of at most maxDecodedSide a side) and
    // starts decoding them on the global thread pool. Returns the key, or ""
    // when refused. A decoded picture is available at once through image();
    // otherwise imageReady(key) follows when it is. Asking again while a
    // decode is in flight shares it.
    QString requestImage(const QByteArray &jpeg);
    // nullopt until decoded, after release() and after eviction.
    [[nodiscard]] std::optional<QImage> image(const QString &key) const;
    // Survives eviction of the pixels while the key is still in use, so a
    // page's corrected colours never flicker.
    [[nodiscard]] std::optional<ImageStats> stats(const QString &key) const;
    // The page stopped showing it. An unfinished decode is discarded; the
    // pixels stay cached (least recently used first out) unless Low memory
    // mode is on, which frees them at once.
    void release(const QString &key);
    // false (Low memory mode): at most one decoded picture, freed on release.
    void setKeepDecoded(bool keep);
    [[nodiscard]] bool keepDecoded() const noexcept { return m_keepDecoded; }
    void clear();
    void resetForTesting();

    // Tests: runs on the decoding thread right before a decode starts, to
    // see where decoding happens and to hold a decode in flight.
    void setDecodeHookForTesting(std::function<void()> hook);
    [[nodiscard]] int decodedCount() const;

    // The tile-extreme statistics of an image (exposed for tests): scaled to
    // 256×256 without averaging, split into 8×8 tiles; darkest = the 5th
    // percentile of the tiles' darkest pixels, lightest = the 95th percentile
    // of their lightest, average = the mean colour.
    [[nodiscard]] static ImageStats computeStats(const QImage &image);

signals:
    // Queued to the GUI thread.
    void imageReady(const QString &key);

private:
    ProfileMediaStore();

    struct Entry final {
        bool pending = false;
        bool inUse = true;  // requested and not released since
        bool failed = false;
        quint64 generation = 0;
        quint64 lastUse = 0;
        std::optional<QImage> image;
        std::optional<ImageStats> stats;
    };

    void finishDecode(const QString &key, quint64 generation, const QImage &image, const ImageStats &stats);
    void enforceBudget(const QString &keep);
    void touch(Entry &entry);

    QHash<QString, Entry> m_entries;
    bool m_keepDecoded = true;
    quint64 m_generation = 0;
    quint64 m_useClock = 0;

    mutable QMutex m_hookMutex;
    std::function<void()> m_decodeHook;
};

} // namespace OpenChat
