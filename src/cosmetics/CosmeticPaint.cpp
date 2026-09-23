#include "cosmetics/CosmeticPaint.h"

#include <QPainter>
#include <QRadialGradient>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <vector>

namespace OpenChat::CosmeticPaint {

qreal deviceScale(const QPainter &painter)
{
    const qreal dpr = painter.device() ? painter.device()->devicePixelRatioF() : 1.0;
    const qreal world = std::sqrt(std::abs(painter.worldTransform().determinant()));
    return std::max(0.25, dpr * (world > 0.0 ? world : 1.0));
}

QImage transparentImage(const QSizeF &logicalSize, qreal dpr)
{
    const QSize pixels(std::max(1, qCeil(logicalSize.width() * dpr)),
                       std::max(1, qCeil(logicalSize.height() * dpr)));
    QImage image(pixels, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::transparent);
    return image;
}

namespace {

// One horizontal box pass of width 2r+1 over premultiplied ARGB32 rows, from
// `src` into `dst`, edges clamped to transparent.
void boxPassHorizontal(const QImage &src, QImage &dst, int r)
{
    const int w = src.width();
    const int h = src.height();
    const int window = 2 * r + 1;
    for (int y = 0; y < h; ++y) {
        const auto *in = reinterpret_cast<const quint32 *>(src.constScanLine(y));
        auto *out = reinterpret_cast<quint32 *>(dst.scanLine(y));
        int sa = 0, sr = 0, sg = 0, sb = 0;
        // Prime the window with the pixels right of x = 0 (left side is empty).
        for (int i = 0; i <= r && i < w; ++i) {
            const quint32 p = in[i];
            sa += int(p >> 24);
            sr += int((p >> 16) & 0xff);
            sg += int((p >> 8) & 0xff);
            sb += int(p & 0xff);
        }
        for (int x = 0; x < w; ++x) {
            out[x] = (quint32(sa / window) << 24) | (quint32(sr / window) << 16)
                     | (quint32(sg / window) << 8) | quint32(sb / window);
            const int add = x + r + 1;
            const int remove = x - r;
            if (add < w) {
                const quint32 p = in[add];
                sa += int(p >> 24);
                sr += int((p >> 16) & 0xff);
                sg += int((p >> 8) & 0xff);
                sb += int(p & 0xff);
            }
            if (remove >= 0) {
                const quint32 p = in[remove];
                sa -= int(p >> 24);
                sr -= int((p >> 16) & 0xff);
                sg -= int((p >> 8) & 0xff);
                sb -= int(p & 0xff);
            }
        }
    }
}

void boxPassVertical(const QImage &src, QImage &dst, int r)
{
    const int w = src.width();
    const int h = src.height();
    const int window = 2 * r + 1;
    const qsizetype stride = src.bytesPerLine() / 4;
    const auto *inBase = reinterpret_cast<const quint32 *>(src.constBits());
    auto *outBase = reinterpret_cast<quint32 *>(dst.bits());
    const qsizetype outStride = dst.bytesPerLine() / 4;
    for (int x = 0; x < w; ++x) {
        int sa = 0, sr = 0, sg = 0, sb = 0;
        for (int i = 0; i <= r && i < h; ++i) {
            const quint32 p = inBase[i * stride + x];
            sa += int(p >> 24);
            sr += int((p >> 16) & 0xff);
            sg += int((p >> 8) & 0xff);
            sb += int(p & 0xff);
        }
        for (int y = 0; y < h; ++y) {
            outBase[y * outStride + x] = (quint32(sa / window) << 24)
                                         | (quint32(sr / window) << 16)
                                         | (quint32(sg / window) << 8) | quint32(sb / window);
            const int add = y + r + 1;
            const int remove = y - r;
            if (add < h) {
                const quint32 p = inBase[add * stride + x];
                sa += int(p >> 24);
                sr += int((p >> 16) & 0xff);
                sg += int((p >> 8) & 0xff);
                sb += int(p & 0xff);
            }
            if (remove >= 0) {
                const quint32 p = inBase[remove * stride + x];
                sa -= int(p >> 24);
                sr -= int((p >> 16) & 0xff);
                sg -= int((p >> 8) & 0xff);
                sb -= int(p & 0xff);
            }
        }
    }
}

} // namespace

void blurImage(QImage &image, qreal radius)
{
    if (radius <= 0.25 || image.isNull())
        return;
    if (image.format() != QImage::Format_ARGB32_Premultiplied)
        image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    // Three box passes whose combined variance matches a Gaussian of sigma
    // radius / 2 — soft, with no visible box edges.
    const qreal sigma = radius / 2.0;
    const int box = std::max(1, int(std::lround(std::sqrt(12.0 * sigma * sigma / 3.0 + 1.0))));
    const int r = std::max(1, box / 2);
    QImage scratch(image.size(), QImage::Format_ARGB32_Premultiplied);
    scratch.setDevicePixelRatio(image.devicePixelRatio());
    for (int pass = 0; pass < 3; ++pass) {
        boxPassHorizontal(image, scratch, r);
        boxPassVertical(scratch, image, r);
    }
}

void drawBlurred(QPainter &painter, const QRectF &bounds, qreal radius,
                 const std::function<void(QPainter &)> &draw)
{
    const qreal effectiveDpr = deviceScale(painter);
    const QRectF area = bounds.adjusted(-radius * 1.6, -radius * 1.6, radius * 1.6, radius * 1.6);
    QImage layer = transparentImage(area.size(), effectiveDpr);
    {
        QPainter lp(&layer);
        lp.setRenderHint(QPainter::Antialiasing, true);
        lp.translate(-area.topLeft());
        draw(lp);
    }
    blurImage(layer, radius * effectiveDpr);
    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(area.topLeft(), layer);
    painter.restore();
}

QImage proceduralImage(const QRectF &area, qreal dpr,
                       const std::function<QRgb(qreal x, qreal y)> &shade)
{
    QImage image = transparentImage(area.size(), dpr);
    for (int y = 0; y < image.height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        const qreal ly = area.top() + (y + 0.5) / dpr;
        for (int x = 0; x < image.width(); ++x)
            line[x] = qPremultiply(shade(area.left() + (x + 0.5) / dpr, ly));
    }
    return image;
}

quint32 hash32(quint32 value)
{
    value ^= value >> 16;
    value *= 0x7feb352dU;
    value ^= value >> 15;
    value *= 0x846ca68bU;
    value ^= value >> 16;
    return value;
}

qreal random01(quint32 seed, int index)
{
    return qreal(hash32(seed * 0x9e3779b9U + quint32(index) * 0x85ebca6bU + 0x165667b1U))
           / qreal(0xffffffffU);
}

qreal valueNoise(qreal x, qreal y, quint32 seed)
{
    const int xi = int(std::floor(x));
    const int yi = int(std::floor(y));
    const qreal xf = x - xi;
    const qreal yf = y - yi;
    const auto corner = [seed](int cx, int cy) {
        return qreal(hash32(quint32(cx) * 0x8da6b343U ^ quint32(cy) * 0xd8163841U ^ seed))
               / qreal(0xffffffffU);
    };
    const qreal u = xf * xf * (3.0 - 2.0 * xf);
    const qreal v = yf * yf * (3.0 - 2.0 * yf);
    const qreal a = corner(xi, yi);
    const qreal b = corner(xi + 1, yi);
    const qreal c = corner(xi, yi + 1);
    const qreal d = corner(xi + 1, yi + 1);
    return (a * (1 - u) + b * u) * (1 - v) + (c * (1 - u) + d * u) * v;
}

qreal fractalNoise(qreal x, qreal y, quint32 seed, int octaves)
{
    qreal sum = 0.0;
    qreal amplitude = 0.5;
    qreal norm = 0.0;
    for (int i = 0; i < octaves; ++i) {
        sum += amplitude * valueNoise(x, y, seed + quint32(i) * 131U);
        norm += amplitude;
        x *= 2.03;
        y *= 2.03;
        amplitude *= 0.5;
    }
    return norm > 0 ? sum / norm : 0.0;
}

QColor mix(const QColor &a, const QColor &b, qreal t)
{
    t = std::clamp(t, 0.0, 1.0);
    return QColor::fromRgbF(float(a.redF() + (b.redF() - a.redF()) * t),
                            float(a.greenF() + (b.greenF() - a.greenF()) * t),
                            float(a.blueF() + (b.blueF() - a.blueF()) * t),
                            float(a.alphaF() + (b.alphaF() - a.alphaF()) * t));
}

QColor withAlpha(const QColor &color, qreal alpha)
{
    QColor c = color;
    c.setAlphaF(float(std::clamp(alpha, 0.0, 1.0)));
    return c;
}

QColor lighter(const QColor &color, qreal amount)
{
    return mix(color, QColor(255, 255, 255, color.alpha()), amount);
}

QColor darker(const QColor &color, qreal amount)
{
    return mix(color, QColor(0, 0, 0, color.alpha()), amount);
}

QPainterPath starPath(const QPointF &centre, qreal outerRadius, qreal innerRadius, int points,
                      qreal rotationDegrees)
{
    QPainterPath path;
    const int corners = points * 2;
    for (int i = 0; i < corners; ++i) {
        const qreal angle = qDegreesToRadians(rotationDegrees - 90.0 + 180.0 * i / points);
        const qreal radius = (i % 2 == 0) ? outerRadius : innerRadius;
        const QPointF p(centre.x() + radius * std::cos(angle), centre.y() + radius * std::sin(angle));
        if (i == 0)
            path.moveTo(p);
        else
            path.lineTo(p);
    }
    path.closeSubpath();
    return path;
}

QPainterPath glintPath(const QPointF &centre, qreal radius, qreal waist)
{
    // Four points joined by curves that pinch towards the centre.
    const qreal w = radius * waist;
    QPainterPath path;
    const QPointF top(centre.x(), centre.y() - radius);
    const QPointF right(centre.x() + radius, centre.y());
    const QPointF bottom(centre.x(), centre.y() + radius);
    const QPointF left(centre.x() - radius, centre.y());
    path.moveTo(top);
    path.quadTo(QPointF(centre.x() + w, centre.y() - w), right);
    path.quadTo(QPointF(centre.x() + w, centre.y() + w), bottom);
    path.quadTo(QPointF(centre.x() - w, centre.y() + w), left);
    path.quadTo(QPointF(centre.x() - w, centre.y() - w), top);
    path.closeSubpath();
    return path;
}

void drawGlint(QPainter &painter, const QPointF &centre, qreal radius, const QColor &tint,
               qreal opacity)
{
    painter.save();
    painter.setOpacity(painter.opacity() * opacity);
    painter.setPen(Qt::NoPen);
    QRadialGradient halo(centre, radius * 0.9);
    halo.setColorAt(0.0, withAlpha(tint, 0.75));
    halo.setColorAt(1.0, withAlpha(tint, 0.0));
    painter.setBrush(halo);
    painter.drawEllipse(centre, radius * 0.9, radius * 0.9);
    QRadialGradient core(centre, radius);
    core.setColorAt(0.0, QColor(255, 255, 255));
    core.setColorAt(0.45, lighter(tint, 0.6));
    core.setColorAt(1.0, withAlpha(tint, 0.2));
    painter.setBrush(core);
    painter.drawPath(glintPath(centre, radius));
    painter.restore();
}

QPainterPath roundedRect(const QRectF &rect, qreal radius)
{
    QPainterPath path;
    const qreal r = std::clamp(radius, 0.0, std::min(rect.width(), rect.height()) / 2.0);
    path.addRoundedRect(rect, r, r);
    return path;
}

} // namespace OpenChat::CosmeticPaint
