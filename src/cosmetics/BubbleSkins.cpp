#include "cosmetics/BubbleSkins.h"

#include "cosmetics/ProceduralNoise.h"

#include <QLinearGradient>
#include <QPainter>
#include <QRadialGradient>
#include <QThreadPool>
#include <QtMath>

#include <array>
#include <cmath>
#include <mutex>

namespace OpenChat {

namespace {

using namespace Procedural;

// Tiles are square and generated at twice the logical size, so the grain,
// glitter and hairlines stay crisp on high-DPI screens; a 1x copy is derived
// once for standard-density screens.
constexpr int kTileLogical = 384;
constexpr int kTileScale = 2;
constexpr int kTilePixels = kTileLogical * kTileScale;

QColor argb(quint32 value)
{
    return QColor::fromRgba(value);
}

// Deterministic per-bubble dice: the same bubble size always gets the same
// sparkles, so a repaint never makes the decoration jump.
class Dice
{
public:
    explicit Dice(std::uint32_t seed) : m_state(hash32(seed ^ 0xa511e9b3U)) {}

    float next()
    {
        m_state = hash32(m_state + 0x9e3779b9U);
        return unit(m_state);
    }

    qreal range(qreal low, qreal high) { return low + (high - low) * next(); }

private:
    std::uint32_t m_state;
};

inline int channel(float value, float dither)
{
    return static_cast<int>(clamp01(value + dither) * 255.0f + 0.5f);
}

// Packs a colour with ±½ LSB of hashed dither, which keeps the slow gradients
// in the darker materials from banding.
inline QRgb pack(Rgb c, int x, int y)
{
    const float dither = (unit(hash2(x, y, 0x2545f491U)) - 0.5f) / 255.0f;
    return qRgb(channel(c.r, dither), channel(c.g, dither), channel(c.b, dither));
}

inline QRgb packPremultiplied(Rgb c, float alpha, int x, int y)
{
    const float dither = (unit(hash2(x, y, 0x68e31da4U)) - 0.5f) / 255.0f;
    const float a = clamp01(alpha);
    return qRgba(channel(c.r * a, dither), channel(c.g * a, dither), channel(c.b * a, dither),
                 channel(a, dither));
}

template <typename Pixel>
QImage generateTile(QImage::Format format, const Pixel &pixel)
{
    QImage image(kTilePixels, kTilePixels, format);
    uchar *bits = image.bits();
    const qsizetype stride = image.bytesPerLine();
    parallelRows(kTilePixels, [&](int y) {
        auto *line = reinterpret_cast<QRgb *>(bits + stride * y);
        const float v = (static_cast<float>(y) + 0.5f) / kTilePixels;
        for (int x = 0; x < kTilePixels; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / kTilePixels;
            line[x] = pixel(u, v, x, y);
        }
    });
    image.setDevicePixelRatio(kTileScale);
    return image;
}

// Draws the tile repeated across `area`, starting `offset` logical pixels into it.
void drawTiled(QPainter &painter, const QRectF &area, const QImage &tile, QPointF offset)
{
    const qreal size = tile.width() / tile.devicePixelRatio();
    const qreal startX = area.left() - std::fmod(offset.x(), size);
    const qreal startY = area.top() - std::fmod(offset.y(), size);
    for (qreal y = startY; y < area.bottom(); y += size) {
        for (qreal x = startX; x < area.right(); x += size)
            painter.drawImage(QRectF(x, y, size, size), tile);
    }
}

QPointF tileOffset(Dice &dice)
{
    return {std::floor(dice.range(0.0, kTileLogical)), std::floor(dice.range(0.0, kTileLogical))};
}

// Stacked strokes centred on the outline; the outer halves are cut away with
// everything else outside the bubble, leaving a soft glow inside the edge.
void innerGlow(QPainter &painter, const QPainterPath &path, const QColor &color, qreal depth,
               int steps = 4)
{
    painter.save();
    painter.setBrush(Qt::NoBrush);
    for (int step = steps; step >= 1; --step) {
        QColor layer = color;
        layer.setAlphaF(color.alphaF() / static_cast<float>(steps));
        QPen pen(layer, 2.0 * depth * step / steps);
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);
        painter.drawPath(path);
    }
    painter.restore();
}

void strokeInside(QPainter &painter, const QPainterPath &path, const QBrush &brush, qreal width)
{
    painter.save();
    painter.setBrush(Qt::NoBrush);
    QPen pen(brush, 2.0 * width);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.drawPath(path);
    painter.restore();
}

void outline(QPainter &painter, const QPainterPath &path, const QBrush &brush, qreal width = 1.0)
{
    painter.save();
    painter.setBrush(Qt::NoBrush);
    QPen pen(brush, width);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.drawPath(path);
    painter.restore();
}

// The upper highlight of an Aero surface: follows the body's top corners and
// ends in a gently bowed lower edge.
QPainterPath glossPath(const QRectF &body, qreal radius, qreal depth, qreal inset)
{
    const QRectF r = body.adjusted(inset, inset, -inset, 0.0);
    const qreal corner = std::max(0.0, radius - inset);
    const qreal bottom = r.top() + depth;
    QPainterPath gloss;
    gloss.moveTo(r.left(), bottom);
    gloss.lineTo(r.left(), r.top() + corner);
    gloss.quadTo(r.left(), r.top(), r.left() + corner, r.top());
    gloss.lineTo(r.right() - corner, r.top());
    gloss.quadTo(r.right(), r.top(), r.right(), r.top() + corner);
    gloss.lineTo(r.right(), bottom);
    gloss.cubicTo(r.right() - r.width() * 0.3, bottom - depth * 0.22, r.left() + r.width() * 0.3,
                  bottom - depth * 0.22, r.left(), bottom);
    gloss.closeSubpath();
    return gloss;
}

// A four-pointed glint with a soft halo.
void drawGlint(QPainter &painter, QPointF centre, qreal size, const QColor &core,
               const QColor &halo)
{
    painter.save();
    painter.setPen(Qt::NoPen);
    QRadialGradient glow(centre, size * 1.25);
    glow.setColorAt(0.0, halo);
    QColor fade = halo;
    fade.setAlpha(0);
    glow.setColorAt(1.0, fade);
    painter.setBrush(glow);
    painter.drawEllipse(centre, size * 1.25, size * 1.25);

    const qreal waist = size * 0.13;
    QPainterPath star;
    star.moveTo(centre.x(), centre.y() - size);
    star.quadTo(centre.x() + waist, centre.y() - waist, centre.x() + size, centre.y());
    star.quadTo(centre.x() + waist, centre.y() + waist, centre.x(), centre.y() + size);
    star.quadTo(centre.x() - waist, centre.y() + waist, centre.x() - size, centre.y());
    star.quadTo(centre.x() - waist, centre.y() - waist, centre.x(), centre.y() - size);
    painter.setBrush(core);
    painter.drawPath(star);
    painter.restore();
}

// A star with long diffraction spikes, as a telescope photographs it.
void drawFlare(QPainter &painter, QPointF centre, qreal size, const QColor &tint)
{
    painter.save();
    painter.setPen(Qt::NoPen);
    QRadialGradient glow(centre, size * 2.4);
    QColor inner = tint;
    inner.setAlpha(120);
    QColor mid = tint;
    mid.setAlpha(34);
    QColor none = tint;
    none.setAlpha(0);
    glow.setColorAt(0.0, inner);
    glow.setColorAt(0.28, mid);
    glow.setColorAt(1.0, none);
    painter.setBrush(glow);
    painter.drawEllipse(centre, size * 2.4, size * 2.4);

    const qreal reach = size * 3.6;
    const qreal thickness = std::max(0.55, size * 0.11);
    for (int axis = 0; axis < 2; ++axis) {
        const QPointF along = axis == 0 ? QPointF(reach, 0.0) : QPointF(0.0, reach);
        QLinearGradient spike(centre - along, centre + along);
        spike.setColorAt(0.0, QColor(255, 255, 255, 0));
        spike.setColorAt(0.5, QColor(255, 255, 255, 235));
        spike.setColorAt(1.0, QColor(255, 255, 255, 0));
        painter.setBrush(spike);
        const QRectF bar = axis == 0
            ? QRectF(centre.x() - reach, centre.y() - thickness / 2, reach * 2, thickness)
            : QRectF(centre.x() - thickness / 2, centre.y() - reach, thickness, reach * 2);
        painter.drawRect(bar);
    }

    QRadialGradient core(centre, size * 0.75);
    core.setColorAt(0.0, QColor(255, 255, 255, 255));
    core.setColorAt(0.45, QColor(255, 255, 255, 200));
    core.setColorAt(1.0, QColor(255, 255, 255, 0));
    painter.setBrush(core);
    painter.drawEllipse(centre, size * 0.75, size * 0.75);
    painter.restore();
}

// -- Aqua Aero -------------------------------------------------------------------
//
// Frutiger-Aero glass: a saturated sky-to-lagoon body, a pool-floor caustic net
// glowing up from below, rising air bubbles and the split gloss of a Vista orb.

QImage generateAeroTile()
{
    return generateTile(QImage::Format_ARGB32_Premultiplied, [](float u, float v, int x, int y) {
        const float wx = fbm(u * 3.0f, v * 3.0f, 3, 3, 3, 101U);
        const float wy = fbm(u * 3.0f + 4.7f, v * 3.0f + 1.9f, 3, 3, 3, 102U);
        const CellSample coarse =
            cells(u * 5.0f + 0.55f * wx, v * 5.0f + 0.55f * wy, 5, 5, 103U, 0.9f);
        const CellSample fine =
            cells(u * 9.0f - 0.45f * wy, v * 9.0f + 0.45f * wx, 9, 9, 104U, 0.9f);
        const float lineA = std::exp(-coarse.border / 0.028f);
        const float lineB = std::exp(-fine.border / 0.045f) * 0.55f;
        float light = std::max(lineA, lineB);
        light *= 0.45f + 0.55f * clamp01(fbm(u * 2.0f, v * 2.0f, 2, 2, 3, 105U) * 1.6f + 0.5f);
        const Rgb colour = mix(rgb(0xc4f6ff), rgb(0xffffff), clamp01(light));
        return packPremultiplied(colour, light * 0.62f, x, y);
    });
}

void drawAirBubble(QPainter &painter, QPointF centre, qreal radius)
{
    painter.save();
    painter.setPen(Qt::NoPen);
    QRadialGradient body(centre, radius);
    body.setColorAt(0.0, QColor(255, 255, 255, 10));
    body.setColorAt(0.72, QColor(255, 255, 255, 40));
    body.setColorAt(1.0, QColor(255, 255, 255, 150));
    painter.setBrush(body);
    painter.drawEllipse(centre, radius, radius);
    QPen rim(QColor(255, 255, 255, 190), std::max(0.5, radius * 0.16));
    painter.setPen(rim);
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(centre, radius, radius);
    // A darker crescent underneath gives the bubble volume against the glass.
    QPen shade(QColor(18, 104, 150, 70), std::max(0.5, radius * 0.14));
    shade.setCapStyle(Qt::RoundCap);
    painter.setPen(shade);
    const QRectF arc(centre.x() - radius * 0.82, centre.y() - radius * 0.82, radius * 1.64,
                     radius * 1.64);
    painter.drawArc(arc, -30 * 16, -120 * 16);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(255, 255, 255, 240));
    painter.drawEllipse(QPointF(centre.x() - radius * 0.38, centre.y() - radius * 0.4),
                        radius * 0.26, radius * 0.2);
    painter.restore();
}

void paintAero(QPainter &painter, const BubbleShape &shape, const QImage &tile, Dice &dice)
{
    const QRectF r = shape.bounds;
    const QRectF body = shape.body;

    QLinearGradient base(0.0, r.top(), 0.0, r.bottom());
    base.setColorAt(0.0, argb(0xffc2efff));
    base.setColorAt(0.45, argb(0xff6fcff4));
    base.setColorAt(1.0, argb(0xff2ba9e5));
    painter.fillRect(r, base);

    drawTiled(painter, r, tile, tileOffset(dice));

    // Light bouncing up off the floor of the "pool".
    painter.save();
    painter.translate(body.center().x(), r.bottom() + 4.0);
    painter.scale(1.0, std::clamp(r.height() / body.width(), 0.3, 0.9));
    QRadialGradient bounce(QPointF(0.0, 0.0), body.width() * 0.62);
    bounce.setColorAt(0.0, argb(0xc8eaffff));
    bounce.setColorAt(0.5, argb(0x50a8f0ff));
    bounce.setColorAt(1.0, argb(0x00a8f0ff));
    painter.fillRect(QRectF(-body.width(), -body.width() * 2.0, body.width() * 2.0,
                            body.width() * 2.0),
                     bounce);
    painter.restore();

    // Air bubbles rising along the trailing side, a few strays elsewhere.
    const int count = std::clamp(static_cast<int>(body.width() * body.height() / 1500.0), 4, 13);
    for (int i = 0; i < count; ++i) {
        const bool stray = dice.next() < 0.3f;
        const qreal along = stray ? dice.next() : 1.0 - std::pow(dice.next(), 1.8) * 0.45;
        const qreal side = shape.outgoing ? along : 1.0 - along;
        const qreal radius = i == 0 ? dice.range(4.2, 5.6) : dice.range(1.4, 3.6);
        const QPointF centre(body.left() + 6.0 + side * (body.width() - 12.0),
                             body.top() + 5.0 + dice.next() * (body.height() - 10.0));
        drawAirBubble(painter, centre, radius);
    }

    const qreal depth = std::clamp(body.height() * 0.47, 12.0, 42.0);
    QLinearGradient gloss(0.0, body.top(), 0.0, body.top() + depth);
    gloss.setColorAt(0.0, argb(0xf4ffffff));
    gloss.setColorAt(0.55, argb(0xa0ffffff));
    gloss.setColorAt(1.0, argb(0x68ffffff));
    painter.fillPath(glossPath(body, shape.radius, depth, 1.4), gloss);

    QLinearGradient rimLight(0.0, r.top(), 0.0, r.bottom());
    rimLight.setColorAt(0.0, argb(0xe0ffffff));
    rimLight.setColorAt(0.5, argb(0x60ffffff));
    rimLight.setColorAt(1.0, argb(0xb0e6fbff));
    strokeInside(painter, shape.path, rimLight, 1.3);
    innerGlow(painter, shape.path, argb(0x5510689c), 5.0);
}

void finishAero(QPainter &painter, const BubbleShape &shape, Dice &)
{
    outline(painter, shape.path, argb(0xff1f82bd));
}

// -- Calacatta Gold ---------------------------------------------------------------
//
// Polished white marble: a warm-white crystalline ground, flowing grey veins with
// soft halos, a web of hairlines, and a thin inlaid thread of gold, framed in a
// gilded rim.

// A scalar field sampled at a tile pixel and one pixel to its right and below,
// which is enough to measure how far the pixel is from one of its contours.
struct FieldSample
{
    float centre = 0.0f;
    float right = 0.0f;
    float below = 0.0f;
};

template <typename Field>
FieldSample sampleField(const Field &field, float u, float v)
{
    constexpr float step = 1.0f / kTilePixels;
    return {field(u, v), field(u + step, v), field(u, v + step)};
}

// Distance in tile pixels from the sample to the field's `level` contour. The
// local gradient normalises it, so a line drawn from it keeps its width where
// the field is flat as well as where it is steep.
float contourDistance(const FieldSample &sample, float level)
{
    const float gx = sample.right - sample.centre;
    const float gy = sample.below - sample.centre;
    return std::abs(sample.centre - level) / (std::sqrt(gx * gx + gy * gy) + 1e-5f);
}

QImage generateMarbleTile()
{
    return generateTile(QImage::Format_RGB32, [](float u, float v, int x, int y) {
        constexpr float tau = 6.28318530718f;
        constexpr float step = 1.0f / kTilePixels;
        // The flow that bends every vein: warped low-frequency noise.
        const auto flow = [](float fu, float fv) {
            const float wx = fbm(fu * 2.0f, fv * 2.0f, 2, 2, 4, 11U);
            const float wy = fbm(fu * 2.0f + 5.3f, fv * 2.0f + 1.7f, 2, 2, 4, 12U);
            return fbm(fu * 2.0f + 0.9f * wx, fv * 2.0f + 0.9f * wy, 2, 2, 5, 13U);
        };
        const FieldSample bend = sampleField(flow, u, v);
        // Diagonal veins: zero crossings of a sine whose phase the flow bends.
        const auto veinAt = [&](float fu, float fv, float bent, float phase) {
            return std::sin(tau * (fu + fv) + 4.0f * bent + phase);
        };
        const FieldSample vein = {veinAt(u, v, bend.centre, 0.0f),
                                  veinAt(u + step, v, bend.right, 0.0f),
                                  veinAt(u, v + step, bend.below, 0.0f)};
        const float veinDistance = contourDistance(vein, 0.0f);

        // Ground: warm white with faint clouding and a grey haze near the veins.
        const float cloud = fbm(u * 3.0f + bend.centre, v * 3.0f - bend.centre, 3, 3, 5, 14U);
        Rgb colour = mix(rgb(0xfcfbf8), rgb(0xebe6de), smoothstep(-0.2f, 0.45f, cloud));
        colour = mix(colour, rgb(0xdcd6cd), std::exp(-veinDistance / 34.0f) * 0.45f);
        // Crystalline grain: a faint speckle, and the odd calcite glint.
        colour = colour * (1.0f + 0.022f * fbm(u * 128.0f, v * 128.0f, 128, 128, 2, 23U));
        if (unit(hash2(x, y, 24U)) > 0.9975f)
            colour = mix(colour, rgb(0xffffff), 0.6f);

        // Main vein: a feathered band of varying breadth around a sharp core.
        const float breadth = clamp01(fbm(u * 4.0f, v * 4.0f, 4, 4, 3, 15U) * 2.2f + 0.5f);
        const float bandWidth = 3.0f + 16.0f * breadth;
        const float band = std::exp(-(veinDistance * veinDistance) / (bandWidth * bandWidth));
        colour = mix(colour, rgb(0xc9c1b6), band * 0.55f);
        const float coreWidth = 0.5f + 2.3f * breadth;
        const float core = 1.0f - smoothstep(coreWidth - 0.7f, coreWidth + 0.7f, veinDistance);
        // The core fades and returns along its length rather than being inked.
        const float strength = 0.3f + 0.6f * clamp01(fbm(u * 8.0f, v * 8.0f, 8, 8, 3, 16U) * 2.0f + 0.5f);
        colour = mix(colour, rgb(0x857d73), core * strength);

        // Hairlines: a finer network that comes and goes in patches.
        const auto hairAt = [](float fu, float fv, float bent) {
            return fbm(fu * 4.0f, fv * 4.0f, 4, 4, 5, 17U) + 1.5f * bent;
        };
        const FieldSample hair = {hairAt(u, v, bend.centre), hairAt(u + step, v, bend.right),
                                  hairAt(u, v + step, bend.below)};
        const float hairMask = smoothstep(0.0f, 0.2f, fbm(u * 3.0f, v * 3.0f, 3, 3, 2, 18U));
        const float hairLine = 1.0f - smoothstep(0.35f, 1.4f, contourDistance(hair, 0.0f));
        colour = mix(colour, rgb(0x9f978c), hairLine * hairMask * 0.45f);

        // Gold: a thread of its own running beside the grey veins, tapering in and out.
        const auto goldWander = [](float fu, float fv) {
            return fbm(fu * 3.0f, fv * 3.0f, 3, 3, 3, 19U);
        };
        const FieldSample wander = sampleField(goldWander, u, v);
        const FieldSample goldField = {
            veinAt(u, v, bend.centre + 0.16f * wander.centre, 1.1f),
            veinAt(u + step, v, bend.right + 0.16f * wander.right, 1.1f),
            veinAt(u, v + step, bend.below + 0.16f * wander.below, 1.1f)};
        const float goldDistance = contourDistance(goldField, 0.0f);
        const float taper = smoothstep(-0.08f, 0.12f, fbm(u * 2.0f, v * 2.0f, 2, 2, 3, 20U));
        const float goldWidth = (0.9f + 1.5f * clamp01(fbm(u * 6.0f, v * 6.0f, 6, 6, 2, 21U) * 2.0f + 0.5f)) * taper;
        colour = mix(colour, rgb(0xe4d3ad), std::exp(-goldDistance / 7.0f) * 0.35f * taper);
        if (goldWidth > 0.25f) {
            const float gold = 1.0f - smoothstep(goldWidth - 0.6f, goldWidth + 0.6f, goldDistance);
            const float glint = clamp01(fbm(u * 20.0f, v * 20.0f, 20, 20, 2, 22U) * 2.2f + 0.5f);
            Rgb metal = mix(rgb(0x9a661a), rgb(0xf6d98a), glint);
            // One flank catches the light, like a bevelled inlay.
            if (goldField.centre > 0.0f)
                metal = mix(metal, rgb(0xfff3c8), 0.35f);
            colour = mix(colour, metal, gold);
        }
        return pack(colour, x, y);
    });
}

QLinearGradient goldGradient(const QRectF &r)
{
    QLinearGradient gilt(r.topLeft(), r.bottomRight());
    gilt.setColorAt(0.0, argb(0xfff6e2a4));
    gilt.setColorAt(0.22, argb(0xffb78a33));
    gilt.setColorAt(0.45, argb(0xfffff0c4));
    gilt.setColorAt(0.62, argb(0xffa7782a));
    gilt.setColorAt(0.85, argb(0xffe9c979));
    gilt.setColorAt(1.0, argb(0xff8c6221));
    return gilt;
}

void paintMarble(QPainter &painter, const BubbleShape &shape, const QImage &tile, Dice &dice)
{
    const QRectF r = shape.bounds;
    drawTiled(painter, r, tile, tileOffset(dice));

    // Polish: a broad reflection across the top and one soft diagonal band.
    QLinearGradient top(0.0, r.top(), 0.0, r.bottom());
    top.setColorAt(0.0, argb(0x80ffffff));
    top.setColorAt(0.4, argb(0x10ffffff));
    top.setColorAt(1.0, argb(0x00ffffff));
    painter.fillRect(r, top);
    QLinearGradient band(r.topLeft(), QPointF(r.left() + r.height() * 1.2, r.bottom()));
    band.setColorAt(0.0, argb(0x00ffffff));
    band.setColorAt(0.45, argb(0x00ffffff));
    band.setColorAt(0.62, argb(0x4cffffff));
    band.setColorAt(0.8, argb(0x00ffffff));
    band.setColorAt(1.0, argb(0x00ffffff));
    painter.fillRect(r, band);

    innerGlow(painter, shape.path, argb(0x2a6b5220), 6.0);
    // A bright bevel just inside the gilt, then the gilt itself.
    strokeInside(painter, shape.path, argb(0xb0fffbea), 2.5);
    strokeInside(painter, shape.path, goldGradient(r), 1.7);
}

void finishMarble(QPainter &painter, const BubbleShape &shape, Dice &)
{
    outline(painter, shape.path, argb(0xff7a5419));
}

// -- Nebula -------------------------------------------------------------------------
//
// A window onto deep space: doubly domain-warped gas glowing magenta and teal in
// a violet haze, soft dust lanes, a starfield of widely varying brightness and a
// few bright stars with diffraction spikes, behind a faint glass sheen.

QImage generateNebulaTile()
{
    return generateTile(QImage::Format_RGB32, [](float u, float v, int x, int y) {
        const float px = u * 2.0f;
        const float py = v * 2.0f;
        const float qx = fbm(px, py, 2, 2, 4, 21U);
        const float qy = fbm(px + 5.2f, py + 1.3f, 2, 2, 4, 22U);
        const float rx = fbm(px + 1.5f * qx + 1.7f, py + 1.5f * qy + 9.2f, 2, 2, 4, 23U);
        const float ry = fbm(px + 1.5f * qx + 8.3f, py + 1.5f * qy + 2.8f, 2, 2, 4, 24U);
        // Wispy detail: many octaves with a slow falloff.
        const float f = fbm(px + 1.2f * rx, py + 1.2f * ry, 2, 2, 7, 25U, 0.54f);
        // Large bright regions and empty dark space between them.
        const float cloud = fbm(px + 0.6f * qy, py + 0.6f * qx, 2, 2, 3, 20U);
        const float envelope = smoothstep(-0.4f, 0.22f, cloud);

        // Two emission layers, as in narrowband photographs: hydrogen glowing
        // magenta-pink where the warped field is dense, oxygen glowing teal in
        // the second field, over a faint violet haze.
        const float hydrogen = smoothstep(-0.06f, 0.4f, f) * envelope;
        const float oxygen = smoothstep(-0.02f, 0.36f, rx * 0.7f + qy * 0.6f) * (0.35f + 0.65f * envelope);
        const float haze = smoothstep(-0.35f, 0.3f, f);
        // Dust lanes cut dark filaments through the brighter gas.
        const float dust = ridged(px * 2.0f + 1.5f * qy, py * 2.0f + 1.5f * qx, 4, 4, 3, 26U);
        const float lane = smoothstep(0.62f, 0.95f, dust) * 0.5f;

        Rgb colour = mix(rgb(0x04020c), rgb(0x0b0922), clamp01(0.5f + ry));
        colour = colour + rgb(0x34156a) * (haze * 0.5f);
        // A broad luminous glow the fine structure sits in.
        colour = colour + rgb(0x7a2a9a) * (smoothstep(-0.15f, 0.35f, cloud) * 0.22f);
        colour = colour + rgb(0xd62f8c) * (std::pow(hydrogen, 1.5f) * 0.72f);
        colour = colour + rgb(0x16b0c8) * (std::pow(oxygen, 1.4f) * 0.5f * (1.0f - 0.6f * hydrogen));
        colour = colour + rgb(0xffc2e6) * (smoothstep(0.24f, 0.52f, f) * envelope * 0.32f);
        colour = colour * (1.0f - lane);

        // Stars: a jittered grid, each lit with a tight Gaussian and a faint halo.
        constexpr int starCells = 64;
        const float sx = u * starCells;
        const float sy = v * starCells;
        const int cx = static_cast<int>(std::floor(sx));
        const int cy = static_cast<int>(std::floor(sy));
        Rgb starColour = {0.0f, 0.0f, 0.0f};
        for (int j = -1; j <= 1; ++j) {
            for (int i = -1; i <= 1; ++i) {
                const std::uint32_t h = hash2(wrapIndex(cx + i, starCells),
                                              wrapIndex(cy + j, starCells), 27U);
                if (unit(h) > 0.46f)
                    continue;
                const float ox = static_cast<float>(cx + i) + unit(hash32(h ^ 1U));
                const float oy = static_cast<float>(cy + j) + unit(hash32(h ^ 2U));
                // Distance in device pixels of the tile.
                const float dx = (sx - ox) * (static_cast<float>(kTilePixels) / starCells);
                const float dy = (sy - oy) * (static_cast<float>(kTilePixels) / starCells);
                const float d2 = dx * dx + dy * dy;
                const float magnitude = std::pow(unit(hash32(h ^ 3U)), 5.0f);
                const float sigma = 0.55f + 1.1f * magnitude;
                const float light = (0.12f + 1.0f * magnitude)
                                    * (std::exp(-d2 / (2.0f * sigma * sigma))
                                       + 0.12f * magnitude * std::exp(-d2 / (2.0f * 36.0f)));
                const Rgb tint = mix(rgb(0xbcd4ff), rgb(0xffe2c0), unit(hash32(h ^ 4U)));
                starColour = starColour + tint * light;
            }
        }
        // Faint dust of single-pixel stars.
        const float speck = unit(hash2(x, y, 28U));
        if (speck > 0.998f)
            starColour = starColour + rgb(0xdfe6ff) * (0.25f + 0.5f * unit(hash2(x, y, 29U)));

        colour = screen(colour, starColour);
        return pack(colour, x, y);
    });
}

void paintNebula(QPainter &painter, const BubbleShape &shape, const QImage &tile, Dice &dice)
{
    const QRectF r = shape.bounds;
    const QRectF body = shape.body;
    drawTiled(painter, r, tile, tileOffset(dice));

    // Depth: the edges fall away into black before the rim light catches them.
    innerGlow(painter, shape.path, argb(0x90020008), 9.0);

    // Two or three bright stars, kept to the corners so the text stays clear.
    const int flares = body.height() > 70.0 ? 3 : 2;
    for (int i = 0; i < flares; ++i) {
        const bool right = (i % 2 == 0) == shape.outgoing;
        const qreal x = right ? body.right() - dice.range(10.0, 34.0)
                              : body.left() + dice.range(10.0, 34.0);
        const qreal y = i == 2 ? body.center().y() + dice.range(-8.0, 8.0)
                               : (i == 0 ? body.top() + dice.range(6.0, 12.0)
                                         : body.bottom() - dice.range(6.0, 12.0));
        const QColor tint = i == 0 ? argb(0xffb49cff) : (i == 1 ? argb(0xff8fd8ff) : argb(0xffff9fd6));
        drawFlare(painter, QPointF(x, y), dice.range(2.2, 3.4), tint);
    }

    QLinearGradient sheen(0.0, body.top(), 0.0, body.top() + std::min(30.0, body.height() * 0.5));
    sheen.setColorAt(0.0, argb(0x30ffffff));
    sheen.setColorAt(1.0, argb(0x04ffffff));
    painter.fillPath(glossPath(body, shape.radius, std::min(30.0, body.height() * 0.45), 1.2), sheen);

    innerGlow(painter, shape.path, argb(0x707a5cff), 4.0);
    QLinearGradient rim(r.topLeft(), r.bottomRight());
    rim.setColorAt(0.0, argb(0xd0c5b5ff));
    rim.setColorAt(0.5, argb(0x907f63ff));
    rim.setColorAt(1.0, argb(0xd06fd8ff));
    strokeInside(painter, shape.path, rim, 0.9);
}

void finishNebula(QPainter &painter, const BubbleShape &shape, Dice &)
{
    outline(painter, shape.path, argb(0xff4b35a8));
}

// -- Magma ---------------------------------------------------------------------------
//
// Cooling basalt plates floating on molten rock: every seam glows through a
// ramp from deep red to white-yellow, the crust is grained and lit like stone,
// and the heat blooms up from the bottom with a few embers riding it.

Rgb heatRamp(float t)
{
    t = clamp01(t);
    if (t < 0.35f)
        return mix(rgb(0x3a0802), rgb(0xa51b03), t / 0.35f);
    if (t < 0.7f)
        return mix(rgb(0xa51b03), rgb(0xff5e0e), (t - 0.35f) / 0.35f);
    return mix(rgb(0xff5e0e), rgb(0xffd46a), (t - 0.7f) / 0.3f);
}

QImage generateMagmaTile()
{
    return generateTile(QImage::Format_RGB32, [](float u, float v, int x, int y) {
        const float wx = fbm(u * 4.0f, v * 4.0f, 4, 4, 3, 31U);
        const float wy = fbm(u * 4.0f + 3.1f, v * 4.0f + 7.7f, 4, 4, 3, 32U);
        constexpr int plates = 7;
        constexpr float plateSize = static_cast<float>(kTilePixels) / plates;
        const CellSample plate =
            cells(u * plates + 0.4f * wx, v * plates + 0.4f * wy, plates, plates, 33U);
        const float edge = plate.border * plateSize; // tile pixels to the nearest seam
        const float plateShade = unit(plate.id);

        // Basalt: near-black, grained, each plate a shallow dome lit from the top left.
        const float grain = fbm(u * 64.0f, v * 64.0f, 64, 64, 2, 34U);
        const float rough = fbm(u * 14.0f, v * 14.0f, 14, 14, 4, 35U);
        Rgb crust = mix(rgb(0x0b0808), rgb(0x2e2623), clamp01(0.42f + rough * 1.2f + grain * 0.5f));
        crust = crust * (0.75f + 0.45f * plateShade);
        const float lit = -(plate.offsetX + plate.offsetY) * 0.7071f;
        crust = crust * (1.0f + 0.6f * lit);
        if (grain > 0.4f)
            crust = crust + rgb(0x2c2826) * smoothstep(0.4f, 0.6f, grain);
        // A bevel along each plate's rim: lit where the rim faces the top left,
        // in shadow where it faces away.
        const float offsetLength = std::sqrt(plate.offsetX * plate.offsetX + plate.offsetY * plate.offsetY) + 1e-4f;
        const float facing = -(plate.offsetX + plate.offsetY) * 0.7071f / offsetLength;
        const float rim = std::exp(-edge / 5.0f);
        crust = crust * (1.0f - 0.35f * rim * std::max(0.0f, -facing))
                + rgb(0x4a3c36) * (rim * std::max(0.0f, facing) * 0.6f);

        // Some seams run hot, some have almost cooled.
        const float heat = smoothstep(-0.28f, 0.26f, fbm(u * 3.0f, v * 3.0f, 3, 3, 3, 36U));
        const float breadth = clamp01(fbm(u * 9.0f, v * 9.0f, 9, 9, 2, 37U) * 1.8f + 0.5f);
        const float halfWidth = (1.0f + 3.2f * breadth) * (0.55f + 0.45f * heat);
        const float crack = 1.0f - smoothstep(halfWidth * 0.3f, halfWidth + 0.8f, edge);

        // Plate rims redden where they meet the melt.
        crust = crust + rgb(0x5a1203) * (std::exp(-edge / 16.0f) * (0.25f + 0.6f * heat));

        // A finer web of hairline fissures inside the plates, dimly lit.
        constexpr int fissures = 17;
        constexpr float fissureSize = static_cast<float>(kTilePixels) / fissures;
        const CellSample fissure = cells(u * fissures + 0.3f * wy, v * fissures + 0.3f * wx,
                                         fissures, fissures, 38U);
        const float fine = (1.0f - smoothstep(0.4f, 1.6f, fissure.border * fissureSize))
                           * clamp01(fbm(u * 5.0f, v * 5.0f, 5, 5, 2, 39U) * 2.5f + 0.2f);

        const float temperature = std::max(crack * (0.3f + 0.7f * heat), fine * 0.3f);
        Rgb colour = mix(crust, heatRamp(temperature), smoothstep(0.0f, 0.12f, temperature));
        // Bloom around the seams, wider where they are hotter.
        colour = colour
                 + rgb(0xff3a00) * (std::exp(-edge / (3.0f + 6.0f * heat)) * 0.42f * heat);
        return pack(colour, x, y);
    });
}

void paintMagma(QPainter &painter, const BubbleShape &shape, const QImage &tile, Dice &dice)
{
    const QRectF r = shape.bounds;
    const QRectF body = shape.body;
    drawTiled(painter, r, tile, tileOffset(dice));

    QLinearGradient heat(0.0, r.bottom(), 0.0, r.top());
    heat.setColorAt(0.0, argb(0x40ff5a00));
    heat.setColorAt(0.4, argb(0x0cff3c00));
    heat.setColorAt(1.0, argb(0x00ff3c00));
    painter.fillRect(r, heat);

    // Embers lifting off the surface.
    painter.save();
    painter.setPen(Qt::NoPen);
    const int embers = std::clamp(static_cast<int>(body.width() / 70.0), 2, 5);
    for (int i = 0; i < embers; ++i) {
        const QPointF centre(body.left() + dice.range(6.0, body.width() - 6.0),
                             body.top() + dice.range(4.0, body.height() - 4.0));
        const qreal radius = dice.range(0.6, 1.3);
        QRadialGradient ember(centre, radius * 3.0);
        ember.setColorAt(0.0, argb(0xfffff4c0));
        ember.setColorAt(0.22, argb(0xf0ffb040));
        ember.setColorAt(0.45, argb(0x50ff6a10));
        ember.setColorAt(1.0, argb(0x00ff4a00));
        painter.setBrush(ember);
        painter.drawEllipse(centre, radius * 3.0, radius * 3.0);
    }
    painter.restore();

    // Obsidian glaze on top.
    QLinearGradient glaze(0.0, body.top(), 0.0, body.top() + std::min(28.0, body.height() * 0.45));
    glaze.setColorAt(0.0, argb(0x38ffffff));
    glaze.setColorAt(1.0, argb(0x02ffffff));
    painter.fillPath(glossPath(body, shape.radius, std::min(28.0, body.height() * 0.42), 1.2), glaze);

    innerGlow(painter, shape.path, argb(0x60ff5a14), 5.0);
    QLinearGradient rim(0.0, r.top(), 0.0, r.bottom());
    rim.setColorAt(0.0, argb(0x80ffb070));
    rim.setColorAt(1.0, argb(0xe0ff7a2a));
    strokeInside(painter, shape.path, rim, 0.8);
}

void finishMagma(QPainter &painter, const BubbleShape &shape, Dice &)
{
    outline(painter, shape.path, argb(0xff3a1206));
}

// -- Prism Holo ------------------------------------------------------------------------
//
// Holographic foil: a silver base with brushed grain, a thin-film rainbow that
// drifts across it, pyramid facets that each catch a different hue, dense
// glitter, and bright glints under a diagonal specular sweep.

QImage generateHoloTile()
{
    return generateTile(QImage::Format_RGB32, [](float u, float v, int x, int y) {
        const float drift = fbm(u * 3.0f, v * 3.0f, 3, 3, 4, 41U);

        // "Cracked ice": angular shards (undistorted Voronoi cells), each split
        // again into smaller fragments. Every fragment is a mirror tilted its own
        // way, so it carries its own slice of the rainbow and its own gradient.
        constexpr int shards = 8;
        constexpr int fragments = 15;
        const CellSample shard = cells(u * shards, v * shards, shards, shards, 45U, 0.95f);
        const CellSample fragment = cells(u * fragments, v * fragments, fragments, fragments, 46U, 0.95f);
        const float tiltAngle = unit(hash32(shard.id ^ fragment.id)) * 6.2831853f;
        const float tilt = (fragment.offsetX * std::cos(tiltAngle) + fragment.offsetY * std::sin(tiltAngle));
        const float shardShift = (unit(shard.id) - 0.5f) * 0.42f + (unit(fragment.id) - 0.5f) * 0.16f;
        const float shardLight = 0.94f + 0.12f * unit(hash32(fragment.id ^ 0x51U));

        const float phase = (u + v) * 1.0f + drift * 0.55f + shardShift + tilt * 0.22f;
        const Rgb film = cosinePalette(phase, {0.79f, 0.78f, 0.82f}, {0.2f, 0.21f, 0.18f},
                                       {1.0f, 1.0f, 1.0f}, {0.0f, 0.33f, 0.67f});
        const float brushed = fbm(u * 2.0f, v * 160.0f, 2, 160, 2, 43U);
        const Rgb silver = rgb(0xdde1ea) * (0.97f + 0.06f * brushed);
        // Some fragments face away and show mostly bare silver.
        const float catchLight = 0.5f + 0.45f * unit(hash32(fragment.id ^ shard.id ^ 0x93U));
        Rgb colour = mix(silver, film, catchLight) * shardLight;

        // Fracture lines catch the light: bright on the big cracks, faint on the small.
        const float bigCrack = 1.0f - smoothstep(0.5f, 1.6f, shard.border * (static_cast<float>(kTilePixels) / shards));
        const float smallCrack = 1.0f - smoothstep(0.3f, 1.2f, fragment.border * (static_cast<float>(kTilePixels) / fragments));
        colour = screen(colour, rgb(0xffffff) * std::max(bigCrack * 0.75f, smallCrack * 0.28f));

        // Glitter: flakes on a 2 px grid; most dim, a few blazing white.
        const std::uint32_t flake = hash2(wrapIndex(x / 2, kTilePixels / 2),
                                          wrapIndex(y / 2, kTilePixels / 2), 44U);
        const float chance = unit(flake);
        if (chance > 0.965f) {
            const float power = std::pow(unit(hash32(flake)), 3.0f);
            const Rgb tint = cosinePalette(unit(hash32(flake ^ 7U)), {0.8f, 0.8f, 0.8f},
                                           {0.2f, 0.2f, 0.2f}, {1.0f, 1.0f, 1.0f},
                                           {0.0f, 0.33f, 0.67f});
            colour = screen(colour, (chance > 0.992f ? rgb(0xffffff) : tint) * (0.25f + 0.75f * power));
        }
        return pack(colour, x, y);
    });
}

void paintHolo(QPainter &painter, const BubbleShape &shape, const QImage &tile, Dice &dice)
{
    const QRectF r = shape.bounds;
    const QRectF body = shape.body;
    drawTiled(painter, r, tile, tileOffset(dice));

    // Specular sweep: a crisp bright band with a softer companion.
    const qreal slant = r.height() * 0.9;
    const qreal bandX = body.left() + body.width() * dice.range(0.25, 0.6);
    QLinearGradient band(QPointF(bandX - 30.0, r.top()), QPointF(bandX + 30.0 + slant * 0.5, r.bottom()));
    band.setColorAt(0.0, argb(0x00ffffff));
    band.setColorAt(0.28, argb(0x18ffffff));
    band.setColorAt(0.4, argb(0xa0ffffff));
    band.setColorAt(0.46, argb(0x38ffffff));
    band.setColorAt(0.58, argb(0x58ffffff));
    band.setColorAt(0.72, argb(0x00ffffff));
    band.setColorAt(1.0, argb(0x00ffffff));
    painter.fillRect(r, band);

    QLinearGradient top(0.0, r.top(), 0.0, r.bottom());
    top.setColorAt(0.0, argb(0x60ffffff));
    top.setColorAt(0.35, argb(0x00ffffff));
    painter.fillRect(r, top);

    const int glints = std::clamp(static_cast<int>(body.width() * body.height() / 3500.0), 3, 6);
    for (int i = 0; i < glints; ++i) {
        const QPointF centre(body.left() + dice.range(8.0, body.width() - 8.0),
                             body.top() + dice.range(5.0, body.height() - 5.0));
        drawGlint(painter, centre, dice.range(2.5, i == 0 ? 6.5 : 4.5), argb(0xffffffff),
                  argb(0x90ffffff));
    }

    // Iridescent rim.
    QLinearGradient rim(r.topLeft(), r.bottomRight());
    rim.setColorAt(0.0, argb(0xffff9ecf));
    rim.setColorAt(0.25, argb(0xffffe08a));
    rim.setColorAt(0.5, argb(0xff8ff0c2));
    rim.setColorAt(0.75, argb(0xff8ec5ff));
    rim.setColorAt(1.0, argb(0xffc79bff));
    strokeInside(painter, shape.path, rim, 1.5);
    innerGlow(painter, shape.path, argb(0x40ffffff), 4.0);
}

void finishHolo(QPainter &painter, const BubbleShape &shape, Dice &)
{
    outline(painter, shape.path, argb(0xff7d84ad));
}

// -- Catalogue -------------------------------------------------------------------------

struct SkinDefinition
{
    const char *id;
    const char *name;
    const char *description;
    QRgb text;
    QRgb secondaryText;
    // Drawn 1 px under the text: a drop shadow on the dark materials, a white
    // emboss on the light ones, so glyphs hold up over veins, seams and glints.
    QRgb textShadow;
    QImage (*generate)();
    void (*paint)(QPainter &, const BubbleShape &, const QImage &, Dice &);
    void (*finish)(QPainter &, const BubbleShape &, Dice &);
};

const std::array<SkinDefinition, 5> kSkins = {{
    {"bubble.aero", "Aqua Aero", "Frutiger Aero glass with pool caustics and rising air bubbles.",
     0xff0a3350, 0xff1f5a80, 0x8cffffff, generateAeroTile, paintAero, finishAero},
    {"bubble.marble", "Calacatta Gold", "Polished white marble veined in grey and gold, in a gilded rim.",
     0xff2a2622, 0xff76603f, 0xb4ffffff, generateMarbleTile, paintMarble, finishMarble},
    {"bubble.nebula", "Nebula", "Deep-space gas clouds, dust lanes and a starfield with bright flares.",
     0xfff6f2ff, 0xffc4b8ef, 0xc8050210, generateNebulaTile, paintNebula, finishNebula},
    {"bubble.magma", "Magma", "Cooling basalt plates over molten rock that glows through every seam.",
     0xfffff5ea, 0xffffc89a, 0xe0140604, generateMagmaTile, paintMagma, finishMagma},
    {"bubble.holo", "Prism Holo", "Cracked-ice holographic foil: every shard catches its own rainbow, with glitter.",
     0xff1d1838, 0xff4c4474, 0x9cffffff, generateHoloTile, paintHolo, finishHolo},
}};

int indexOf(const QString &id)
{
    if (id.isEmpty())
        return -1;
    for (std::size_t i = 0; i < kSkins.size(); ++i) {
        if (id == QLatin1String(kSkins[i].id))
            return static_cast<int>(i);
    }
    return -1;
}

struct TileCache
{
    std::mutex mutex;
    std::array<QImage, kSkins.size()> full;
    std::array<QImage, kSkins.size()> standard;
};

TileCache &tileCache()
{
    static TileCache cache;
    return cache;
}

QImage tileFor(int index, qreal devicePixelRatio)
{
    TileCache &cache = tileCache();
    const std::lock_guard lock(cache.mutex);
    QImage &full = cache.full[static_cast<std::size_t>(index)];
    if (full.isNull())
        full = kSkins[static_cast<std::size_t>(index)].generate();
    if (devicePixelRatio > 1.01)
        return full;
    QImage &standard = cache.standard[static_cast<std::size_t>(index)];
    if (standard.isNull()) {
        QImage source = full;
        source.setDevicePixelRatio(1.0);
        standard = source.scaled(kTileLogical, kTileLogical, Qt::IgnoreAspectRatio,
                                 Qt::SmoothTransformation);
        standard.setDevicePixelRatio(1.0);
    }
    return standard;
}

} // namespace

QList<BubbleSkinInfo> BubbleSkins::catalog()
{
    QList<BubbleSkinInfo> skins;
    skins.reserve(static_cast<qsizetype>(kSkins.size()));
    for (const SkinDefinition &skin : kSkins) {
        skins.append({QString::fromLatin1(skin.id), QString::fromUtf8(skin.name),
                      QString::fromUtf8(skin.description)});
    }
    return skins;
}

bool BubbleSkins::isSkin(const QString &id)
{
    return indexOf(id) >= 0;
}

QColor BubbleSkins::textColor(const QString &id)
{
    const int index = indexOf(id);
    return index < 0 ? QColor() : QColor::fromRgba(kSkins[static_cast<std::size_t>(index)].text);
}

QColor BubbleSkins::secondaryTextColor(const QString &id)
{
    const int index = indexOf(id);
    return index < 0 ? QColor()
                     : QColor::fromRgba(kSkins[static_cast<std::size_t>(index)].secondaryText);
}

void BubbleSkins::prepare(const QString &id)
{
    const int index = indexOf(id);
    if (index < 0)
        return;
    {
        TileCache &cache = tileCache();
        const std::lock_guard lock(cache.mutex);
        if (!cache.full[static_cast<std::size_t>(index)].isNull())
            return;
    }
    // A paint that arrives mid-generation waits on the cache lock, then reuses it.
    QThreadPool::globalInstance()->start([index] { (void)tileFor(index, kTileScale); });
}

QColor BubbleSkins::textShadowColor(const QString &id)
{
    const int index = indexOf(id);
    return index < 0 ? QColor()
                     : QColor::fromRgba(kSkins[static_cast<std::size_t>(index)].textShadow);
}

QImage BubbleSkins::texture(const QString &id)
{
    const int index = indexOf(id);
    return index < 0 ? QImage() : tileFor(index, kTileScale);
}

void BubbleSkins::paint(QPainter *painter, const QString &id, const BubbleShape &shape,
                        qreal devicePixelRatio)
{
    const int index = indexOf(id);
    if (index < 0 || !painter || shape.bounds.isEmpty() || shape.path.isEmpty())
        return;
    const SkinDefinition &skin = kSkins[static_cast<std::size_t>(index)];
    const qreal scale = std::max<qreal>(1.0, devicePixelRatio);

    const QSize pixels(qCeil(shape.bounds.width() * scale), qCeil(shape.bounds.height() * scale));
    QImage layer(pixels, QImage::Format_ARGB32_Premultiplied);
    layer.setDevicePixelRatio(scale);
    layer.fill(Qt::transparent);

    Dice dice(hash2(qRound(shape.bounds.width()), qRound(shape.bounds.height()),
                    static_cast<std::uint32_t>(index) * 977U + (shape.outgoing ? 1U : 0U)));
    {
        QPainter layerPainter(&layer);
        layerPainter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
        layerPainter.translate(-shape.bounds.topLeft());
        skin.paint(layerPainter, shape, tileFor(index, scale), dice);

        // Cut away everything outside the outline. Filling the complement
        // (bounds XOR path) keeps the edge antialiased, which a clip would not.
        QPainterPath outside;
        outside.setFillRule(Qt::OddEvenFill);
        outside.addRect(shape.bounds.adjusted(-2.0, -2.0, 2.0, 2.0));
        outside.addPath(shape.path);
        layerPainter.setCompositionMode(QPainter::CompositionMode_DestinationOut);
        layerPainter.fillPath(outside, Qt::black);
        layerPainter.setCompositionMode(QPainter::CompositionMode_SourceOver);

        skin.finish(layerPainter, shape, dice);
    }
    painter->drawImage(shape.bounds, layer);
}

} // namespace OpenChat
