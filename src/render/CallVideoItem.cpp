#include "render/CallVideoItem.h"

#include <QPainter>
#include <QPainterPath>

namespace OpenChat {

CallVideoItem::CallVideoItem(QQuickItem *parent)
    : QQuickPaintedItem(parent)
{
    // A view that was hidden was never repainted while it was hidden, so it has
    // to be repainted whole when it comes back.
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

void CallVideoItem::setFrame(const QImage &frame)
{
    const QSize previous = m_frame.size();
    m_frame = frame;
    if (isVisible())
        update();
    emit frameChanged();
    if (previous != m_frame.size() && !m_canvas)
        emit sourceAspectChanged();
}

void CallVideoItem::setCanvas(const QVariant &value)
{
    ScreenCanvasPtr next;
    if (value.canConvert<ScreenCanvasPtr>())
        next = value.value<ScreenCanvasPtr>();
    const bool replaced = next != m_canvas;
    const QRect dirty = next ? next->dirtyRect() : QRect();
    m_canvas = std::move(next);
    emit canvasChanged();

    const QSize size = m_canvas ? m_canvas->size() : QSize();
    if (size != m_lastSourceSize) {
        m_lastSourceSize = size;
        emit sourceAspectChanged();
    }
    // Nothing is drawn for a view nobody can see. The sender is told the same
    // thing separately, so it stops encoding for it as well.
    if (!isVisible() || width() <= 0 || height() <= 0)
        return;
    if (replaced || !m_canvas) {
        update();
        return;
    }
    // A heartbeat from a motionless desktop carries no dirty region at all, and
    // costs the scene graph nothing.
    if (dirty.isNull())
        return;
    // Several updates can land between two frames; QQuickPaintedItem unions the
    // rectangles, so the eventual repaint covers all of them exactly.
    update(mapFromSource(dirty));
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

QRectF CallVideoItem::targetRect(QSize sourceSize) const
{
    const QSizeF size = QSizeF(sourceSize).scaled(boundingRect().size(), Qt::KeepAspectRatio);
    return QRectF((width() - size.width()) / 2, (height() - size.height()) / 2, size.width(),
                  size.height());
}

QRect CallVideoItem::mapFromSource(const QRect &rect) const
{
    const QImage *source = sourceImage();
    if (source == nullptr || source->width() <= 0 || source->height() <= 0)
        return boundingRect().toAlignedRect();
    const QRectF target = targetRect(source->size());
    const double scaleX = target.width() / source->width();
    const double scaleY = target.height() / source->height();
    QRectF mapped(target.x() + rect.x() * scaleX, target.y() + rect.y() * scaleY,
                  rect.width() * scaleX, rect.height() * scaleY);
    if (m_mirrored)
        mapped.moveLeft(width() - mapped.right());
    // One pixel of slack in every direction: smooth scaling reads just outside
    // the rectangle it writes, so a tight repaint would leave a visible seam.
    return mapped.adjusted(-1, -1, 1, 1).toAlignedRect();
}

void CallVideoItem::paint(QPainter *painter)
{
    const QImage *source = sourceImage();
    if (source == nullptr)
        return;
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setRenderHint(QPainter::SmoothPixmapTransform);
    const QRectF target = targetRect(source->size());
    QPainterPath clip;
    clip.addRoundedRect(target, 6, 6);
    painter->setClipPath(clip, Qt::IntersectClip);
    if (m_mirrored) {
        painter->translate(width(), 0);
        painter->scale(-1, 1);
    }
    // The painter is already clipped to whatever region was marked dirty, so a
    // small change scales and blits a small rectangle rather than the desktop.
    painter->drawImage(target, *source);
}

} // namespace OpenChat
