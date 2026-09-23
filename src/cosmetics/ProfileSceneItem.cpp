#include "cosmetics/ProfileSceneItem.h"

#include "cosmetics/CosmeticPaint.h"

#include <QCache>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QtMath>

#include <algorithm>
#include <array>
#include <cmath>

namespace OpenChat {

using namespace CosmeticPaint;

namespace {

struct Stage
{
    QRectF r;      // the whole scene
    qreal horizon; // y of the horizon line
    bool dark;
    qreal dpr;
};

qreal smoothstep(qreal edge0, qreal edge1, qreal x)
{
    const qreal t = std::clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

void fillSky(QPainter &p, const QRectF &r, std::initializer_list<std::pair<qreal, QColor>> stops)
{
    QLinearGradient sky(r.topLeft(), r.bottomLeft());
    for (const auto &[at, colour] : stops)
        sky.setColorAt(at, colour);
    p.fillRect(r, sky);
}

void drawStars(QPainter &p, const QRectF &area, quint32 seed, int count, qreal brightness)
{
    p.save();
    p.setPen(Qt::NoPen);
    for (int i = 0; i < count; ++i) {
        const QPointF at(area.left() + area.width() * random01(seed, i),
                         area.top() + area.height() * std::pow(random01(seed + 1, i), 1.3));
        const qreal b = random01(seed + 2, i);
        p.setBrush(QColor(255, 255, 255, int((70 + 185 * b) * brightness)));
        const qreal radius = 0.3 + 0.45 * b * b;
        p.drawEllipse(at, radius, radius);
    }
    p.restore();
}

// A cumulus cloud: overlapping puffs with a flat, shaded base.
void drawCloud(QPainter &p, const QPointF &at, qreal scale, const QColor &top, const QColor &base,
               qreal opacity)
{
    QPainterPath cloud;
    const std::array<std::array<qreal, 3>, 6> puffs = {{{-9, 1, 5.5},
                                                         {-3.5, -3.5, 7},
                                                         {4, -2, 6},
                                                         {10, 1.5, 4.5},
                                                         {0, 2, 6},
                                                         {-12.5, 3.2, 3.2}}};
    for (const auto &puff : puffs)
        cloud.addEllipse(at + QPointF(puff[0], puff[1]) * scale, puff[2] * scale, puff[2] * scale * 0.9);
    QPainterPath floor;
    floor.addRect(QRectF(at.x() - 20 * scale, at.y() + 4.2 * scale, 40 * scale, 20 * scale));
    cloud = cloud.subtracted(floor).simplified();
    p.save();
    p.setOpacity(opacity);
    drawBlurred(p, cloud.boundingRect(), 0.9 * scale, [&](QPainter &lp) {
        QLinearGradient fill(0, at.y() - 10 * scale, 0, at.y() + 4.2 * scale);
        fill.setColorAt(0.0, top);
        fill.setColorAt(0.55, top);
        fill.setColorAt(1.0, base);
        lp.setPen(Qt::NoPen);
        lp.setBrush(fill);
        lp.drawPath(cloud);
    });
    p.restore();
}

// ------------------------------------------------------------ Northern Lights

void paintAurora(QPainter &p, const Stage &s)
{
    const QRectF r = s.r;
    const quint32 seed = 0xA0204u;
    if (s.dark)
        fillSky(p, r, {{0.0, QColor(3, 9, 22)}, {0.55, QColor(8, 28, 48)}, {1.0, QColor(16, 58, 76)}});
    else
        fillSky(p, r, {{0.0, QColor(198, 226, 250)}, {0.6, QColor(228, 242, 252)},
                       {1.0, QColor(252, 238, 246)}});
    if (s.dark)
        drawStars(p, QRectF(r.left(), r.top(), r.width(), s.horizon - r.top()), seed, int(r.width() * 0.5), 1.0);

    // Aurora curtains: vertical rays hanging from a wavering lower hem.
    const qreal w = r.width();
    const qreal h = s.horizon - r.top();
    const QImage curtains = proceduralImage(r, s.dpr, [&](qreal x, qreal y) {
        qreal red = 0, green = 0, blue = 0, alpha = 0;
        for (int k = 0; k < 2; ++k) {
            const qreal phase = x / w * 2.0 * M_PI;
            const qreal hem = r.top() + h * (0.72 - 0.16 * k + 0.1 * std::sin(phase * 0.9 + k * 1.9)
                                             + 0.05 * std::sin(phase * 2.6 + k * 0.7));
            const qreal reach = h * (0.45 + 0.3 * valueNoise(x / 22.0, k * 3.0, seed + k));
            if (y > hem + 1.5 || y < hem - reach)
                continue;
            const qreal t = std::clamp((hem - y) / reach, 0.0, 1.0);
            const qreal rays = 0.45 + 0.55 * fractalNoise(x / 2.2, k * 5.0 + y / 90.0, seed + 7 + k, 2);
            const qreal hemGlow = smoothstep(1.5, -0.5, y - hem);
            const qreal intensity = std::pow(1.0 - t, 1.6) * rays * hemGlow * (k == 0 ? 1.0 : 0.75);
            const QColor green_ = s.dark ? QColor(60, 255, 160) : QColor(20, 196, 132);
            const QColor violet = s.dark ? QColor(170, 80, 255) : QColor(150, 90, 230);
            const QColor tint = mix(green_, violet, smoothstep(0.35, 0.95, t));
            red += tint.redF() * intensity;
            green += tint.greenF() * intensity;
            blue += tint.blueF() * intensity;
            alpha = std::max(alpha, intensity);
        }
        if (alpha <= 0.0)
            return qRgba(0, 0, 0, 0);
        const qreal a = std::min(1.0, alpha * (s.dark ? 0.95 : 1.1));
        const qreal norm = std::max(alpha, 1e-6);
        return qRgba(int(std::min(1.0, red / norm) * 255), int(std::min(1.0, green / norm) * 255),
                     int(std::min(1.0, blue / norm) * 255), int(a * 255));
    });
    QImage soft = curtains;
    blurImage(soft, 1.2 * s.dpr);
    p.drawImage(r.topLeft(), soft);
    p.save();
    p.setCompositionMode(QPainter::CompositionMode_Plus);
    p.setOpacity(s.dark ? 0.35 : 0.0);
    p.drawImage(r.topLeft(), curtains);
    p.restore();

    // A snowy ridge of pines along the horizon.
    const QColor far = s.dark ? QColor(10, 30, 44) : QColor(150, 176, 204);
    const QColor near = s.dark ? QColor(3, 10, 18) : QColor(88, 116, 146);
    QPainterPath ridge;
    ridge.moveTo(r.left(), r.bottom());
    for (qreal x = r.left(); x <= r.right() + 1; x += 2)
        ridge.lineTo(x, s.horizon - 4 - 5 * valueNoise(x / 30.0, 0.5, seed + 30) - 3 * std::sin(x / 41.0));
    ridge.lineTo(r.right(), r.bottom());
    ridge.closeSubpath();
    p.setPen(Qt::NoPen);
    p.setBrush(far);
    p.drawPath(ridge);
    for (int layer = 0; layer < 2; ++layer) {
        const QColor ink = layer == 0 ? mix(far, near, 0.5) : near;
        qreal x = r.left() - 4 + layer * 3;
        int i = 0;
        while (x < r.right() + 6) {
            const qreal tall = (layer == 0 ? 9.0 : 13.0) * (0.6 + 0.6 * random01(seed + 40 + layer, i));
            const qreal ground = s.horizon + (layer == 0 ? -1.0 : 3.0) + 2.0 * std::sin(x / 23.0 + layer);
            const qreal half = tall * 0.3;
            QPainterPath tree;
            for (int tier = 0; tier < 3; ++tier) {
                const qreal top = ground - tall + tier * tall * 0.27;
                const qreal bottom = ground - tall * 0.12 * (2 - tier) + (tier == 2 ? 0.0 : -tall * 0.05);
                const qreal spread = half * (0.55 + 0.25 * tier);
                tree.moveTo(x, top);
                tree.lineTo(x + spread, bottom);
                tree.lineTo(x - spread, bottom);
                tree.closeSubpath();
            }
            tree.addRect(QRectF(x - 0.5, ground - tall * 0.15, 1.0, tall * 0.2));
            p.setBrush(ink);
            p.drawPath(tree);
            if (!s.dark || layer == 1) {
                // Snow caught on the upper boughs.
                p.save();
                p.setClipPath(tree);
                p.setBrush(s.dark ? QColor(160, 200, 220, 70) : QColor(255, 255, 255, 170));
                p.drawEllipse(QPointF(x - half * 0.25, ground - tall * 0.8), half * 0.6, tall * 0.1);
                p.restore();
            }
            x += (layer == 0 ? 5.5 : 7.5) + 5.0 * random01(seed + 50 + layer, i);
            ++i;
        }
    }
    QLinearGradient snow(0, s.horizon + 2, 0, r.bottom());
    snow.setColorAt(0.0, s.dark ? QColor(26, 52, 70) : QColor(236, 244, 252));
    snow.setColorAt(1.0, s.dark ? QColor(14, 34, 50) : QColor(214, 230, 244));
    QPainterPath ground;
    ground.moveTo(r.left(), s.horizon + 5);
    for (qreal x = r.left(); x <= r.right() + 1; x += 3)
        ground.lineTo(x, s.horizon + 4 + 1.5 * std::sin(x / 17.0));
    ground.lineTo(r.right(), r.bottom());
    ground.lineTo(r.left(), r.bottom());
    ground.closeSubpath();
    p.setBrush(snow);
    p.drawPath(ground);
}

// -------------------------------------------------------------- Spring Meadow

void paintMeadow(QPainter &p, const Stage &s)
{
    const QRectF r = s.r;
    const quint32 seed = 0x3EAD0u;
    if (s.dark) {
        fillSky(p, r, {{0.0, QColor(8, 22, 48)}, {0.6, QColor(26, 60, 98)}, {1.0, QColor(56, 96, 128)}});
        drawStars(p, QRectF(r.left(), r.top(), r.width(), s.horizon - r.top()), seed, int(r.width() * 0.3), 0.8);
        // A full moon with a wide halo.
        const QPointF moon(r.left() + r.width() * 0.87, r.top() + (s.horizon - r.top()) * 0.3);
        QRadialGradient halo(moon, 28);
        halo.setColorAt(0.0, QColor(200, 226, 255, 110));
        halo.setColorAt(1.0, QColor(200, 226, 255, 0));
        p.setPen(Qt::NoPen);
        p.setBrush(halo);
        p.drawEllipse(moon, 28, 28);
        QRadialGradient disc(moon + QPointF(-2, -2), 8);
        disc.setColorAt(0.0, QColor(255, 255, 246));
        disc.setColorAt(1.0, QColor(214, 226, 236));
        p.setBrush(disc);
        p.drawEllipse(moon, 7, 7);
        p.setBrush(QColor(170, 186, 204, 120));
        p.drawEllipse(moon + QPointF(2, 1.5), 1.8, 1.4);
        p.drawEllipse(moon + QPointF(-2.5, 2.5), 1.1, 1.0);
    } else {
        fillSky(p, r, {{0.0, QColor(58, 154, 240)}, {0.45, QColor(132, 200, 250)},
                       {0.8, QColor(212, 238, 255)}, {1.0, QColor(238, 250, 255)}});
        // Sun glare in the corner.
        const QPointF sun(r.left() + r.width() * 0.88, r.top() + 6);
        QRadialGradient glare(sun, 46);
        glare.setColorAt(0.0, QColor(255, 255, 240, 255));
        glare.setColorAt(0.12, QColor(255, 252, 220, 230));
        glare.setColorAt(0.4, QColor(255, 250, 220, 80));
        glare.setColorAt(1.0, QColor(255, 255, 255, 0));
        p.setPen(Qt::NoPen);
        p.setBrush(glare);
        p.drawEllipse(sun, 46, 46);
    }

    // Clouds drifting across the sky.
    const QColor cloudTop = s.dark ? QColor(90, 120, 156) : QColor(255, 255, 255);
    const QColor cloudBase = s.dark ? QColor(48, 74, 108) : QColor(206, 228, 246);
    if (s.dark) {
        // By night only thin moonlit wisps.
        drawBlurred(p, r, 2.0, [&](QPainter &lp) {
            lp.setPen(Qt::NoPen);
            lp.setBrush(QColor(150, 180, 220, 60));
            lp.drawEllipse(QPointF(r.left() + r.width() * 0.34, r.top() + 12), 30, 3.2);
            lp.drawEllipse(QPointF(r.left() + r.width() * 0.62, r.top() + 22), 24, 2.4);
            lp.setBrush(QColor(170, 200, 240, 45));
            lp.drawEllipse(QPointF(r.left() + r.width() * 0.78, r.top() + 34), 18, 2.0);
        });
    } else {
        drawCloud(p, QPointF(r.left() + r.width() * 0.3, r.top() + 14), 1.15, cloudTop, cloudBase, 0.95);
        drawCloud(p, QPointF(r.left() + r.width() * 0.66, r.top() + 24), 0.8, cloudTop, cloudBase, 0.9);
        drawCloud(p, QPointF(r.left() + r.width() * 0.08, r.top() + 36), 0.6, cloudTop, cloudBase, 0.8);
    }

    // Rolling hills: a hazy far range, then two glossy green swells.
    const auto hill = [&](qreal base, qreal amplitude, qreal period, qreal offset) {
        QPainterPath path;
        path.moveTo(r.left(), r.bottom());
        for (qreal x = r.left(); x <= r.right() + 2; x += 2)
            path.lineTo(x, base - amplitude * (0.5 + 0.5 * std::sin((x / period) + offset)));
        path.lineTo(r.right(), r.bottom());
        path.closeSubpath();
        return path;
    };
    const QPainterPath far = hill(s.horizon + 1, 9, 38, 1.2);
    const QPainterPath mid = hill(s.horizon + 7, 11, 52, 3.7);
    const QPainterPath front = hill(s.horizon + 13, 10, 44, 0.3);
    struct Layer
    {
        const QPainterPath *path;
        QColor top;
        QColor bottom;
    };
    const std::array<Layer, 3> layers = {
        Layer{&far, s.dark ? QColor(34, 70, 84) : QColor(150, 206, 170),
              s.dark ? QColor(26, 58, 66) : QColor(124, 190, 150)},
        Layer{&mid, s.dark ? QColor(30, 86, 58) : QColor(126, 212, 72),
              s.dark ? QColor(18, 58, 38) : QColor(70, 170, 40)},
        Layer{&front, s.dark ? QColor(26, 98, 52) : QColor(108, 206, 48),
              s.dark ? QColor(12, 50, 26) : QColor(40, 146, 22)}};
    for (const Layer &layer : layers) {
        const QRectF box = layer.path->boundingRect();
        QLinearGradient fill(0, box.top(), 0, box.top() + 22);
        fill.setColorAt(0.0, layer.top);
        fill.setColorAt(1.0, layer.bottom);
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawPath(*layer.path);
        // A glossy crest, the Frutiger Aero signature.
        p.save();
        p.setClipPath(*layer.path);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(s.dark ? QColor(150, 210, 255, 90) : QColor(255, 255, 255, 150), 1.6));
        p.drawPath(layer.path->translated(0, 0.9));
        p.restore();
    }
    // Wildflowers (by day) or fireflies (by night).
    p.setPen(Qt::NoPen);
    for (int i = 0; i < 26; ++i) {
        const qreal x = r.left() + r.width() * random01(seed + 60, i);
        const qreal y = s.horizon + 10 + (r.bottom() - s.horizon - 10) * random01(seed + 61, i);
        if (!front.contains(QPointF(x, y)))
            continue;
        if (s.dark) {
            QRadialGradient fly(QPointF(x, y - 6), 2.6);
            fly.setColorAt(0.0, QColor(240, 255, 170, 240));
            fly.setColorAt(1.0, QColor(200, 255, 80, 0));
            p.setBrush(fly);
            p.drawEllipse(QPointF(x, y - 6), 2.6, 2.6);
        } else {
            const std::array<QColor, 3> petals = {QColor(255, 255, 255), QColor(255, 226, 90),
                                                  QColor(255, 150, 190)};
            p.setBrush(petals[size_t(i % 3)]);
            p.drawEllipse(QPointF(x, y), 0.9, 0.9);
        }
    }
}

// --------------------------------------------------------------- Aqua Bubbles

void drawBubble(QPainter &p, const QPointF &c, qreal radius, bool dark)
{
    QRadialGradient body(c, radius);
    body.setColorAt(0.0, QColor(255, 255, 255, dark ? 8 : 25));
    body.setColorAt(0.72, QColor(255, 255, 255, dark ? 22 : 45));
    body.setColorAt(0.93, dark ? QColor(120, 230, 255, 150) : QColor(255, 255, 255, 190));
    body.setColorAt(1.0, dark ? QColor(120, 230, 255, 40) : QColor(255, 255, 255, 60));
    p.setPen(Qt::NoPen);
    p.setBrush(body);
    p.drawEllipse(c, radius, radius);
    // Iridescent tint along the lower rim.
    p.save();
    QPainterPath disc;
    disc.addEllipse(c, radius, radius);
    p.setClipPath(disc);
    QLinearGradient irid(c + QPointF(-radius, radius * 0.2), c + QPointF(radius, radius));
    irid.setColorAt(0.0, QColor(255, 140, 220, 0));
    irid.setColorAt(0.5, QColor(255, 150, 230, dark ? 60 : 70));
    irid.setColorAt(1.0, QColor(120, 255, 220, dark ? 70 : 60));
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QBrush(irid), std::max(0.8, radius * 0.16)));
    p.drawEllipse(c, radius * 0.92, radius * 0.92);
    p.restore();
    // Highlights.
    p.setBrush(QColor(255, 255, 255, 235));
    p.save();
    p.translate(c + QPointF(-radius * 0.38, -radius * 0.42));
    p.rotate(-40);
    p.drawEllipse(QPointF(0, 0), radius * 0.34, radius * 0.18);
    p.restore();
    p.setBrush(QColor(255, 255, 255, 150));
    p.drawEllipse(c + QPointF(radius * 0.45, radius * 0.45), radius * 0.1, radius * 0.1);
}

void paintAqua(QPainter &p, const Stage &s)
{
    const QRectF r = s.r;
    const quint32 seed = 0xA0AAu;
    if (s.dark)
        fillSky(p, r, {{0.0, QColor(2, 22, 42)}, {0.55, QColor(6, 58, 94)}, {1.0, QColor(10, 96, 138)}});
    else
        fillSky(p, r, {{0.0, QColor(214, 243, 255)}, {0.55, QColor(150, 220, 248)},
                       {1.0, QColor(84, 190, 236)}});

    // Light ribbons sweeping up from the lower left.
    const auto ribbon = [&](qreal startY, qreal endY, qreal thickness, qreal bend) {
        QPainterPath path;
        const QPointF a(r.left() - 10, r.top() + startY);
        const QPointF b(r.right() + 10, r.top() + endY);
        path.moveTo(a);
        path.cubicTo(a + QPointF(r.width() * 0.35, -bend), b + QPointF(-r.width() * 0.35, bend), b);
        path.lineTo(b + QPointF(0, thickness));
        path.cubicTo(b + QPointF(-r.width() * 0.35, bend + thickness * 1.6),
                     a + QPointF(r.width() * 0.35, -bend + thickness * 0.4), a + QPointF(0, thickness));
        path.closeSubpath();
        return path;
    };
    const std::array<std::array<qreal, 5>, 4> ribbons = {{{70, 18, 16, 26, 0.34},
                                                          {82, 34, 9, 18, 0.5},
                                                          {60, 6, 5, 34, 0.6},
                                                          {92, 52, 22, 10, 0.22}}};
    for (const auto &spec : ribbons) {
        const QPainterPath band = ribbon(spec[0] * r.height() / 72.0, spec[1] * r.height() / 72.0,
                                         spec[2], spec[3]);
        QLinearGradient shine(r.left(), 0, r.right(), 0);
        const QColor light = s.dark ? QColor(120, 230, 255) : QColor(255, 255, 255);
        shine.setColorAt(0.0, withAlpha(light, 0.0));
        shine.setColorAt(0.45, withAlpha(light, spec[4] * (s.dark ? 0.8 : 1.0)));
        shine.setColorAt(1.0, withAlpha(light, 0.05));
        p.setPen(Qt::NoPen);
        p.setBrush(shine);
        p.drawPath(band);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(withAlpha(light, spec[4] * 1.5), 0.6));
        p.drawPath(band);
    }

    // Bokeh far away, then the bubbles.
    drawBlurred(p, r, 2.2, [&](QPainter &lp) {
        lp.setPen(Qt::NoPen);
        for (int i = 0; i < 14; ++i) {
            const QPointF at(r.left() + r.width() * random01(seed, i),
                             r.top() + r.height() * random01(seed + 1, i));
            const qreal radius = 2.0 + 4.0 * random01(seed + 2, i);
            lp.setBrush(s.dark ? QColor(120, 220, 255, 60) : QColor(255, 255, 255, 110));
            lp.drawEllipse(at, radius, radius);
        }
    });
    const std::array<std::array<qreal, 3>, 9> bubbles = {{{0.035, 0.28, 7.5},
                                                          {0.09, 0.7, 3.5},
                                                          {0.29, 0.1, 3.0},
                                                          {0.62, 0.2, 4.0},
                                                          {0.8, 0.52, 11.0},
                                                          {0.9, 0.14, 5.0},
                                                          {0.97, 0.78, 4.0},
                                                          {0.72, 0.84, 3.0},
                                                          {0.5, 0.86, 2.4}}};
    for (const auto &b : bubbles)
        drawBubble(p, QPointF(r.left() + r.width() * b[0], r.top() + r.height() * b[1]), b[2], s.dark);
}

// -------------------------------------------------------------- Outrun Sunset

void paintSynthwave(QPainter &p, const Stage &s)
{
    const QRectF r = s.r;
    const quint32 seed = 0x5A7Eu;
    const qreal horizon = s.horizon;
    const QRectF sky(r.left(), r.top(), r.width(), horizon - r.top());
    if (s.dark) {
        fillSky(p, sky, {{0.0, QColor(10, 4, 32)}, {0.55, QColor(58, 12, 92)}, {1.0, QColor(255, 60, 128)}});
        drawStars(p, QRectF(sky.left(), sky.top(), sky.width(), sky.height() * 0.55), seed, int(r.width() * 0.3), 0.9);
    } else {
        fillSky(p, sky, {{0.0, QColor(248, 230, 255)}, {0.55, QColor(255, 212, 236)},
                         {1.0, QColor(255, 190, 170)}});
    }
    // The striped sun, resting on the horizon.
    const qreal sunR = std::min(30.0, (horizon - r.top()) * 0.5);
    const QPointF sunC(r.left() + r.width() * 0.8, horizon - sunR * 0.3);
    QPainterPath sun;
    sun.addEllipse(sunC, sunR, sunR);
    QPainterPath cuts;
    for (int i = 0; i < 6; ++i) {
        const qreal y = sunC.y() - sunR * 0.05 + i * sunR * 0.2;
        cuts.addRect(QRectF(sunC.x() - sunR - 1, y, 2 * sunR + 2, 0.7 + i * 0.55));
    }
    sun = sun.subtracted(cuts);
    drawBlurred(p, sun.boundingRect(), 6, [&](QPainter &lp) {
        lp.setPen(Qt::NoPen);
        lp.setBrush(s.dark ? QColor(255, 80, 150, 170) : QColor(255, 140, 150, 120));
        lp.drawEllipse(sunC, sunR * 1.05, sunR * 1.05);
    });
    QLinearGradient sunFill(0, sunC.y() - sunR, 0, sunC.y() + sunR * 0.6);
    sunFill.setColorAt(0.0, s.dark ? QColor(255, 236, 110) : QColor(255, 226, 120));
    sunFill.setColorAt(0.5, s.dark ? QColor(255, 140, 90) : QColor(255, 168, 130));
    sunFill.setColorAt(1.0, s.dark ? QColor(255, 50, 150) : QColor(255, 120, 176));
    p.setPen(Qt::NoPen);
    p.setBrush(sunFill);
    p.drawPath(sun);

    // Low-poly mountains on the left, rim-lit.
    QPainterPath range;
    const std::array<QPointF, 9> peaks = {QPointF(0.0, 0.0),  QPointF(0.07, 0.42), QPointF(0.13, 0.25),
                                          QPointF(0.21, 0.62), QPointF(0.3, 0.3),  QPointF(0.38, 0.48),
                                          QPointF(0.47, 0.18), QPointF(0.55, 0.3), QPointF(0.62, 0.0)};
    const qreal peakHeight = (horizon - r.top()) * 0.42;
    range.moveTo(r.left(), horizon);
    for (const QPointF &pk : peaks)
        range.lineTo(r.left() + r.width() * pk.x(), horizon - peakHeight * pk.y());
    range.closeSubpath();
    QLinearGradient rock(0, horizon - peakHeight, 0, horizon);
    rock.setColorAt(0.0, s.dark ? QColor(44, 12, 84) : QColor(206, 170, 255));
    rock.setColorAt(1.0, s.dark ? QColor(20, 4, 44) : QColor(236, 196, 250));
    p.setBrush(rock);
    p.drawPath(range);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(s.dark ? QColor(80, 240, 255, 200) : QColor(255, 255, 255, 230), 0.8));
    QPainterPath rim;
    rim.moveTo(r.left(), horizon);
    for (const QPointF &pk : peaks)
        rim.lineTo(r.left() + r.width() * pk.x(), horizon - peakHeight * pk.y());
    p.drawPath(rim);

    // The grid floor, receding to a vanishing point.
    const QRectF floor(r.left(), horizon, r.width(), r.bottom() - horizon);
    QLinearGradient ground(floor.topLeft(), floor.bottomLeft());
    ground.setColorAt(0.0, s.dark ? QColor(40, 6, 60) : QColor(250, 222, 250));
    ground.setColorAt(1.0, s.dark ? QColor(14, 2, 28) : QColor(236, 214, 255));
    p.setPen(Qt::NoPen);
    p.fillRect(floor, ground);
    const QColor line = s.dark ? QColor(255, 70, 230) : QColor(214, 96, 230);
    const QPointF vanish(r.left() + r.width() * 0.5, horizon - 6);
    p.save();
    p.setClipRect(floor);
    const auto gridLines = [&](QPainter &lp, qreal width, int alpha) {
        lp.setPen(QPen(withAlpha(line, alpha / 255.0), width));
        for (int i = -16; i <= 16; ++i) {
            const QPointF bottom(vanish.x() + i * r.width() * 0.09, floor.bottom() + 30);
            lp.drawLine(vanish, bottom);
        }
        qreal y = horizon + 1.2;
        qreal step = 1.6;
        while (y < floor.bottom() + 2) {
            lp.drawLine(QPointF(floor.left(), y), QPointF(floor.right(), y));
            y += step;
            step *= 1.45;
        }
    };
    drawBlurred(p, floor, 1.4, [&](QPainter &lp) { gridLines(lp, 1.4, s.dark ? 220 : 160); });
    gridLines(p, 0.6, 255);
    p.restore();
    // The horizon line itself glows.
    p.setPen(QPen(s.dark ? QColor(255, 200, 250) : QColor(255, 255, 255), 0.8));
    p.drawLine(QPointF(r.left(), horizon), QPointF(r.right(), horizon));
}

// -------------------------------------------------------------- Sakura Breeze

QPainterPath petalPath(const QPointF &at, qreal length, qreal angleDegrees)
{
    QPainterPath petal;
    petal.moveTo(0, 0);
    petal.cubicTo(length * 0.35, -length * 0.42, length * 0.9, -length * 0.3, length, -length * 0.08);
    petal.lineTo(length * 0.86, 0);
    petal.lineTo(length, length * 0.08);
    petal.cubicTo(length * 0.9, length * 0.3, length * 0.35, length * 0.42, 0, 0);
    petal.closeSubpath();
    QTransform t;
    t.translate(at.x(), at.y());
    t.rotate(angleDegrees);
    return t.map(petal);
}

void drawBlossom(QPainter &p, const QPointF &c, qreal radius, qreal turn, bool dark)
{
    for (int i = 0; i < 5; ++i) {
        const qreal angle = turn + i * 72.0;
        const QPainterPath petal = petalPath(c, radius, angle);
        const qreal a = qDegreesToRadians(angle);
        const QPointF tip = c + QPointF(std::cos(a), std::sin(a)) * radius;
        QLinearGradient fill(c, tip);
        fill.setColorAt(0.0, dark ? QColor(255, 120, 170) : QColor(255, 106, 160));
        fill.setColorAt(0.55, dark ? QColor(255, 196, 222) : QColor(255, 176, 208));
        fill.setColorAt(1.0, dark ? QColor(255, 236, 246) : QColor(255, 232, 242));
        p.setPen(QPen(dark ? QColor(200, 90, 140, 160) : QColor(226, 110, 156, 170), 0.35));
        p.setBrush(fill);
        p.drawPath(petal);
    }
    // Stamens.
    p.setPen(QPen(QColor(200, 40, 90, 200), 0.35));
    for (int i = 0; i < 7; ++i) {
        const qreal a = qDegreesToRadians(turn + 18 + i * 51.4);
        const QPointF end = c + QPointF(std::cos(a), std::sin(a)) * radius * 0.42;
        p.drawLine(c, end);
        p.setBrush(QColor(255, 214, 90));
        p.drawEllipse(end, 0.4, 0.4);
    }
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(210, 50, 100));
    p.drawEllipse(c, radius * 0.16, radius * 0.16);
}

void paintSakura(QPainter &p, const Stage &s)
{
    const QRectF r = s.r;
    const quint32 seed = 0x5A4Du;
    if (s.dark) {
        fillSky(p, r, {{0.0, QColor(20, 14, 42)}, {0.6, QColor(36, 26, 64)}, {1.0, QColor(52, 36, 80)}});
        const QPointF moon(r.left() + r.width() * 0.74, r.top() + r.height() * 0.42);
        QRadialGradient halo(moon, 40);
        halo.setColorAt(0.0, QColor(255, 220, 236, 110));
        halo.setColorAt(1.0, QColor(255, 220, 236, 0));
        p.setPen(Qt::NoPen);
        p.setBrush(halo);
        p.drawEllipse(moon, 40, 40);
        QRadialGradient disc(moon + QPointF(-3, -3), 17);
        disc.setColorAt(0.0, QColor(255, 250, 238));
        disc.setColorAt(1.0, QColor(240, 222, 214));
        p.setBrush(disc);
        p.drawEllipse(moon, 15, 15);
    } else {
        fillSky(p, r, {{0.0, QColor(255, 247, 250)}, {0.6, QColor(255, 230, 240)},
                       {1.0, QColor(255, 216, 232)}});
        QRadialGradient warm(r.topLeft() + QPointF(r.width() * 0.2, 0), r.width() * 0.5);
        warm.setColorAt(0.0, QColor(255, 250, 230, 180));
        warm.setColorAt(1.0, QColor(255, 250, 230, 0));
        p.fillRect(r, warm);
    }

    // Soft blossoms far off.
    drawBlurred(p, r, 2.6, [&](QPainter &lp) {
        lp.setPen(Qt::NoPen);
        for (int i = 0; i < 16; ++i) {
            const QPointF at(r.left() + r.width() * random01(seed + 5, i),
                             r.top() + r.height() * (0.45 + 0.55 * random01(seed + 6, i)));
            const qreal radius = 3 + 5 * random01(seed + 7, i);
            lp.setBrush(s.dark ? QColor(255, 150, 200, 55) : QColor(255, 170, 205, 90));
            lp.drawEllipse(at, radius, radius);
        }
    });

    // The branch reaches in from the top right.
    const QPointF start(r.right() + 4, r.top() + r.height() * 0.1);
    const QPointF end(r.left() + r.width() * 0.62, r.top() + r.height() * 0.16);
    QPainterPath branch;
    branch.moveTo(start);
    branch.cubicTo(start + QPointF(-r.width() * 0.12, r.height() * 0.26),
                   end + QPointF(r.width() * 0.12, r.height() * 0.1), end);
    QPainterPath twig;
    twig.moveTo(branch.pointAtPercent(0.4));
    twig.quadTo(branch.pointAtPercent(0.4) + QPointF(-4, 12), branch.pointAtPercent(0.4) + QPointF(-12, 17));
    QPainterPath twig2;
    twig2.moveTo(branch.pointAtPercent(0.72));
    twig2.quadTo(branch.pointAtPercent(0.72) + QPointF(-4, -8), branch.pointAtPercent(0.72) + QPointF(-14, -12));
    const QColor bark = s.dark ? QColor(30, 18, 26) : QColor(76, 44, 38);
    const QColor barkLight = s.dark ? QColor(90, 60, 80) : QColor(140, 96, 80);
    for (const auto &[path, width] : {std::pair{branch, 4.2}, std::pair{twig, 2.0}, std::pair{twig2, 1.6}}) {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(bark, width, Qt::SolidLine, Qt::RoundCap));
        p.drawPath(path);
        p.setPen(QPen(barkLight, width * 0.28, Qt::SolidLine, Qt::RoundCap));
        p.drawPath(path.translated(0, -width * 0.22));
    }
    // Blossoms and buds along it.
    const std::array<std::array<qreal, 3>, 7> onBranch = {{{0.12, 5.2, 10},
                                                           {0.3, 4.4, 40},
                                                           {0.5, 5.4, 5},
                                                           {0.64, 4.0, 60},
                                                           {0.85, 4.8, 25},
                                                           {0.98, 3.6, 70},
                                                           {0.2, 3.2, 20}}};
    for (const auto &b : onBranch) {
        const QPointF at = branch.pointAtPercent(b[0]) + QPointF(0, (int(b[0] * 10) % 2 ? 2.5 : -2.0));
        drawBlossom(p, at, b[1], b[2], s.dark);
    }
    drawBlossom(p, twig.pointAtPercent(1.0), 4.2, 12, s.dark);
    drawBlossom(p, twig2.pointAtPercent(1.0), 3.4, 48, s.dark);
    for (const qreal at : {0.58, 0.9}) {
        const QPointF bud = branch.pointAtPercent(at) + QPointF(1.5, 3.0);
        p.setPen(QPen(bark, 0.5));
        p.setBrush(s.dark ? QColor(255, 140, 190) : QColor(255, 110, 160));
        p.drawEllipse(bud, 1.5, 2.0);
    }

    // Petals on the breeze.
    for (int i = 0; i < 14; ++i) {
        const QPointF at(r.left() + r.width() * random01(seed + 20, i),
                         r.top() + r.height() * random01(seed + 21, i));
        const qreal length = 2.2 + 1.8 * random01(seed + 22, i);
        const QPainterPath petal = petalPath(at, length, 360.0 * random01(seed + 23, i));
        p.setPen(Qt::NoPen);
        p.setBrush(s.dark ? QColor(255, 196, 224, 200) : QColor(255, 150, 190, 210));
        p.drawPath(petal);
    }
}

void paintScene(QPainter &p, const QString &id, const Stage &stage)
{
    if (id == QLatin1String("scene.aurora"))
        paintAurora(p, stage);
    else if (id == QLatin1String("scene.meadow"))
        paintMeadow(p, stage);
    else if (id == QLatin1String("scene.aqua"))
        paintAqua(p, stage);
    else if (id == QLatin1String("scene.synthwave"))
        paintSynthwave(p, stage);
    else if (id == QLatin1String("scene.sakura"))
        paintSakura(p, stage);
}

} // namespace

ProfileScene::ProfileScene(QQuickItem *parent) : QQuickPaintedItem(parent)
{
    setAntialiasing(true);
}

void ProfileScene::setSceneId(const QString &sceneId)
{
    if (m_sceneId == sceneId)
        return;
    m_sceneId = sceneId;
    emit sceneIdChanged();
    update();
}

void ProfileScene::setDarkMode(bool dark)
{
    if (m_darkMode == dark)
        return;
    m_darkMode = dark;
    emit darkModeChanged();
    update();
}

void ProfileScene::setReadableRect(const QRectF &rect)
{
    if (m_readableRect == rect)
        return;
    m_readableRect = rect;
    emit readableRectChanged();
    update();
}

void ProfileScene::setFadeHeight(qreal height)
{
    if (qFuzzyCompare(m_fadeHeight, height))
        return;
    m_fadeHeight = height;
    emit fadeHeightChanged();
    update();
}

QImage ProfileScene::render(const QString &sceneId, const QSizeF &size, bool dark,
                            const QRectF &readable, qreal fadeHeight, qreal dpr)
{
    if (sceneId.isEmpty() || size.isEmpty())
        return transparentImage(size.isEmpty() ? QSizeF(1, 1) : size, dpr);
    // The painted scene is the expensive part and depends only on these; the
    // veil and the fade are cheap and follow the text, so they go on a copy.
    static QCache<QString, QImage> cache(8 * 1024);
    const QString key = QStringLiteral("%1|%2x%3|%4|%5|%6")
                            .arg(sceneId)
                            .arg(size.width(), 0, 'f', 1)
                            .arg(size.height(), 0, 'f', 1)
                            .arg(dark ? 1 : 0)
                            .arg(fadeHeight, 0, 'f', 1)
                            .arg(dpr, 0, 'f', 3);
    const QRectF r(QPointF(0, 0), size);
    QImage image;
    if (const QImage *cached = cache.object(key)) {
        image = *cached;
    } else {
        image = transparentImage(size, dpr);
        QPainter p(&image);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        // The horizon sits just above the fade, below the status line, so the
        // landscape shows between the header's text and the search field.
        const Stage stage{r, std::max(r.top() + r.height() * 0.6, r.bottom() - fadeHeight - 2.0),
                          dark, dpr};
        paintScene(p, sceneId, stage);
        p.end();
        cache.insert(key, new QImage(image), std::max<qsizetype>(1, image.sizeInBytes() / 1024));
    }

    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing, true);
    // Frost the glass behind the words so they read over any scene.
    if (!readable.isEmpty()) {
        const QColor veil = dark ? QColor(20, 34, 48, 120) : QColor(248, 252, 255, 150);
        drawBlurred(p, readable, 7.0, [&](QPainter &lp) {
            lp.setPen(Qt::NoPen);
            lp.setBrush(veil);
            lp.drawRoundedRect(readable, 8, 8);
        });
    }
    // Fade out into the sidebar along the bottom.
    if (fadeHeight > 0) {
        p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        QLinearGradient fade(0, r.bottom() - fadeHeight, 0, r.bottom());
        fade.setColorAt(0.0, QColor(0, 0, 0, 255));
        fade.setColorAt(1.0, QColor(0, 0, 0, 0));
        p.fillRect(QRectF(r.left(), r.bottom() - fadeHeight, r.width(), fadeHeight), fade);
    }
    return image;
}

void ProfileScene::paint(QPainter *painter)
{
    if (m_sceneId.isEmpty())
        return;
    painter->drawImage(QPointF(0, 0), render(m_sceneId, size(), m_darkMode, m_readableRect,
                                             m_fadeHeight, deviceScale(*painter)));
}

} // namespace OpenChat
