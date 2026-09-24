#include "profile/ProfileImageLayerItem.h"

#include "domain/ProfilePage.h"
#include "profile/ProfileMediaStore.h"

#include <QPainter>
#include <QQuickWindow>
#include <QSGImageNode>
#include <QSGTexture>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace OpenChat {

namespace {

// The layer's root: owns the one texture every image node below it shares,
// so the texture dies with the node tree, on the render thread.
class LayerNode final : public QSGNode
{
public:
    std::unique_ptr<QSGTexture> texture;
    quint64 serial = 0;
    bool tiled = false;
    QSizeF block; // the texture's size in logical pixels (1 picture pixel = 1)
};

struct Placement final {
    QRectF target;
    QRectF source;
};

// `target` showing `source` of the texture, cut to what lies inside `bounds`
// (the item never draws outside itself).
bool clipped(const QRectF &target, const QRectF &source, const QRectF &bounds, Placement &out)
{
    const QRectF visible = target.intersected(bounds);
    if (visible.isEmpty() || target.width() <= 0 || target.height() <= 0)
        return false;
    const qreal sx = source.width() / target.width();
    const qreal sy = source.height() / target.height();
    out.target = visible;
    out.source = QRectF(source.x() + (visible.x() - target.x()) * sx, source.y() + (visible.y() - target.y()) * sy,
                        visible.width() * sx, visible.height() * sy);
    return true;
}

// A tiny picture repeated into a block of at least minTileBlock a side (never
// past maxTileBlock), so tiling it takes few nodes.
QImage tileBlock(const QImage &image)
{
    const int across = std::clamp(int(std::ceil(double(ProfileImageLayer::minTileBlock) / image.width())), 1,
                                  std::max(1, ProfileImageLayer::maxTileBlock / image.width()));
    const int down = std::clamp(int(std::ceil(double(ProfileImageLayer::minTileBlock) / image.height())), 1,
                                std::max(1, ProfileImageLayer::maxTileBlock / image.height()));
    if (across == 1 && down == 1)
        return image;
    QImage block(image.width() * across, image.height() * down, image.format());
    QPainter painter(&block);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    for (int y = 0; y < down; ++y) {
        for (int x = 0; x < across; ++x)
            painter.drawImage(x * image.width(), y * image.height(), image);
    }
    return block;
}

} // namespace

ProfileImageLayer::ProfileImageLayer(QQuickItem *parent) : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    connect(&ProfileMediaStore::instance(), &ProfileMediaStore::imageReady, this, [this](const QString &key) {
        if (key == m_imageKey)
            fetchImage();
    });
}

void ProfileImageLayer::setImageKey(const QString &key)
{
    if (key == m_imageKey)
        return;
    m_imageKey = key;
    emit imageKeyChanged();
    fetchImage();
}

void ProfileImageLayer::setImageMode(int mode)
{
    if (mode < 0 || mode > int(Profile::ImageMode::CenterImage) || mode == m_imageMode)
        return;
    m_imageMode = mode;
    emit imageModeChanged();
    update();
}

void ProfileImageLayer::setScrollOffset(qreal offset)
{
    if (qFuzzyCompare(offset + 1.0, m_scrollOffset + 1.0))
        return;
    m_scrollOffset = offset;
    emit scrollOffsetChanged();
    update();
}

void ProfileImageLayer::fetchImage()
{
    const bool wasReady = ready();
    const std::optional<QImage> image =
        m_imageKey.isEmpty() ? std::nullopt : ProfileMediaStore::instance().image(m_imageKey);
    if (image) {
        if (image->cacheKey() != m_image.cacheKey()) {
            m_image = *image;
            ++m_imageSerial;
        }
        m_heldKey = m_imageKey;
    } else if (m_heldKey != m_imageKey && !m_image.isNull()) {
        // A different picture that is not decoded yet: show the base colour
        // until imageReady. (A picture the store merely evicted keeps
        // showing: this item still holds its shared pixels.)
        m_image = QImage();
        m_heldKey.clear();
        ++m_imageSerial;
    }
    if (wasReady != ready())
        emit readyChanged();
    update();
}

void ProfileImageLayer::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size())
        update();
}

QSGNode *ProfileImageLayer::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    auto *root = static_cast<LayerNode *>(oldNode);
    if (m_image.isNull() || width() <= 0 || height() <= 0 || !window()) {
        delete root;
        m_nodeCount = 0;
        return nullptr;
    }
    if (!root)
        root = new LayerNode;

    const bool tiled = m_imageMode == int(Profile::ImageMode::TileImage);
    if (!root->texture || root->serial != m_imageSerial || root->tiled != tiled) {
        const QImage pixels = tiled ? tileBlock(m_image) : m_image;
        root->texture.reset(window()->createTextureFromImage(pixels));
        root->texture->setFiltering(QSGTexture::Linear);
        root->texture->setHorizontalWrapMode(QSGTexture::ClampToEdge);
        root->texture->setVerticalWrapMode(QSGTexture::ClampToEdge);
        root->serial = m_imageSerial;
        root->tiled = tiled;
        root->block = QSizeF(pixels.size());
        ++m_textureUploads;
        // Every node still points at the old texture: rebuild them below.
        while (QSGNode *child = root->firstChild()) {
            root->removeChildNode(child);
            delete child;
        }
    }

    const QRectF bounds(0, 0, width(), height());
    const QSizeF picture(m_image.size());
    const QRectF whole(QPointF(0, 0), root->block);
    std::vector<Placement> placements;
    Placement placement;
    switch (Profile::ImageMode(m_imageMode)) {
    case Profile::ImageMode::FillImage:
    case Profile::ImageMode::FitImage: {
        const qreal sx = bounds.width() / picture.width();
        const qreal sy = bounds.height() / picture.height();
        const qreal scale = m_imageMode == int(Profile::ImageMode::FillImage) ? std::max(sx, sy) : std::min(sx, sy);
        const QSizeF drawn = picture * scale;
        const QRectF target((bounds.width() - drawn.width()) / 2, (bounds.height() - drawn.height()) / 2 - m_scrollOffset,
                            drawn.width(), drawn.height());
        if (clipped(target, whole, bounds, placement))
            placements.push_back(placement);
        break;
    }
    case Profile::ImageMode::CenterImage: {
        const QRectF target((bounds.width() - picture.width()) / 2,
                            (bounds.height() - picture.height()) / 2 - m_scrollOffset, picture.width(),
                            picture.height());
        if (clipped(target, whole, bounds, placement))
            placements.push_back(placement);
        break;
    }
    case Profile::ImageMode::TileImage: {
        const qreal bw = root->block.width();
        const qreal bh = root->block.height();
        // The grid moves with the page; the first row starts at or above 0.
        qreal top = -std::fmod(m_scrollOffset, bh);
        if (top > 0)
            top -= bh;
        for (qreal y = top; y < bounds.height(); y += bh) {
            for (qreal x = 0; x < bounds.width(); x += bw) {
                if (clipped(QRectF(x, y, bw, bh), whole, bounds, placement))
                    placements.push_back(placement);
            }
        }
        break;
    }
    }

    // Reuse the nodes already there; only rectangles change on a scroll.
    int index = 0;
    QSGNode *child = root->firstChild();
    for (const Placement &p : placements) {
        auto *node = static_cast<QSGImageNode *>(child);
        if (!node) {
            node = window()->createImageNode();
            node->setTexture(root->texture.get());
            node->setOwnsTexture(false);
            node->setFiltering(QSGTexture::Linear);
            root->appendChildNode(node);
        } else {
            child = child->nextSibling();
        }
        node->setRect(p.target);
        node->setSourceRect(p.source);
        ++index;
    }
    while (child) {
        QSGNode *next = child->nextSibling();
        root->removeChildNode(child);
        delete child;
        child = next;
    }
    m_nodeCount = index;
    return root;
}

} // namespace OpenChat
