#include "render/BubbleBackground.h"

#include <QLinearGradient>
#include <QPainter>
#include <QTransform>

#include <algorithm>

namespace OpenChat {

namespace {

QPainterPath incomingPath(const QRectF &bounds, qreal requestedRadius,
                          qreal requestedTailWidth, qreal requestedTailHeight)
{
    const QRectF rect = bounds.normalized();
    if (rect.isEmpty())
        return {};

    const qreal tailWidth = std::clamp(requestedTailWidth, 0.0, rect.width() / 3.0);
    const qreal bodyLeft = rect.left() + tailWidth;
    const qreal bodyRight = rect.right();
    const qreal radius = std::clamp(
        requestedRadius, 0.0, std::min((bodyRight - bodyLeft) / 2.0, rect.height() / 2.0));
    const qreal tailLow = rect.bottom() - radius - 1.0;
    const qreal maximumTailHeight = std::max(0.0, tailLow - (rect.top() + radius));
    const qreal tailHeight = std::clamp(requestedTailHeight, 0.0, maximumTailHeight);
    const qreal tailHigh = tailLow - tailHeight;

    QPainterPath path;
    path.moveTo(bodyLeft + radius, rect.top());
    path.lineTo(bodyRight - radius, rect.top());
    path.quadTo(bodyRight, rect.top(), bodyRight, rect.top() + radius);
    path.lineTo(bodyRight, rect.bottom() - radius);
    path.quadTo(bodyRight, rect.bottom(), bodyRight - radius, rect.bottom());
    path.lineTo(bodyLeft + radius, rect.bottom());
    path.quadTo(bodyLeft, rect.bottom(), bodyLeft, rect.bottom() - radius);
    path.lineTo(bodyLeft, tailLow);
    path.lineTo(rect.left(), tailLow);
    path.lineTo(bodyLeft, tailHigh);
    path.lineTo(bodyLeft, rect.top() + radius);
    path.quadTo(bodyLeft, rect.top(), bodyLeft + radius, rect.top());
    path.closeSubpath();
    return path;
}

} // namespace

BubbleBackground::BubbleBackground(QQuickItem *parent)
    : QQuickPaintedItem(parent)
{
    setAntialiasing(true);
}

bool BubbleBackground::outgoing() const
{
    return m_outgoing;
}

void BubbleBackground::setOutgoing(bool outgoing)
{
    if (m_outgoing == outgoing)
        return;
    m_outgoing = outgoing;
    emit outgoingChanged();
    repaint();
}

qreal BubbleBackground::radius() const
{
    return m_radius;
}

void BubbleBackground::setRadius(qreal radius)
{
    if (qFuzzyCompare(m_radius, radius))
        return;
    m_radius = std::max(0.0, radius);
    emit radiusChanged();
    repaint();
}

qreal BubbleBackground::tailWidth() const
{
    return m_tailWidth;
}

void BubbleBackground::setTailWidth(qreal width)
{
    if (qFuzzyCompare(m_tailWidth, width))
        return;
    m_tailWidth = std::max(0.0, width);
    emit tailWidthChanged();
    repaint();
}

qreal BubbleBackground::tailHeight() const
{
    return m_tailHeight;
}

void BubbleBackground::setTailHeight(qreal height)
{
    if (qFuzzyCompare(m_tailHeight, height))
        return;
    m_tailHeight = std::max(0.0, height);
    emit tailHeightChanged();
    repaint();
}

QColor BubbleBackground::fillTop() const
{
    return m_fillTop;
}

void BubbleBackground::setFillTop(const QColor &color)
{
    if (m_fillTop == color)
        return;
    m_fillTop = color;
    emit fillTopChanged();
    repaint();
}

QColor BubbleBackground::fillBottom() const
{
    return m_fillBottom;
}

void BubbleBackground::setFillBottom(const QColor &color)
{
    if (m_fillBottom == color)
        return;
    m_fillBottom = color;
    emit fillBottomChanged();
    repaint();
}

QColor BubbleBackground::strokeColor() const
{
    return m_strokeColor;
}

void BubbleBackground::setStrokeColor(const QColor &color)
{
    if (m_strokeColor == color)
        return;
    m_strokeColor = color;
    emit strokeColorChanged();
    repaint();
}

void BubbleBackground::paint(QPainter *painter)
{
    painter->setRenderHint(QPainter::Antialiasing, true);
    const QPainterPath path = makePath(boundingRect(), m_outgoing, m_radius, m_tailWidth,
                                       m_tailHeight);
    QLinearGradient gradient(0.0, 0.0, 0.0, height());
    gradient.setColorAt(0.0, m_fillTop);
    gradient.setColorAt(1.0, m_fillBottom);
    painter->setBrush(gradient);
    QPen outline(m_strokeColor);
    outline.setWidthF(1.0);
    outline.setCosmetic(true);
    painter->setPen(outline);
    painter->drawPath(path);
}

QPainterPath BubbleBackground::makePath(const QRectF &bounds, bool outgoing, qreal radius,
                                        qreal tailWidth, qreal tailHeight)
{
    // QPainter centers a 1 px cosmetic pen on the path. Keeping the path half a
    // pixel inside the item prevents any side of the outline from being clipped.
    const QRectF strokeSafeBounds = bounds.normalized().adjusted(0.5, 0.5, -0.5, -0.5);
    const QPainterPath incoming = incomingPath(strokeSafeBounds, radius, tailWidth, tailHeight);
    if (!outgoing)
        return incoming;

    QTransform mirror;
    mirror.translate(strokeSafeBounds.left() + strokeSafeBounds.right(), 0.0);
    mirror.scale(-1.0, 1.0);
    return mirror.map(incoming);
}

void BubbleBackground::repaint()
{
    update();
}

} // namespace OpenChat
