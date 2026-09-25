#pragma once

#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QQuickPaintedItem>
#include <QString>

#include <list>

namespace OpenChat {

// The blobs custom panels show (docs/profile-panels.md): pictures, covers,
// posters and video segments, by the hex SHA-256 the page core names them
// by. ProfileController puts the bytes of every panel blob a page on screen
// holds and releases them when no page shows them; items read them here. A
// contact's picture is hostile input, so it is decoded off the GUI thread,
// only at the size it is drawn, within a budget of decoded pixels (least
// recently used first out).
//
// Bytes are counted: put() twice needs release() twice. get() is safe from
// any thread (a video decoder reads segments on its own).
class PanelMediaLibrary final : public QObject
{
    Q_OBJECT

public:
    static PanelMediaLibrary &instance();

    // The decoded-picture budget in bytes; Low memory mode lowers it.
    static constexpr qint64 defaultDecodedBudget = 64LL * 1024 * 1024;
    static constexpr qint64 lowMemoryDecodedBudget = 16LL * 1024 * 1024;
    static constexpr int maxDecodedSide = 1280;
    static constexpr int decodeAllocationLimitMb = 32;

    void put(const QString &key, const QByteArray &bytes);
    void release(const QString &key);
    [[nodiscard]] QByteArray get(const QString &key) const;
    [[nodiscard]] bool contains(const QString &key) const;
    void clear();

    // The picture decoded to fit `longSide` (rounded up to 320, 640 or
    // 1280), or a null image while it is still being decoded (decoded(key)
    // follows) or when the bytes are missing or not a picture.
    [[nodiscard]] QImage image(const QString &key, int longSide);
    void setDecodedBudget(qint64 bytes);
    [[nodiscard]] qint64 decodedBytes() const noexcept { return m_decodedBytes; }

    // The side a request for `longSide` decodes at.
    [[nodiscard]] static int bucketFor(int longSide);

signals:
    void arrived(const QString &key); // put() made bytes available
    void decoded(const QString &key);

private:
    PanelMediaLibrary() = default;

    struct Decoded final {
        QString key;
        int side = 0;
        QImage image;
    };

    void finishDecode(const QString &key, int side, quint64 generation, const QImage &image);
    void enforceBudget();

    mutable QMutex m_mutex; // guards m_bytes
    QHash<QString, std::pair<QByteArray, int>> m_bytes; // bytes, holders
    // GUI thread only.
    std::list<Decoded> m_decoded; // most recently used first
    qint64 m_decodedBytes = 0;
    qint64 m_budget = defaultDecodedBudget;
    QHash<QString, quint64> m_pending; // "key/side" → the generation decoding it
    quint64 m_generation = 0;
};

// One panel picture (a gallery tile, a game's cover, a video's poster),
// painted from PanelMediaLibrary at the size it is drawn. `crop` covers the
// item (tiles, covers), otherwise the whole picture is fitted inside it.
// `radius` rounds the corners; `circle` clips to the largest centred circle.
class ProfilePanelImage : public QQuickPaintedItem
{
    Q_OBJECT
    Q_PROPERTY(QString mediaKey READ mediaKey WRITE setMediaKey NOTIFY mediaKeyChanged)
    Q_PROPERTY(bool crop READ crop WRITE setCrop NOTIFY cropChanged)
    Q_PROPERTY(qreal radius READ radius WRITE setRadius NOTIFY radiusChanged)
    Q_PROPERTY(bool circle READ circle WRITE setCircle NOTIFY circleChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
    // The picture's own aspect (width / height), 0 until it is known.
    Q_PROPERTY(qreal sourceAspect READ sourceAspect NOTIFY readyChanged)

public:
    explicit ProfilePanelImage(QQuickItem *parent = nullptr);

    [[nodiscard]] QString mediaKey() const { return m_key; }
    void setMediaKey(const QString &key);
    [[nodiscard]] bool crop() const noexcept { return m_crop; }
    void setCrop(bool crop);
    [[nodiscard]] qreal radius() const noexcept { return m_radius; }
    void setRadius(qreal radius);
    [[nodiscard]] bool circle() const noexcept { return m_circle; }
    void setCircle(bool circle);
    [[nodiscard]] bool ready() const noexcept { return !m_image.isNull(); }
    [[nodiscard]] qreal sourceAspect() const;

    void paint(QPainter *painter) override;

signals:
    void mediaKeyChanged();
    void cropChanged();
    void radiusChanged();
    void circleChanged();
    void readyChanged();

protected:
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

private:
    void fetch();

    QString m_key;
    bool m_crop = true;
    qreal m_radius = 0;
    bool m_circle = false;
    QImage m_image;
};

} // namespace OpenChat
