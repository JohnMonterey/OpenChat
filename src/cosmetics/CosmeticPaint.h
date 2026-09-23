#pragma once

#include <QColor>
#include <QImage>
#include <QPainterPath>
#include <QPointF>
#include <QRectF>
#include <QString>

#include <functional>

class QPainter;

// Small painting toolkit shared by the profile cosmetics: blurs for glows,
// deterministic noise for textures, and a few shapes (stars, glints) that
// several items draw. Everything here is pure QPainter/QImage work so it
// renders identically on the software scene graph, which runs no shaders.
namespace OpenChat::CosmeticPaint {

// Device pixels per logical unit for whatever `painter` is drawing on: the
// device's pixel ratio times any scale already in the world transform.
qreal deviceScale(const QPainter &painter);

// A premultiplied ARGB image of the given logical size at `dpr`, cleared to
// transparent.
QImage transparentImage(const QSizeF &logicalSize, qreal dpr);

// Blurs a premultiplied ARGB32 image in place with three box passes, which is
// close enough to a Gaussian of the given radius (device pixels) for glows.
void blurImage(QImage &image, qreal radius);

// Paints `draw` (in the painter's logical coordinates) into an offscreen layer,
// blurs it by `radius` logical pixels and composites it onto `painter`. The
// layer covers `bounds` grown by the blur so nothing is clipped.
void drawBlurred(QPainter &painter, const QRectF &bounds, qreal radius,
                 const std::function<void(QPainter &)> &draw);

// An image covering `area` (logical) whose every device pixel is `shade`
// evaluated at that pixel's logical centre; `shade` returns straight ARGB.
QImage proceduralImage(const QRectF &area, qreal dpr,
                       const std::function<QRgb(qreal x, qreal y)> &shade);

// A cheap, deterministic hash and 2D value noise in [0, 1].
quint32 hash32(quint32 value);
qreal random01(quint32 seed, int index);
qreal valueNoise(qreal x, qreal y, quint32 seed);
qreal fractalNoise(qreal x, qreal y, quint32 seed, int octaves);

QColor mix(const QColor &a, const QColor &b, qreal t);
QColor withAlpha(const QColor &color, qreal alpha);
QColor lighter(const QColor &color, qreal amount);
QColor darker(const QColor &color, qreal amount);

// A `points`-pointed star centred on `centre`, first point straight up unless
// rotated (degrees).
QPainterPath starPath(const QPointF &centre, qreal outerRadius, qreal innerRadius, int points,
                      qreal rotationDegrees = 0.0);
// A four-pointed glint with concave sides, the classic "sparkle".
QPainterPath glintPath(const QPointF &centre, qreal radius, qreal waist = 0.18);
// A glint with a soft halo and a white-hot centre.
void drawGlint(QPainter &painter, const QPointF &centre, qreal radius, const QColor &tint,
               qreal opacity = 1.0);

// A rounded rectangle that tolerates a radius larger than half the side.
QPainterPath roundedRect(const QRectF &rect, qreal radius);

} // namespace OpenChat::CosmeticPaint
