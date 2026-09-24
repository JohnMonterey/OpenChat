#pragma once

#include <QImage>
#include <QQuickItem>
#include <QString>

namespace OpenChat {

// A profile's background picture (SPEC §8.3) as scene-graph image nodes over
// the backdrop: one texture made from ProfileMediaStore's decoded image,
// scaled by the scene graph (Fill = cover, Fit = contain, Center = 1:1) or
// repeated as a grid of nodes sharing it (Tile). The decoded image and that
// texture are the only copies: nothing is scaled into a per-size cache, and
// scrolling (`scrollOffset`, bound to the page's contentY only when the
// picture is not fixed) moves node rectangles without touching a pixel.
// Around a Fit or Center picture the item is transparent, so the backdrop's
// base colour shows.
class ProfileImageLayer : public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(QString imageKey READ imageKey WRITE setImageKey NOTIFY imageKeyChanged)
    Q_PROPERTY(int imageMode READ imageMode WRITE setImageMode NOTIFY imageModeChanged) // Profile.ImageMode
    Q_PROPERTY(qreal scrollOffset READ scrollOffset WRITE setScrollOffset NOTIFY scrollOffsetChanged)
    // Whether the store has handed over the picture yet.
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)

public:
    // A picture smaller than this on a side is repeated into a block of at
    // least this size before tiling, so a 4K viewport needs at most a few
    // hundred nodes.
    static constexpr int minTileBlock = 256;
    static constexpr int maxTileBlock = 1024;

    explicit ProfileImageLayer(QQuickItem *parent = nullptr);

    [[nodiscard]] QString imageKey() const { return m_imageKey; }
    void setImageKey(const QString &key);
    [[nodiscard]] int imageMode() const noexcept { return m_imageMode; }
    void setImageMode(int mode);
    [[nodiscard]] qreal scrollOffset() const noexcept { return m_scrollOffset; }
    void setScrollOffset(qreal offset);
    [[nodiscard]] bool ready() const noexcept { return !m_image.isNull(); }

    // Tests: how many textures were made from pixels, and how many image
    // nodes the last update laid out.
    [[nodiscard]] int textureUploads() const noexcept { return m_textureUploads; }
    [[nodiscard]] int nodeCount() const noexcept { return m_nodeCount; }

signals:
    void imageKeyChanged();
    void imageModeChanged();
    void scrollOffsetChanged();
    void readyChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

private:
    void fetchImage();

    QString m_imageKey;
    int m_imageMode = 1; // Profile::ImageMode::FillImage
    qreal m_scrollOffset = 0;
    QImage m_image;       // the store's decoded image (shared, not a copy)
    QString m_heldKey;    // the key m_image came from
    quint64 m_imageSerial = 0; // bumps when m_image changes, so the texture is remade
    int m_textureUploads = 0;
    int m_nodeCount = 0;
};

} // namespace OpenChat
