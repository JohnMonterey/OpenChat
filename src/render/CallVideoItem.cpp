#include "render/CallVideoItem.h"

#include <QQuickWindow>
#include <QSGGeometryNode>
#include <QSGImageNode>
#include <QSGRendererInterface>
#include <QSGTexture>
#include <QSGTextureMaterial>

#include <cmath>
#include <vector>

namespace OpenChat {

namespace {

constexpr qreal cornerRadius = 6.0;
constexpr int cornerSegments = 6;
constexpr qreal halfPi = 1.5707963267948966;

// A textured rounded rectangle: the picture, cut to the call screen's rounded
// corners by its own outline rather than by a clip, which the GPU draws for
// free. The node owns the texture it shows.
class RoundedVideoNode final : public QSGGeometryNode
{
public:
    RoundedVideoNode()
        : m_geometry(QSGGeometry::defaultAttributes_TexturedPoint2D(), 0)
    {
        m_geometry.setDrawingMode(QSGGeometry::DrawTriangles);
        setGeometry(&m_geometry);
        setMaterial(&m_material);
        setOpaqueMaterial(&m_opaqueMaterial);
    }

    ~RoundedVideoNode() override { delete m_texture; }

    RoundedVideoNode(const RoundedVideoNode &) = delete;
    RoundedVideoNode &operator=(const RoundedVideoNode &) = delete;

    [[nodiscard]] QSGTexture *texture() const { return m_texture; }

    void setTexture(QSGTexture *texture, bool mipmaps)
    {
        if (texture == m_texture)
            return;
        delete m_texture;
        m_texture = texture;
        for (QSGOpaqueTextureMaterial *material : {static_cast<QSGOpaqueTextureMaterial *>(&m_material), &m_opaqueMaterial}) {
            material->setTexture(texture);
            material->setFiltering(QSGTexture::Linear);
            material->setMipmapFiltering(mipmaps ? QSGTexture::Linear : QSGTexture::None);
        }
        markDirty(DirtyMaterial);
    }

    void setRect(const QRectF &rect, bool mirrored)
    {
        if (rect == m_rect && mirrored == m_mirrored)
            return;
        m_rect = rect;
        m_mirrored = mirrored;
        // The outline, corner by corner, then a fan of triangles from the
        // centre: a convex shape, so the fan covers it exactly.
        const qreal radius = std::min({cornerRadius, rect.width() / 2, rect.height() / 2});
        std::vector<QPointF> outline;
        outline.reserve(4 * (cornerSegments + 1));
        const QPointF centres[4] = {
            {rect.right() - radius, rect.top() + radius},
            {rect.right() - radius, rect.bottom() - radius},
            {rect.left() + radius, rect.bottom() - radius},
            {rect.left() + radius, rect.top() + radius},
        };
        for (int corner = 0; corner < 4; ++corner) {
            const qreal start = -halfPi + corner * halfPi;
            for (int step = 0; step <= cornerSegments; ++step) {
                const qreal angle = start + halfPi * step / cornerSegments;
                outline.emplace_back(centres[corner].x() + radius * std::cos(angle),
                                     centres[corner].y() + radius * std::sin(angle));
            }
        }
        const int triangles = int(outline.size());
        m_geometry.allocate(triangles * 3);
        QSGGeometry::TexturedPoint2D *vertices = m_geometry.vertexDataAsTexturedPoint2D();
        const QPointF centre = rect.center();
        auto set = [&](QSGGeometry::TexturedPoint2D &vertex, const QPointF &point) {
            qreal u = rect.width() > 0 ? (point.x() - rect.left()) / rect.width() : 0.0;
            const qreal v = rect.height() > 0 ? (point.y() - rect.top()) / rect.height() : 0.0;
            if (m_mirrored)
                u = 1.0 - u;
            vertex.set(float(point.x()), float(point.y()), float(u), float(v));
        };
        for (int index = 0; index < triangles; ++index) {
            set(vertices[3 * index], centre);
            set(vertices[3 * index + 1], outline[size_t(index)]);
            set(vertices[3 * index + 2], outline[size_t((index + 1) % triangles)]);
        }
        markDirty(DirtyGeometry);
    }

private:
    QSGGeometry m_geometry;
    QSGTextureMaterial m_material;
    QSGOpaqueTextureMaterial m_opaqueMaterial;
    QSGTexture *m_texture = nullptr;
    QRectF m_rect;
    bool m_mirrored = false;
};

} // namespace

CallVideoItem::CallVideoItem(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    // A view that was hidden was never updated while it was hidden, so it has
    // to catch up when it comes back.
    connect(this, &QQuickItem::visibleChanged, this, [this] {
        if (isVisible())
            update();
    });
}

const QImage *CallVideoItem::sourceImage() const
{
    if (m_canvas && !m_canvas->isEmpty())
        return &m_canvas->image();
    return m_frame.isNull() ? nullptr : &m_frame;
}

double CallVideoItem::sourceAspect() const
{
    const QImage *source = sourceImage();
    if (source == nullptr || source->height() <= 0)
        return 16.0 / 9.0;
    return double(source->width()) / double(source->height());
}

void CallVideoItem::requestRepaint()
{
    if (isVisible() && !m_paused && width() > 0 && height() > 0)
        update();
}

void CallVideoItem::setFrame(const QImage &frame)
{
    const QSize previous = m_frame.size();
    m_frame = frame;
    requestRepaint();
    emit frameChanged();
    if (previous != m_frame.size() && !m_canvas)
        emit sourceAspectChanged();
}

void CallVideoItem::setCanvas(const QVariant &value)
{
    ScreenCanvasPtr next;
    if (value.canConvert<ScreenCanvasPtr>())
        next = value.value<ScreenCanvasPtr>();
    applyCanvas(std::move(next));
}

void CallVideoItem::applyCanvas(ScreenCanvasPtr next)
{
    const bool replaced = next != m_canvas;
    const QRect dirty = next ? next->dirtyRect() : QRect();
    m_canvas = std::move(next);
    emit canvasChanged();

    const QSize size = m_canvas ? m_canvas->size() : QSize();
    if (size != m_lastSourceSize) {
        m_lastSourceSize = size;
        emit sourceAspectChanged();
    }
    // A heartbeat from a motionless desktop carries no dirty region at all and
    // costs the scene graph nothing.
    if (!replaced && m_canvas && dirty.isNull())
        return;
    requestRepaint();
}

void CallVideoItem::setMirrored(bool value)
{
    if (m_mirrored == value)
        return;
    m_mirrored = value;
    if (isVisible())
        update();
    emit mirroredChanged();
}

void CallVideoItem::setSource(CallVideoItem *source)
{
    if (m_source == source || source == this)
        return;
    if (m_source)
        disconnect(m_source, nullptr, this, nullptr);
    m_source = source;
    if (m_source) {
        connect(m_source, &CallVideoItem::frameChanged, this,
                [this] { setFrame(m_source->m_frame); });
        connect(m_source, &CallVideoItem::canvasChanged, this,
                [this] { applyCanvas(m_source->m_canvas); });
        connect(m_source, &CallVideoItem::mirroredChanged, this,
                [this] { setMirrored(m_source->m_mirrored); });
        // A source that goes away (a participant leaving a group call) leaves
        // the copy showing nothing, not pointing at freed memory.
        connect(m_source, &QObject::destroyed, this, [this] {
            m_source = nullptr;
            setFrame(QImage());
            applyCanvas({});
            emit sourceChanged();
        });
        setFrame(m_source->m_frame);
        applyCanvas(m_source->m_canvas);
        setMirrored(m_source->m_mirrored);
    } else {
        setFrame(QImage());
        applyCanvas({});
    }
    emit sourceChanged();
}

void CallVideoItem::setPaused(bool value)
{
    if (m_paused == value)
        return;
    m_paused = value;
    // Whatever arrived while paused was never uploaded.
    if (!m_paused && isVisible())
        update();
    emit pausedChanged();
}

void CallVideoItem::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size())
        update();
}

QRectF CallVideoItem::targetRect(QSize sourceSize) const
{
    const QSizeF size = QSizeF(sourceSize).scaled(boundingRect().size(), Qt::KeepAspectRatio);
    return QRectF((width() - size.width()) / 2, (height() - size.height()) / 2, size.width(),
                  size.height());
}

QSGNode *CallVideoItem::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    const QImage *source = sourceImage();
    QQuickWindow *view = window();
    if (source == nullptr || view == nullptr || width() <= 0 || height() <= 0) {
        delete oldNode;
        m_uploadedRevision = 0;
        m_uploadedFrameKey = 0;
        return nullptr;
    }
    const QRectF target = targetRect(source->size());
    const bool software = view->rendererInterface()->graphicsApi() == QSGRendererInterface::Software;
    // Shown much smaller than it is — a desktop in a side panel — the picture
    // gets mipmaps, so text shrinks smoothly instead of shimmering.
    const bool mipmaps = !software && target.width() < source->width() * 0.6;

    auto *rounded = software ? nullptr : dynamic_cast<RoundedVideoNode *>(oldNode);
    auto *plain = software ? dynamic_cast<QSGImageNode *>(oldNode) : nullptr;
    if (rounded == nullptr && plain == nullptr) {
        delete oldNode;
        oldNode = nullptr;
        m_uploadedRevision = 0;
        m_uploadedFrameKey = 0;
    }
    const bool haveTexture = rounded ? rounded->texture() != nullptr
                                     : (plain ? plain->texture() != nullptr : false);
    const bool changed = m_canvas ? m_canvas->revision() != m_uploadedRevision
                                  : (m_uploadedRevision != 0 || m_frame.cacheKey() != m_uploadedFrameKey);
    QSGTexture *texture = nullptr;
    if (!haveTexture || mipmaps != m_uploadedMipmaps || (changed && !m_paused)) {
        QQuickWindow::CreateTextureOptions options = QQuickWindow::TextureIsOpaque;
        if (mipmaps)
            options |= QQuickWindow::TextureHasMipmaps;
        // Shallow: the texture holds the picture until it is uploaded, later
        // in this frame, and then lets go of it.
        texture = view->createTextureFromImage(*source, options);
        m_uploadedRevision = m_canvas ? m_canvas->revision() : 0;
        m_uploadedFrameKey = m_canvas ? 0 : m_frame.cacheKey();
        m_uploadedMipmaps = mipmaps;
    }

    if (software) {
        if (plain == nullptr) {
            plain = view->createImageNode();
            plain->setOwnsTexture(true);
        }
        if (texture != nullptr)
            plain->setTexture(texture);
        if (plain->texture() == nullptr) {
            delete plain;
            return nullptr;
        }
        plain->setRect(target);
        plain->setSourceRect(QRectF(QPointF(0, 0), plain->texture()->textureSize()));
        plain->setFiltering(QSGTexture::Linear);
        plain->setTextureCoordinatesTransform(m_mirrored ? QSGImageNode::MirrorHorizontally
                                                         : QSGImageNode::NoTransform);
        return plain;
    }

    if (rounded == nullptr)
        rounded = new RoundedVideoNode;
    if (texture != nullptr)
        rounded->setTexture(texture, mipmaps);
    if (rounded->texture() == nullptr) {
        delete rounded;
        return nullptr;
    }
    rounded->setRect(target, m_mirrored);
    return rounded;
}

} // namespace OpenChat
