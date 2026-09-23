#include "cosmetics/FramePainters.h"

#include "cosmetics/CosmeticPaint.h"

#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QtMath>

#include <algorithm>
#include <array>
#include <cmath>

namespace OpenChat::FramePainters {

using namespace CosmeticPaint;

namespace {

QRectF grow(const QRectF &rect, qreal by)
{
    return rect.adjusted(-by, -by, by, by);
}

qreal smoothstep(qreal edge0, qreal edge1, qreal x)
{
    const qreal t = std::clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

QPainterPath ringPath(const QRectF &outer, qreal outerRadius, const QRectF &inner,
                      qreal innerRadius)
{
    QPainterPath path = roundedRect(outer, outerRadius);
    path.addPath(roundedRect(inner, std::max(0.0, innerRadius)));
    path.setFillRule(Qt::OddEvenFill);
    return path;
}

// Everything in the frame image outside `shape`: glows and shadows drawn
// through this clip never tint the frame's own translucent body.
QPainterPath outsideOf(const Canvas &c, const QPainterPath &shape)
{
    QPainterPath all;
    all.addRect(grow(c.bounds, 2));
    return all.subtracted(shape);
}

// A soft shadow under the frame on a light sidebar, a coloured glow on a dark one.
void drawHalo(QPainter &p, const Canvas &c, const QRectF &outer, qreal outerRadius,
              const QColor &lightShadow, const QColor &darkGlow, qreal blur)
{
    const QPainterPath shape = roundedRect(outer, outerRadius);
    p.save();
    p.setClipPath(outsideOf(c, shape));
    drawBlurred(p, outer, blur, [&](QPainter &lp) {
        lp.setPen(Qt::NoPen);
        lp.setBrush(c.dark ? darkGlow : lightShadow);
        lp.drawPath(c.dark ? shape : shape.translated(0, 0.6));
    });
    p.restore();
}

QLinearGradient polishedGold(const QRectF &area)
{
    QLinearGradient g(area.topLeft(), area.bottomRight());
    g.setColorAt(0.00, QColor(0xff, 0xf8, 0xdc));
    g.setColorAt(0.13, QColor(0xf4, 0xd0, 0x6c));
    g.setColorAt(0.29, QColor(0xb0, 0x78, 0x1c));
    g.setColorAt(0.43, QColor(0xff, 0xe9, 0xa4));
    g.setColorAt(0.55, QColor(0xdc, 0xa6, 0x40));
    g.setColorAt(0.71, QColor(0x8c, 0x5a, 0x10));
    g.setColorAt(0.85, QColor(0xec, 0xc4, 0x62));
    g.setColorAt(1.00, QColor(0x9e, 0x6a, 0x18));
    return g;
}

// A leaf from `origin` along the unit vector `dir`, `length` long and `width` wide.
QPainterPath leafPath(const QPointF &origin, const QPointF &dir, qreal length, qreal width)
{
    const QPointF n(-dir.y(), dir.x());
    const QPointF tip = origin + dir * length;
    QPainterPath path;
    path.moveTo(origin);
    path.cubicTo(origin + dir * (length * 0.25) + n * (width * 0.62),
                 origin + dir * (length * 0.72) + n * (width * 0.42), tip);
    path.cubicTo(origin + dir * (length * 0.72) - n * (width * 0.42),
                 origin + dir * (length * 0.25) - n * (width * 0.62), origin);
    path.closeSubpath();
    return path;
}

// ----------------------------------------------------------------- Aero Glass

void paintAero(QPainter &p, const Canvas &c)
{
    const qreal s = c.scale;
    const qreal band = c.margin - 1.0;
    const QRectF outer = grow(c.picture, band);
    const qreal outerR = c.radius + band;
    const QPainterPath body =
        ringPath(outer, outerR, grow(c.picture, -0.5), c.radius - 0.5);

    drawHalo(p, c, outer, outerR, QColor(16, 48, 88, 95), QColor(96, 196, 255, 150), 1.6);

    // Tinted glass: the classic Aero horizon, bright above and deep below.
    QLinearGradient glass(outer.topLeft(), outer.bottomLeft());
    if (c.dark) {
        glass.setColorAt(0.00, QColor(178, 222, 250, 236));
        glass.setColorAt(0.47, QColor(96, 160, 208, 226));
        glass.setColorAt(0.50, QColor(38, 96, 148, 234));
        glass.setColorAt(1.00, QColor(64, 138, 192, 240));
    } else {
        glass.setColorAt(0.00, QColor(244, 251, 255, 246));
        glass.setColorAt(0.47, QColor(176, 216, 244, 240));
        glass.setColorAt(0.50, QColor(98, 160, 212, 244));
        glass.setColorAt(1.00, QColor(150, 202, 238, 248));
    }
    p.fillPath(body, glass);

    // Diagonal light streaks through the glass, as on a Vista window frame.
    p.save();
    p.setClipPath(body);
    p.setPen(Qt::NoPen);
    const qreal slant = outer.height() * 0.6;
    struct Streak
    {
        qreal at;
        qreal width;
        int alpha;
    };
    const std::array<Streak, 5> streaks = {
        {{-0.18, 0.13, 70}, {0.10, 0.05, 95}, {0.38, 0.19, 42}, {0.66, 0.06, 90}, {0.80, 0.12, 55}}};
    for (const Streak &streak : streaks) {
        const qreal x = outer.left() + outer.width() * streak.at;
        const qreal w = outer.width() * streak.width;
        const QPolygonF poly{QPointF(x + slant, outer.top()), QPointF(x + slant + w, outer.top()),
                             QPointF(x + w, outer.bottom()), QPointF(x, outer.bottom())};
        p.setBrush(QColor(255, 255, 255, c.dark ? streak.alpha * 2 / 3 : streak.alpha));
        p.drawPolygon(poly);
    }
    p.restore();

    p.setBrush(Qt::NoBrush);
    // The rim, and a bright bevel just inside it that fades down the sides.
    p.setPen(QPen(c.dark ? QColor(4, 18, 32, 240) : QColor(34, 76, 122, 240), 1.0));
    p.drawPath(roundedRect(grow(outer, -0.5), outerR - 0.5));
    QLinearGradient bevel(outer.topLeft(), outer.bottomLeft());
    bevel.setColorAt(0.0, QColor(255, 255, 255, c.dark ? 190 : 250));
    bevel.setColorAt(0.5, QColor(255, 255, 255, c.dark ? 50 : 110));
    bevel.setColorAt(1.0, QColor(255, 255, 255, c.dark ? 95 : 170));
    p.setPen(QPen(QBrush(bevel), 0.9));
    p.drawPath(roundedRect(grow(outer, -1.45), outerR - 1.45));
    // The seat the picture is set into: a dark groove with a lit lip under it.
    p.setPen(QPen(c.dark ? QColor(2, 12, 22, 235) : QColor(36, 70, 104, 220), 1.0));
    p.drawPath(roundedRect(c.picture, c.radius));
    QLinearGradient lip(c.picture.topLeft(), c.picture.bottomLeft());
    lip.setColorAt(0.0, QColor(255, 255, 255, 0));
    lip.setColorAt(0.6, QColor(255, 255, 255, c.dark ? 40 : 70));
    lip.setColorAt(1.0, QColor(255, 255, 255, c.dark ? 130 : 220));
    p.setPen(QPen(QBrush(lip), 0.9));
    p.drawPath(roundedRect(grow(c.picture, 1.0), c.radius + 1.0));

    drawGlint(p, QPointF(outer.left() + outerR * 0.62, outer.top() + outerR * 0.5),
              std::max(2.2, 2.8 * s), QColor(210, 240, 255), 0.95);
}

// ------------------------------------------------------------ Gilded Filigree

void paintGilded(QPainter &p, const Canvas &c)
{
    const qreal s = c.scale;
    const qreal band = c.margin - 0.6;
    const QRectF outer = grow(c.picture, band);
    const qreal outerR = c.radius + band;
    const QPainterPath body = ringPath(outer, outerR, grow(c.picture, -0.5), c.radius - 0.5);
    const QColor ink(70, 40, 6);

    drawHalo(p, c, outer, outerR, QColor(60, 36, 0, 110), QColor(255, 196, 80, 105), 1.5);
    p.fillPath(body, polishedGold(outer));

    // A recessed channel round the middle of the band for the beading.
    const qreal mid = band * 0.52;
    const QRectF midRect = grow(c.picture, mid);
    const qreal midR = c.radius + mid;
    const qreal channel = std::max(1.3, band * 0.44);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(104, 64, 10, 215), channel));
    p.drawPath(roundedRect(midRect, midR));
    // The channel's lit lower-right wall.
    QLinearGradient wall(midRect.topLeft(), midRect.bottomRight());
    wall.setColorAt(0.0, QColor(255, 236, 170, 0));
    wall.setColorAt(1.0, QColor(255, 236, 170, 150));
    p.setPen(QPen(QBrush(wall), 0.6));
    p.drawPath(roundedRect(grow(midRect, -channel * 0.5), midR - channel * 0.5));

    // Rims: dark outline, a bright bevel just inside it, and the picture's seat.
    p.setPen(QPen(ink, 0.9));
    p.drawPath(roundedRect(grow(outer, -0.45), outerR - 0.45));
    p.drawPath(roundedRect(c.picture, c.radius));
    QLinearGradient bevel(outer.topLeft(), outer.bottomRight());
    bevel.setColorAt(0.0, QColor(255, 252, 232, 250));
    bevel.setColorAt(0.55, QColor(255, 240, 190, 110));
    bevel.setColorAt(1.0, QColor(255, 232, 160, 50));
    p.setPen(QPen(QBrush(bevel), 0.75));
    p.drawPath(roundedRect(grow(outer, -1.25), outerR - 1.25));

    // Corner points, where the ornaments go.
    const std::array<QPointF, 4> boundsCorners = {midRect.topLeft(), midRect.topRight(),
                                                  midRect.bottomRight(), midRect.bottomLeft()};
    const std::array<QPointF, 4> inward = {QPointF(1, 1), QPointF(-1, 1), QPointF(-1, -1),
                                           QPointF(1, -1)};
    const qreal ornamentReach = 6.2 * s;
    const QPointF rubyCentre(midRect.center().x(), midRect.top());

    // Beading in the channel, clear of the ornaments and the ruby.
    const QPainterPath track = roundedRect(midRect, midR);
    const qreal length = track.length();
    const qreal spacing = std::max(2.3, 2.55 * s);
    const int beads = std::max(8, int(length / spacing));
    const qreal beadR = std::max(0.62, 0.74 * s);
    p.setPen(Qt::NoPen);
    for (int i = 0; i < beads; ++i) {
        const QPointF pt = track.pointAtPercent(qreal(i) / beads);
        bool clear = QLineF(pt, rubyCentre).length() > 4.6 * s;
        for (const QPointF &corner : boundsCorners)
            clear = clear && QLineF(pt, corner).length() > ornamentReach;
        if (!clear)
            continue;
        QRadialGradient bead(pt - QPointF(beadR * 0.35, beadR * 0.4), beadR * 1.35);
        bead.setColorAt(0.0, QColor(255, 252, 232));
        bead.setColorAt(0.45, QColor(240, 196, 88));
        bead.setColorAt(1.0, QColor(110, 66, 8));
        p.setBrush(bead);
        p.drawEllipse(pt, beadR, beadR);
    }

    // Corner ornaments: two acanthus leaves along the edges, a small leaf
    // pointing out, and a round boss where they meet.
    for (int k = 0; k < 4; ++k) {
        const QPointF in = inward[size_t(k)];
        const QPointF origin = boundsCorners[size_t(k)] + in * (midR * (1.0 - M_SQRT1_2));
        const QPointF alongH(in.x(), 0.0);
        const QPointF alongV(0.0, in.y());
        const QPointF out = -in / std::sqrt(2.0);
        const qreal leafLength = 5.6 * s;
        const qreal leafWidth = std::max(1.6, 2.0 * s);
        QPainterPath leaves = leafPath(origin, alongH, leafLength, leafWidth);
        leaves.addPath(leafPath(origin, alongV, leafLength, leafWidth));
        leaves.addPath(leafPath(origin, out, 2.9 * s, 1.5 * s));
        leaves.setFillRule(Qt::WindingFill);
        QPainterPath shadow = leaves.translated(0.35, 0.55);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(40, 20, 0, 90));
        p.drawPath(shadow);
        QLinearGradient leafGold(origin - QPointF(leafLength, leafLength),
                                 origin + QPointF(leafLength, leafLength));
        leafGold.setColorAt(0.0, QColor(255, 250, 220));
        leafGold.setColorAt(0.45, QColor(246, 206, 100));
        leafGold.setColorAt(0.8, QColor(196, 138, 36));
        leafGold.setColorAt(1.0, QColor(150, 98, 20));
        p.setBrush(leafGold);
        p.setPen(QPen(ink, 0.5));
        p.drawPath(leaves);
        // Veins.
        p.setPen(QPen(QColor(110, 66, 10, 190), 0.4));
        p.drawLine(origin + alongH * (leafLength * 0.2), origin + alongH * (leafLength * 0.85));
        p.drawLine(origin + alongV * (leafLength * 0.2), origin + alongV * (leafLength * 0.85));
        // The boss.
        const qreal bossR = std::max(1.25, 1.55 * s);
        QRadialGradient boss(origin - QPointF(bossR * 0.4, bossR * 0.45), bossR * 1.4);
        boss.setColorAt(0.0, QColor(255, 255, 245));
        boss.setColorAt(0.4, QColor(250, 212, 110));
        boss.setColorAt(1.0, QColor(120, 72, 8));
        p.setBrush(boss);
        p.setPen(QPen(ink, 0.5));
        p.drawEllipse(origin, bossR, bossR);
    }

    // A cabochon ruby in a gold bezel at the top.
    const qreal rx = 2.7 * s;
    const qreal ry = 2.15 * s;
    p.setPen(QPen(ink, 0.55));
    p.setBrush(polishedGold(QRectF(rubyCentre.x() - rx - 1, rubyCentre.y() - ry - 1,
                                   2 * rx + 2, 2 * ry + 2)));
    p.drawEllipse(rubyCentre, rx + 0.95 * s, ry + 0.95 * s);
    QRadialGradient ruby(rubyCentre + QPointF(-rx * 0.3, -ry * 0.35), rx * 1.3);
    ruby.setColorAt(0.0, QColor(255, 186, 198));
    ruby.setColorAt(0.22, QColor(255, 52, 88));
    ruby.setColorAt(0.68, QColor(176, 8, 42));
    ruby.setColorAt(1.0, QColor(70, 0, 14));
    p.setBrush(ruby);
    p.setPen(QPen(QColor(64, 0, 12), 0.5));
    p.drawEllipse(rubyCentre, rx, ry);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 215));
    p.drawEllipse(rubyCentre + QPointF(-rx * 0.38, -ry * 0.42), rx * 0.34, ry * 0.24);

    drawGlint(p, QPointF(outer.left() + outer.width() * 0.8, outer.bottom() - band * 0.5),
              std::max(2.0, 2.6 * s), QColor(255, 236, 170), 0.9);
}

// ---------------------------------------------------------------- Neon Tubes

QPainterPath halfTube(const QRectF &t, qreal radius, qreal gap, bool left)
{
    const qreal cx = t.center().x();
    const qreal d = 2 * radius;
    QPainterPath path;
    if (left) {
        path.moveTo(cx - gap / 2, t.top());
        path.lineTo(t.left() + radius, t.top());
        path.arcTo(QRectF(t.left(), t.top(), d, d), 90, 90);
        path.lineTo(t.left(), t.bottom() - radius);
        path.arcTo(QRectF(t.left(), t.bottom() - d, d, d), 180, 90);
        path.lineTo(cx - gap / 2, t.bottom());
    } else {
        path.moveTo(cx + gap / 2, t.top());
        path.lineTo(t.right() - radius, t.top());
        path.arcTo(QRectF(t.right() - d, t.top(), d, d), 90, -90);
        path.lineTo(t.right(), t.bottom() - radius);
        path.arcTo(QRectF(t.right() - d, t.bottom() - d, d, d), 0, -90);
        path.lineTo(cx + gap / 2, t.bottom());
    }
    return path;
}

void paintNeon(QPainter &p, const Canvas &c)
{
    const qreal s = c.scale;
    const qreal inset = c.margin * 0.5;
    const QRectF t = grow(c.picture, inset);
    const qreal radius = c.radius + inset;
    const qreal w = std::max(1.5, 1.95 * std::sqrt(s));
    const qreal gap = std::max(4.0, 5.6 * s);

    struct Tube
    {
        QPainterPath path;
        QColor colour;
        QColor core;
    };
    const std::array<Tube, 2> tubes = {
        Tube{halfTube(t, radius, gap, true),
             c.dark ? QColor(255, 70, 200) : QColor(236, 26, 164), QColor(255, 236, 250)},
        Tube{halfTube(t, radius, gap, false),
             c.dark ? QColor(44, 226, 255) : QColor(0, 178, 232), QColor(232, 253, 255)}};

    // Bloom: a wide soft glow, then a tight bright one.
    for (const Tube &tube : tubes) {
        drawBlurred(p, t, c.margin * 0.62, [&](QPainter &lp) {
            lp.setBrush(Qt::NoBrush);
            lp.setPen(QPen(withAlpha(tube.colour, c.dark ? 1.0 : 0.8), w * 2.8, Qt::SolidLine,
                           Qt::FlatCap, Qt::RoundJoin));
            lp.drawPath(tube.path);
        });
    }
    for (const Tube &tube : tubes) {
        drawBlurred(p, t, 1.1, [&](QPainter &lp) {
            lp.setBrush(Qt::NoBrush);
            lp.setPen(QPen(withAlpha(tube.colour, 0.9), w * 1.7, Qt::SolidLine, Qt::FlatCap,
                           Qt::RoundJoin));
            lp.drawPath(tube.path);
        });
    }
    p.setBrush(Qt::NoBrush);
    for (const Tube &tube : tubes) {
        if (!c.dark) {
            p.setPen(QPen(withAlpha(darker(tube.colour, 0.45), 0.75), w + 0.9, Qt::SolidLine,
                          Qt::FlatCap, Qt::RoundJoin));
            p.drawPath(tube.path);
        }
        p.setPen(QPen(tube.colour, w, Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin));
        p.drawPath(tube.path);
        p.setPen(QPen(tube.core, w * 0.42, Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin));
        p.drawPath(tube.path);
    }

    // Electrode sleeves where each tube ends, facing across the gaps.
    const qreal sleeveLength = std::max(1.6, 2.0 * s);
    const qreal sleeveHeight = w * 1.35;
    const qreal cx = t.center().x();
    for (const qreal y : {t.top(), t.bottom()}) {
        for (const int side : {-1, 1}) {
            const qreal end = cx + side * gap / 2;
            const QRectF sleeve(side < 0 ? end - sleeveLength * 0.45 : end - sleeveLength * 0.55,
                                y - sleeveHeight / 2, sleeveLength, sleeveHeight);
            QLinearGradient metal(sleeve.topLeft(), sleeve.bottomLeft());
            metal.setColorAt(0.0, QColor(200, 206, 214));
            metal.setColorAt(0.45, QColor(96, 104, 114));
            metal.setColorAt(1.0, QColor(46, 50, 58));
            p.setPen(QPen(QColor(20, 22, 28, 200), 0.4));
            p.setBrush(metal);
            p.drawRoundedRect(sleeve, 0.5, 0.5);
        }
    }
}

// ----------------------------------------------------------------- 8-Bit Hero

void paintPixel(QPainter &p, const Canvas &c)
{
    p.setRenderHint(QPainter::Antialiasing, false);
    const qreal u = std::max(1.0, std::floor(c.margin / 3.0));
    const QRectF pic = c.picture;
    const QColor outline(30, 18, 40);
    const QColor light(255, 240, 176);
    const QColor body(250, 172, 42);
    const QColor shade(200, 94, 10);
    const QColor rivet(120, 52, 6);

    const auto bars = [&](const QRectF &o, const QColor &top, const QColor &left,
                          const QColor &bottom, const QColor &right, bool cut) {
        const qreal k = cut ? u : 0.0;
        p.fillRect(QRectF(o.left() + k, o.top(), o.width() - 2 * k, u), top);
        p.fillRect(QRectF(o.left() + k, o.bottom() - u, o.width() - 2 * k, u), bottom);
        p.fillRect(QRectF(o.left(), o.top() + u, u, o.height() - 2 * u), left);
        p.fillRect(QRectF(o.right() - u, o.top() + u, u, o.height() - 2 * u), right);
    };
    const QRectF o0 = grow(pic, 3 * u);
    const QRectF o1 = grow(pic, 2 * u);
    const QRectF o2 = grow(pic, u);

    // A hard one-unit drop shadow, as sprites had.
    if (!c.dark) {
        const QColor drop(20, 30, 50, 60);
        p.fillRect(QRectF(o0.left() + 2 * u, o0.bottom(), o0.width() - 2 * u, u).intersected(c.bounds), drop);
        p.fillRect(QRectF(o0.right(), o0.top() + 2 * u, u, o0.height() - 2 * u).intersected(c.bounds), drop);
    }
    bars(o0, outline, outline, outline, outline, true);
    bars(o1, light, light, body, body, false);
    bars(o2, body, body, shade, shade, false);
    // Stair-stepped outer corners.
    for (const QPointF &corner : {o1.topLeft(), QPointF(o1.right() - u, o1.top()),
                                  QPointF(o1.left(), o1.bottom() - u),
                                  QPointF(o1.right() - u, o1.bottom() - u)})
        p.fillRect(QRectF(corner, QSizeF(u, u)), outline);
    // The inner lip squares off the picture's rounded corners.
    bars(pic, outline, outline, outline, outline, false);
    for (const QPointF &corner : {pic.topLeft(), QPointF(pic.right() - u, pic.top()),
                                  QPointF(pic.left(), pic.bottom() - u),
                                  QPointF(pic.right() - u, pic.bottom() - u)})
        p.fillRect(QRectF(corner, QSizeF(u, u)), outline);
    // Rivets at the corners of the body ring.
    for (const QPointF &corner : {o2.topLeft(), QPointF(o2.right() - u, o2.top()),
                                  QPointF(o2.left(), o2.bottom() - u)})
        p.fillRect(QRectF(corner, QSizeF(u, u)), rivet);

    // An extra life in the bottom-right corner.
    static const std::array<const char *, 7> heart = {
        ".XX.XX.", "XWRXRRX", "XRRRRDX", "XRRRRDX", ".XRRDX.", "..XDX..", "...X..."};
    const qreal hu = std::max(1.0, std::floor(pic.width() / 36.0));
    const QPointF origin(std::round(o0.right() - 7 * hu), std::round(o0.bottom() - 7 * hu));
    for (int row = 0; row < 7; ++row) {
        for (int col = 0; col < 7; ++col) {
            QColor colour;
            switch (heart[size_t(row)][col]) {
            case 'X': colour = outline; break;
            case 'W': colour = QColor(255, 255, 255); break;
            case 'R': colour = QColor(236, 36, 64); break;
            case 'D': colour = QColor(160, 14, 40); break;
            default: continue;
            }
            p.fillRect(QRectF(origin.x() + col * hu, origin.y() + row * hu, hu, hu), colour);
        }
    }
    p.setRenderHint(QPainter::Antialiasing, true);
}

// ------------------------------------------------------------------ Frostbite

void drawFern(QPainter &p, const QPointF &root, const QPointF &dir, qreal length, qreal s,
              quint32 seed)
{
    const QPointF n(-dir.y(), dir.x());
    QPainterPath fern;
    const QPointF tip = root + dir * length + n * (length * 0.12);
    fern.moveTo(root);
    fern.quadTo(root + dir * (length * 0.5) - n * (length * 0.05), tip);
    const int branches = std::max(3, int(length / (1.5 * s)));
    for (int i = 1; i < branches; ++i) {
        const qreal t = qreal(i) / branches;
        const QPointF at = root + dir * (length * t) + n * (length * 0.12 * t * t);
        const qreal branch = length * 0.34 * (1.0 - t) * (0.7 + 0.5 * random01(seed, i));
        for (const int side : {-1, 1}) {
            const QPointF bdir = dir * 0.62 + n * (0.78 * side);
            fern.moveTo(at);
            fern.lineTo(at + bdir * branch);
        }
    }
    p.drawPath(fern);
}

void paintFrost(QPainter &p, const Canvas &c)
{
    const qreal s = c.scale;
    const quint32 seed = 0xF205u;
    const qreal band = c.margin - 1.2;
    const QRectF outer = grow(c.picture, band);
    const qreal outerR = c.radius + band;
    const QRectF seat = grow(c.picture, -0.5);
    const qreal seatR = std::max(0.0, c.radius - 0.5);
    const QPainterPath body = ringPath(outer, outerR, seat, seatR);

    // Frost ferns creeping in from the picture's corners, soft and pale.
    p.save();
    p.setClipPath(roundedRect(c.picture, c.radius));
    const qreal fernLength = c.picture.width() * 0.2;
    const std::array<std::pair<QPointF, QPointF>, 3> ferns = {
        std::pair{c.picture.topLeft(), QPointF(0.83, 0.56)},
        std::pair{c.picture.bottomRight(), QPointF(-0.6, -0.8)},
        std::pair{c.picture.bottomLeft(), QPointF(0.86, -0.5)}};
    int fernIndex = 0;
    for (const auto &[root, dir] : ferns) {
        const qreal len = fernLength * (fernIndex == 2 ? 0.7 : 1.0);
        drawBlurred(p, c.picture, 0.9, [&](QPainter &lp) {
            lp.setPen(QPen(QColor(210, 240, 255, 230), 1.1 * s, Qt::SolidLine, Qt::RoundCap));
            drawFern(lp, root, dir, len, s, seed + quint32(fernIndex));
        });
        p.setPen(QPen(QColor(255, 255, 255, 190), std::max(0.45, 0.5 * s), Qt::SolidLine,
                      Qt::RoundCap));
        drawFern(p, root, dir, len, s, seed + quint32(fernIndex));
        ++fernIndex;
    }
    p.restore();

    drawHalo(p, c, outer, outerR, QColor(30, 90, 140, 90), QColor(150, 225, 255, 150), 1.6);

    // The ice.
    QLinearGradient ice(outer.topLeft(), outer.bottomRight());
    ice.setColorAt(0.0, QColor(248, 254, 255, 246));
    ice.setColorAt(0.35, QColor(206, 241, 255, 236));
    ice.setColorAt(0.7, QColor(148, 212, 242, 238));
    ice.setColorAt(1.0, QColor(96, 172, 222, 244));
    p.fillPath(body, ice);

    // Facets: a zigzag of triangles between the outer and inner edges, each
    // catching the light a little differently.
    p.save();
    p.setClipPath(body);
    const QPainterPath outerPath = roundedRect(outer, outerR);
    const QPainterPath innerPath = roundedRect(seat, seatR);
    const int facets = std::max(16, int(outerPath.length() / (3.4 * s)));
    std::vector<QPointF> outs;
    std::vector<QPointF> ins;
    for (int i = 0; i <= facets; ++i) {
        const int wrap = i % facets;
        outs.push_back(outerPath.pointAtPercent(qreal(wrap) / facets));
        ins.push_back(innerPath.pointAtPercent((wrap + 0.3 + 0.4 * random01(seed, wrap)) / facets
                                               - (wrap + 1 == facets ? 1.0 / facets : 0.0) * 0.0));
    }
    p.setPen(QPen(QColor(255, 255, 255, 110), 0.35));
    for (int i = 0; i < facets; ++i) {
        const QPolygonF a{outs[size_t(i)], outs[size_t(i + 1)], ins[size_t(i)]};
        const QPolygonF b{ins[size_t(i)], ins[size_t(i + 1)], outs[size_t(i + 1)]};
        const qreal ra = random01(seed + 7, i);
        const qreal rb = random01(seed + 11, i);
        p.setBrush(ra > 0.5 ? QColor(255, 255, 255, int(40 + 110 * (ra - 0.5)))
                            : QColor(40, 120, 190, int(10 + 70 * ra)));
        p.drawPolygon(a);
        p.setBrush(rb > 0.55 ? QColor(255, 255, 255, int(30 + 100 * (rb - 0.5)))
                             : QColor(60, 150, 210, int(10 + 60 * rb)));
        p.drawPolygon(b);
    }
    p.restore();

    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(c.dark ? QColor(170, 230, 255, 235) : QColor(62, 138, 190, 235), 0.9));
    p.drawPath(roundedRect(grow(outer, -0.45), outerR - 0.45));
    p.setPen(QPen(QColor(255, 255, 255, 220), 0.7));
    p.drawPath(roundedRect(grow(outer, -1.2), outerR - 1.2));
    p.setPen(QPen(QColor(40, 110, 170, 200), 0.8));
    p.drawPath(roundedRect(c.picture, c.radius));

    // Icicles hanging off the top edge over the picture, short and clear.
    const std::array<std::pair<qreal, qreal>, 4> icicles = {
        {{0.30, 0.75}, {0.40, 1.0}, {0.49, 0.55}, {0.83, 0.8}}};
    for (const auto &[at, len] : icicles) {
        const qreal x = c.picture.left() + c.picture.width() * at;
        const qreal top = c.picture.top() - 0.5;
        const qreal h = 4.6 * s * len;
        const qreal half = std::max(0.8, 1.1 * s);
        QPainterPath icicle;
        icicle.moveTo(x - half, top);
        icicle.quadTo(x - half * 0.3, top + h * 0.6, x + 0.1, top + h);
        icicle.quadTo(x + half * 0.35, top + h * 0.5, x + half, top);
        icicle.closeSubpath();
        QLinearGradient g(x - half, 0, x + half, 0);
        g.setColorAt(0.0, QColor(255, 255, 255, 240));
        g.setColorAt(0.5, QColor(196, 236, 255, 200));
        g.setColorAt(1.0, QColor(110, 180, 230, 220));
        p.setPen(QPen(QColor(70, 140, 200, 170), 0.35));
        p.setBrush(g);
        p.drawPath(icicle);
    }

    // A snow drift along the top edge, heaped on the top-left corner and
    // draped a little way down the side: soft rounded lumps, lit from above.
    const qreal ridge = outer.top() + band * 0.5;
    const qreal drift = std::min(c.picture.top() - c.bounds.top() - band * 0.5 - 0.3, 5.0 * s);
    const qreal x0 = outer.left() + outerR * 0.3;
    const qreal x1 = outer.left() + outer.width() * 0.68;
    const std::array<std::array<qreal, 3>, 6> lumps = {{{0.04, 1.0, 0.1},
                                                        {0.17, 0.82, 0.09},
                                                        {0.31, 0.62, 0.08},
                                                        {0.45, 0.72, 0.09},
                                                        {0.6, 0.42, 0.08},
                                                        {0.78, 0.25, 0.09}}};
    const auto height = [&](qreal t) {
        qreal h = 0.0;
        for (const auto &lump : lumps) {
            const qreal d = (t - lump[0]) / lump[2];
            h = std::max(h, lump[1] * std::exp(-d * d * 0.5) * (1.0 - 0.15 * d * d));
        }
        return h * (1.0 - smoothstep(0.82, 1.0, t));
    };
    QPainterPath snow;
    snow.moveTo(x0, ridge + band * 0.45);
    for (qreal x = x0; x <= x1; x += 0.35) {
        const qreal t = (x - x0) / (x1 - x0);
        snow.lineTo(x, ridge - drift * height(t) - 0.4);
    }
    for (qreal x = x1; x >= x0; x -= 0.5) {
        const qreal t = (x - x0) / (x1 - x0);
        snow.lineTo(x, ridge + band * 0.3 + 0.5 * s * std::sin(t * 17.0) * (1.0 - t));
    }
    snow.closeSubpath();
    // The corner heap, rounding the drift over the corner onto the side.
    QPainterPath heap;
    heap.addEllipse(QPointF(outer.left() + outerR * 0.62, ridge + band * 0.05),
                    band * 0.5 + drift * 0.55, band * 0.5 + drift * 0.36);
    QPainterPath cap;
    cap.addEllipse(QPointF(outer.right() - outerR * 0.55, ridge - drift * 0.05), band * 0.5 + 0.9 * s,
                   band * 0.34 + drift * 0.2);
    snow = snow.united(heap).united(cap).simplified();
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(40, 90, 140, c.dark ? 110 : 60));
    p.drawPath(snow.translated(0.3, 0.9));
    QLinearGradient powder(0, ridge - drift, 0, ridge + band * 0.8);
    powder.setColorAt(0.0, QColor(255, 255, 255));
    powder.setColorAt(0.6, QColor(246, 251, 255));
    powder.setColorAt(1.0, QColor(206, 228, 244));
    p.setBrush(powder);
    p.setPen(QPen(c.dark ? QColor(170, 210, 236, 150) : QColor(110, 156, 196, 100), 0.45));
    p.drawPath(snow);
    // Blue shade in the hollows of the drift, and a sparkle or two on it.
    p.save();
    p.setClipPath(snow);
    p.setPen(QPen(QColor(130, 180, 222, 120), 1.1 * s));
    p.setBrush(Qt::NoBrush);
    p.drawPath(snow.translated(0, 1.3 * s));
    p.restore();
    for (int i = 0; i < 4; ++i) {
        const qreal x = x0 + (x1 - x0) * (0.12 + 0.2 * i + 0.05 * random01(seed + 40, i));
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255));
        drawGlint(p, QPointF(x, ridge - drift * 0.45), std::max(0.9, 1.1 * s), QColor(190, 230, 255), 0.9);
    }

    // A snowflake set into the bottom-right corner.
    const QPointF flake(outer.right() - outerR * 0.5 + 0.6, outer.bottom() - outerR * 0.5 + 0.6);
    const qreal fr = 3.9 * s;
    QPainterPath arms;
    for (int i = 0; i < 6; ++i) {
        const qreal a = qDegreesToRadians(90.0 + 60.0 * i);
        const QPointF d(std::cos(a), -std::sin(a));
        const QPointF n(-d.y(), d.x());
        arms.moveTo(flake);
        arms.lineTo(flake + d * fr);
        const QPointF b = flake + d * (fr * 0.55);
        arms.moveTo(b);
        arms.lineTo(b + (d + n * 1.1) * (fr * 0.28));
        arms.moveTo(b);
        arms.lineTo(b + (d - n * 1.1) * (fr * 0.28));
    }
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(30, 90, 150, 170), std::max(1.3, 1.5 * s), Qt::SolidLine, Qt::RoundCap));
    p.drawPath(arms);
    p.setPen(QPen(QColor(255, 255, 255), std::max(0.6, 0.7 * s), Qt::SolidLine, Qt::RoundCap));
    p.drawPath(arms);
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::white);
    p.drawEllipse(flake, 0.8 * s, 0.8 * s);

    drawGlint(p, QPointF(outer.right() - band * 0.5, outer.top() + outer.height() * 0.3),
              std::max(2.0, 2.5 * s), QColor(200, 240, 255));
    drawGlint(p, QPointF(outer.left() + band * 0.5, outer.bottom() - outer.height() * 0.28),
              std::max(1.6, 1.9 * s), QColor(200, 240, 255), 0.85);
}

// -------------------------------------------------------------------- Inferno

// Fire colours by intensity: a transparent deep red at the tips through orange
// to a yellow-white heart.
QRgb fireColour(qreal i)
{
    struct Stop
    {
        qreal at;
        int r, g, b, a;
    };
    static const std::array<Stop, 6> stops = {{{0.00, 150, 10, 0, 0},
                                               {0.16, 200, 34, 0, 150},
                                               {0.36, 255, 88, 0, 235},
                                               {0.58, 255, 156, 18, 250},
                                               {0.80, 255, 218, 84, 255},
                                               {1.00, 255, 250, 214, 255}}};
    i = std::clamp(i, 0.0, 1.0);
    for (size_t k = 1; k < stops.size(); ++k) {
        if (i <= stops[k].at) {
            const Stop &a = stops[k - 1];
            const Stop &b = stops[k];
            const qreal t = (i - a.at) / (b.at - a.at);
            return qRgba(int(a.r + (b.r - a.r) * t), int(a.g + (b.g - a.g) * t),
                         int(a.b + (b.b - a.b) * t), int(a.a + (b.a - a.a) * t));
        }
    }
    return qRgba(255, 250, 214, 255);
}

void paintInferno(QPainter &p, const Canvas &c)
{
    const qreal s = c.scale;
    const quint32 seed = 0x1F3Eu;
    const qreal band = c.margin - 0.8;
    const QRectF outer = grow(c.picture, band);
    const qreal outerR = c.radius + band;
    const QPainterPath body = ringPath(outer, outerR, grow(c.picture, -0.5), c.radius - 0.5);
    const qreal loop = qreal(c.phase) / std::max(1, c.phases);
    const qreal cycle = 2.0 * M_PI * loop;

    // Cracked lava rock: ridged noise gives molten veins in dark stone.
    const QImage rock = proceduralImage(outer, c.dpr, [&](qreal x, qreal y) {
        const qreal v = fractalNoise(x / (5.5 * s), y / (5.5 * s), seed, 3);
        const qreal vein = std::pow(1.0 - std::abs(v * 2.0 - 1.0), 9.0);
        const qreal grain = fractalNoise(x * 1.3, y * 1.3, seed + 5, 2);
        const qreal vertical = (y - outer.top()) / outer.height();
        const QColor stone = mix(QColor(74, 40, 26), QColor(22, 10, 6),
                                 0.35 + 0.5 * vertical + 0.25 * (grain - 0.5));
        const QColor lava = mix(QColor(255, 70, 0), QColor(255, 214, 90), vein * vein);
        return mix(stone, lava, std::min(1.0, vein * 1.6)).rgba();
    });

    // The fire: turbulence scrolling upwards, shaped by the distance from the
    // picture's edge. Flames reach high above the top, lick up the sides and
    // barely rise off the bottom. The two noise samples cross-fade so the last
    // phase flows back into the first without a jump.
    const QPointF centre = c.picture.center();
    const qreal halfW = c.picture.width() / 2 - c.radius;
    const qreal halfH = c.picture.height() / 2 - c.radius;
    const qreal reachTop = c.picture.top() - c.bounds.top();
    const qreal reachSide = c.margin * 0.75;
    const qreal reachBottom = c.margin * 0.3;
    const qreal period = 16.0 * s;
    const QImage fire = proceduralImage(c.bounds, c.dpr, [&](qreal x, qreal y) {
        const qreal qx = std::abs(x - centre.x()) - halfW;
        const qreal qy = std::abs(y - centre.y()) - halfH;
        const qreal ox = std::max(qx, 0.0);
        const qreal oy = std::max(qy, 0.0);
        const qreal outside = std::hypot(ox, oy);
        const qreal d = outside + std::min(std::max(qx, qy), 0.0) - c.radius;
        if (d < -0.6)
            return qRgba(0, 0, 0, 0);
        // How squarely this point sits above (or below) the picture.
        const qreal vertical = outside > 1e-6 ? oy / outside : (qy > qx ? 1.0 : 0.0);
        const qreal reach = y < centre.y() ? reachSide + (reachTop - reachSide) * vertical * vertical
                                           : reachSide + (reachBottom - reachSide) * vertical;
        const qreal nx = x / (2.1 * s);
        const qreal ny = y / (6.0 * s);
        const qreal a = fractalNoise(nx, ny + loop * period / (6.0 * s), seed + 17, 3);
        const qreal b = fractalNoise(nx, ny + (loop - 1.0) * period / (6.0 * s), seed + 17, 3);
        const qreal turbulence = smoothstep(0.2, 0.8, a * (1.0 - loop) + b * loop);
        const qreal height = reach * (0.35 + 0.85 * turbulence);
        qreal intensity = 1.0 - std::max(0.0, d) / std::max(0.5, height);
        intensity *= smoothstep(c.bounds.top(), c.bounds.top() + 2.5 * s, y);
        // Keep the rock readable: the fire is thinner right on the band.
        intensity *= 0.62 + 0.38 * smoothstep(-0.5, band, d);
        intensity = std::pow(std::max(0.0, intensity), 1.15);
        return fireColour(intensity * (0.85 + 0.3 * turbulence));
    });

    // Heat glow, then rock, then fire, then the firelight on the picture.
    p.save();
    p.setOpacity(c.dark ? 0.9 : 0.65);
    QImage glow = fire;
    blurImage(glow, 2.6 * s * c.dpr);
    p.drawImage(c.bounds.topLeft(), glow);
    p.restore();
    p.save();
    p.setClipPath(body);
    p.drawImage(outer.topLeft(), rock);
    p.restore();
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(24, 8, 2, 220), 0.9));
    p.drawPath(roundedRect(c.picture, c.radius));
    p.save();
    p.setClipPath(roundedRect(c.picture, c.radius));
    QLinearGradient cast(c.picture.topLeft(), c.picture.bottomLeft());
    cast.setColorAt(0.0, QColor(255, 120, 20, 110));
    cast.setColorAt(0.22, QColor(255, 120, 20, 22));
    cast.setColorAt(1.0, QColor(255, 120, 20, 0));
    p.fillRect(c.picture, cast);
    p.restore();
    p.drawImage(c.bounds.topLeft(), fire);

    // Embers drifting up out of the fire.
    p.setPen(Qt::NoPen);
    const qreal room = c.picture.top() - c.bounds.top();
    for (int i = 0; i < 7; ++i) {
        const qreal travel = std::fmod(loop + random01(seed + 9, i), 1.0);
        const qreal x = outer.left() + outer.width() * (0.06 + 0.88 * random01(seed + 13, i))
                        + std::sin(cycle + i) * 1.2 * s;
        const qreal y = c.picture.top() - room * (0.35 + 0.65 * travel);
        if (y < c.bounds.top() + 0.8)
            continue;
        const qreal alpha = 1.0 - travel * 0.7;
        const qreal r = std::max(0.5, 0.62 * s);
        QRadialGradient ember(QPointF(x, y), r * 2.4);
        ember.setColorAt(0.0, QColor(255, 244, 180, int(255 * alpha)));
        ember.setColorAt(0.3, QColor(255, 150, 30, int(220 * alpha)));
        ember.setColorAt(1.0, QColor(255, 80, 0, 0));
        p.setBrush(ember);
        p.drawEllipse(QPointF(x, y), r * 2.4, r * 2.4);
    }
}

// ------------------------------------------------------------- Stardust Orbit

void paintOrbit(QPainter &p, const Canvas &c)
{
    const qreal s = c.scale;
    const quint32 seed = 0x0B17u;
    const qreal band = c.margin - 0.7;
    const QRectF outer = grow(c.picture, band);
    const qreal outerR = c.radius + band;
    const QPainterPath body = ringPath(outer, outerR, grow(c.picture, -0.5), c.radius - 0.5);

    drawHalo(p, c, outer, outerR, QColor(30, 20, 80, 110), QColor(150, 110, 255, 150), 1.8);

    // Deep space with nebula clouds.
    const QImage space = proceduralImage(outer, c.dpr, [&](qreal x, qreal y) {
        const qreal n1 = fractalNoise(x / (7.0 * s), y / (7.0 * s), seed, 3);
        const qreal n2 = fractalNoise(x / (5.0 * s) + 11.0, y / (5.0 * s), seed + 3, 3);
        QColor colour = mix(QColor(8, 10, 34), QColor(22, 12, 58), (y - outer.top()) / outer.height());
        colour = mix(colour, QColor(140, 50, 190), std::pow(std::max(0.0, n1 - 0.35) * 1.6, 1.6) * 0.8);
        colour = mix(colour, QColor(40, 130, 230), std::pow(std::max(0.0, n2 - 0.45) * 1.8, 1.8) * 0.7);
        return colour.rgba();
    });
    p.save();
    p.setClipPath(body);
    p.drawImage(outer.topLeft(), space);
    // Pinprick stars.
    p.setPen(Qt::NoPen);
    const int stars = int(outer.width() * 1.4);
    for (int i = 0; i < stars; ++i) {
        const QPointF at(outer.left() + outer.width() * random01(seed + 21, i),
                         outer.top() + outer.height() * random01(seed + 22, i));
        const qreal b = random01(seed + 23, i);
        p.setBrush(QColor(255, 255, 255, int(90 + 165 * b)));
        const qreal r = (0.22 + 0.3 * b * b) * std::max(1.0, s);
        p.drawEllipse(at, r, r);
    }
    p.restore();

    // Silver rims.
    QLinearGradient silver(outer.topLeft(), outer.bottomRight());
    silver.setColorAt(0.0, QColor(255, 255, 255));
    silver.setColorAt(0.35, QColor(170, 180, 200));
    silver.setColorAt(0.55, QColor(236, 240, 250));
    silver.setColorAt(1.0, QColor(120, 128, 150));
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QBrush(silver), 0.95));
    p.drawPath(roundedRect(grow(outer, -0.48), outerR - 0.48));
    p.drawPath(roundedRect(c.picture, c.radius));

    // Two comets chasing each other round the orbit.
    const QPainterPath track = roundedRect(grow(c.picture, band * 0.5), c.radius + band * 0.5);
    const qreal progress = qreal(c.phase) / std::max(1, c.phases);
    const std::array<QColor, 2> tails = {QColor(120, 220, 255), QColor(255, 140, 230)};
    for (int k = 0; k < 2; ++k) {
        const qreal head = std::fmod(progress + 0.5 * k + 0.12, 1.0);
        const int segments = 18;
        for (int j = segments; j >= 1; --j) {
            const qreal at = std::fmod(head - j * 0.011 + 1.0, 1.0);
            const qreal fade = 1.0 - qreal(j) / segments;
            const QPointF pt = track.pointAtPercent(at);
            const qreal r = std::max(0.4, (0.4 + 1.25 * fade) * std::max(0.9, s));
            p.setPen(Qt::NoPen);
            p.setBrush(withAlpha(mix(tails[size_t(k)], QColor(150, 90, 255), 1.0 - fade), 0.15 + 0.6 * fade * fade));
            p.drawEllipse(pt, r, r);
        }
        const QPointF pt = track.pointAtPercent(head);
        drawBlurred(p, QRectF(pt - QPointF(3, 3), QSizeF(6, 6)), 1.6 * s, [&](QPainter &lp) {
            lp.setPen(Qt::NoPen);
            lp.setBrush(tails[size_t(k)]);
            lp.drawEllipse(pt, 2.6 * s, 2.6 * s);
        });
        drawGlint(p, pt, std::max(3.4, 4.0 * s), tails[size_t(k)]);
    }
}

} // namespace

qreal sideMargin(qreal avatarSize)
{
    return std::round(3.0 + 0.07 * avatarSize);
}

qreal extraTop(const QString &frameId, qreal avatarSize)
{
    if (frameId == QLatin1String("frame.inferno"))
        return std::round(0.11 * avatarSize);
    if (frameId == QLatin1String("frame.frost"))
        return std::round(0.07 * avatarSize);
    return 0.0;
}

int phaseCount(const QString &frameId)
{
    if (frameId == QLatin1String("frame.inferno"))
        return 8;
    if (frameId == QLatin1String("frame.orbit"))
        return 36;
    return 1;
}

int phaseInterval(const QString &frameId)
{
    if (frameId == QLatin1String("frame.inferno"))
        return 95;
    return 80;
}

void paint(QPainter &painter, const QString &frameId, const Canvas &canvas)
{
    if (frameId == QLatin1String("frame.aero"))
        paintAero(painter, canvas);
    else if (frameId == QLatin1String("frame.gilded"))
        paintGilded(painter, canvas);
    else if (frameId == QLatin1String("frame.neon"))
        paintNeon(painter, canvas);
    else if (frameId == QLatin1String("frame.pixel"))
        paintPixel(painter, canvas);
    else if (frameId == QLatin1String("frame.frost"))
        paintFrost(painter, canvas);
    else if (frameId == QLatin1String("frame.inferno"))
        paintInferno(painter, canvas);
    else if (frameId == QLatin1String("frame.orbit"))
        paintOrbit(painter, canvas);
}

} // namespace OpenChat::FramePainters
