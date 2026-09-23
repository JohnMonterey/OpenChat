#include "cosmetics/BeadArtItem.h"

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

// The four presence inks. Every style draws from these so a state reads the
// same whatever the bead is made of.
struct StateInk
{
    QColor base;
    QColor light;
    QColor dark;
    QColor rim;
};

StateInk inkFor(int presence)
{
    switch (presence) {
    case 0: // Available
        return {QColor(0x45, 0xc0, 0x1e), QColor(0xb4, 0xf5, 0x7c), QColor(0x22, 0x7e, 0x0c),
                QColor(0x2a, 0x68, 0x0e)};
    case 1: // Away
        return {QColor(0xff, 0xbe, 0x1a), QColor(0xff, 0xf0, 0x92), QColor(0xc8, 0x80, 0x00),
                QColor(0x9a, 0x62, 0x00)};
    case 3: // Busy
        return {QColor(0xe8, 0x36, 0x2a), QColor(0xff, 0xa6, 0x96), QColor(0xa0, 0x16, 0x0c),
                QColor(0x80, 0x12, 0x0a)};
    default: // Offline
        return {QColor(0xa6, 0xb1, 0xbb), QColor(0xf2, 0xf5, 0xf7), QColor(0x6c, 0x78, 0x84),
                QColor(0x5e, 0x69, 0x74)};
    }
}

// Light falls from the upper left: 1 for a face turned to it, 0 turned away.
qreal lightFacing(qreal angleRadians)
{
    const qreal lightAngle = qDegreesToRadians(-125.0);
    return 0.5 + 0.5 * std::cos(angleRadians - lightAngle);
}

// ------------------------------------------------------------ Brilliant Cut

void paintGem(QPainter &p, const QPointF &c, qreal size, const StateInk &ink, bool offline)
{
    const qreal R = size / 2.0 - 0.25;
    const qreal T = R * 0.52;
    std::array<QPointF, 8> outer{};
    std::array<QPointF, 8> table{};
    for (int i = 0; i < 8; ++i) {
        const qreal a = qDegreesToRadians(22.5 + 45.0 * i);
        outer[size_t(i)] = c + QPointF(std::cos(a), std::sin(a)) * R;
        table[size_t(i)] = c + QPointF(std::cos(a + M_PI / 8), std::sin(a + M_PI / 8)) * T;
    }
    QPolygonF girdle;
    for (const QPointF &pt : outer)
        girdle << pt;

    // A dark pavilion shows round the edge; the crown facets sit on it.
    p.setPen(Qt::NoPen);
    p.setBrush(ink.dark);
    p.drawPolygon(girdle);
    for (int i = 0; i < 8; ++i) {
        const QPointF o0 = outer[size_t(i)];
        const QPointF o1 = outer[size_t((i + 1) % 8)];
        const QPointF t0 = table[size_t((i + 7) % 8)];
        const QPointF t1 = table[size_t(i)];
        const qreal facing = lightFacing(std::atan2((o0.y() + o1.y()) / 2 - c.y(),
                                                    (o0.x() + o1.x()) / 2 - c.x()));
        // Each crown segment splits into a bright kite and a darker star facet.
        const QColor kite = facing > 0.5 ? mix(ink.base, ink.light, (facing - 0.5) * 1.8)
                                         : mix(ink.base, ink.dark, (0.5 - facing) * 1.5);
        p.setBrush(kite);
        p.drawPolygon(QPolygonF{o0, t1, o1});
        p.setBrush(mix(kite, ink.dark, 0.35));
        p.drawPolygon(QPolygonF{o0, t0, t1});
    }
    // The table: a flat window into the stone, lit from the top left.
    QPolygonF tablePoly;
    for (const QPointF &pt : table)
        tablePoly << pt;
    QLinearGradient tableFill(c - QPointF(T, T), c + QPointF(T, T));
    tableFill.setColorAt(0.0, lighter(ink.light, 0.3));
    tableFill.setColorAt(0.55, ink.base);
    tableFill.setColorAt(1.0, mix(ink.base, ink.dark, 0.3));
    p.setBrush(tableFill);
    p.drawPolygon(tablePoly);
    // Facet edges and the girdle outline.
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(withAlpha(ink.light, offline ? 0.55 : 0.45), std::max(0.3, size * 0.03)));
    for (int i = 0; i < 8; ++i)
        p.drawLine(outer[size_t(i)], table[size_t(i)]);
    p.drawPolygon(tablePoly);
    p.setPen(QPen(ink.rim, std::max(0.55, size * 0.06)));
    p.drawPolygon(girdle);
    // Fire: a white glint on the upper-left facets.
    drawGlint(p, c + QPointF(-R * 0.38, -R * 0.42), R * 0.62, QColor(255, 255, 255), 0.95);
}

// ------------------------------------------------------------- Candy Heart

QPainterPath heartPath(const QRectF &r)
{
    const auto at = [&r](qreal x, qreal y) {
        return QPointF(r.left() + r.width() * x, r.top() + r.height() * y);
    };
    QPainterPath path;
    path.moveTo(at(0.5, 0.97));
    path.cubicTo(at(0.43, 0.9), at(0.02, 0.64), at(0.02, 0.36));
    path.cubicTo(at(0.02, 0.15), at(0.16, 0.03), at(0.3, 0.03));
    path.cubicTo(at(0.4, 0.03), at(0.47, 0.1), at(0.5, 0.21));
    path.cubicTo(at(0.53, 0.1), at(0.6, 0.03), at(0.7, 0.03));
    path.cubicTo(at(0.84, 0.03), at(0.98, 0.15), at(0.98, 0.36));
    path.cubicTo(at(0.98, 0.64), at(0.57, 0.9), at(0.5, 0.97));
    path.closeSubpath();
    return path;
}

void paintHeart(QPainter &p, const QPointF &c, qreal size, const StateInk &ink)
{
    const qreal w = size * 1.06;
    const qreal h = size * 0.98;
    const QRectF box(c.x() - w / 2, c.y() - h / 2 + size * 0.02, w, h);
    const QPainterPath heart = heartPath(box);
    QRadialGradient fill(box.left() + w * 0.34, box.top() + h * 0.3, w * 0.78);
    fill.setColorAt(0.0, lighter(ink.light, 0.25));
    fill.setColorAt(0.35, ink.base);
    fill.setColorAt(0.85, mix(ink.base, ink.dark, 0.6));
    fill.setColorAt(1.0, ink.dark);
    p.setPen(QPen(ink.rim, std::max(0.55, size * 0.06)));
    p.setBrush(fill);
    p.drawPath(heart);
    // Candy gloss: a long highlight on the left lobe and a dot on the right.
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 225));
    p.save();
    p.translate(box.left() + w * 0.27, box.top() + h * 0.3);
    p.rotate(-38);
    p.drawEllipse(QPointF(0, 0), w * 0.15, h * 0.085);
    p.restore();
    p.setBrush(QColor(255, 255, 255, 170));
    p.drawEllipse(QPointF(box.left() + w * 0.71, box.top() + h * 0.24), w * 0.055, w * 0.055);
}

// --------------------------------------------------------------- Lucky Star

void paintStar(QPainter &p, const QPointF &c, qreal size, const StateInk &ink)
{
    const qreal R = size * 0.58;
    const qreal r = R * 0.5;
    const QPointF centre = c + QPointF(0, size * 0.04);
    std::array<QPointF, 10> pts{};
    for (int i = 0; i < 10; ++i) {
        const qreal a = qDegreesToRadians(-90.0 + 36.0 * i);
        pts[size_t(i)] = centre + QPointF(std::cos(a), std::sin(a)) * (i % 2 == 0 ? R : r);
    }
    // Ten bevel facets, each lit by how it faces the light.
    p.setPen(Qt::NoPen);
    for (int i = 0; i < 10; ++i) {
        const QPointF a = pts[size_t(i)];
        const QPointF b = pts[size_t((i + 1) % 10)];
        const qreal facing = lightFacing(std::atan2((a.y() + b.y()) / 2 - centre.y(),
                                                    (a.x() + b.x()) / 2 - centre.x()));
        const QColor shade = facing > 0.5 ? mix(ink.base, ink.light, (facing - 0.5) * 1.7)
                                          : mix(ink.base, ink.dark, (0.5 - facing) * 1.6);
        p.setBrush(shade);
        p.drawPolygon(QPolygonF{centre, a, b});
    }
    QPolygonF outline;
    for (const QPointF &pt : pts)
        outline << pt;
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(ink.rim, std::max(0.55, size * 0.06), Qt::SolidLine, Qt::SquareCap,
                  Qt::MiterJoin));
    p.drawPolygon(outline);
    drawGlint(p, centre + QPointF(-R * 0.18, -R * 0.3), R * 0.36, QColor(255, 255, 255), 0.9);
}

// --------------------------------------------------------------- Plasma Orb

void paintOrb(QPainter &p, const QPointF &c, qreal size, const StateInk &ink, bool offline,
              qreal overhang)
{
    const qreal R = size / 2.0 - 0.2;
    // Halo: the orb lights its surroundings; a switched-off orb barely does.
    QRadialGradient halo(c, R + overhang);
    halo.setColorAt(0.0, withAlpha(ink.base, offline ? 0.25 : 0.75));
    halo.setColorAt(R / (R + overhang), withAlpha(ink.base, offline ? 0.18 : 0.5));
    halo.setColorAt(1.0, withAlpha(ink.base, 0.0));
    p.setPen(Qt::NoPen);
    p.setBrush(halo);
    p.drawEllipse(c, R + overhang, R + overhang);
    // The glowing core fades out to a darker glass skin.
    QRadialGradient core(c + QPointF(0, R * 0.1), R);
    core.setColorAt(0.0, offline ? ink.light : QColor(255, 255, 240));
    core.setColorAt(0.28, lighter(ink.light, 0.2));
    core.setColorAt(0.62, ink.base);
    core.setColorAt(1.0, ink.dark);
    p.setBrush(core);
    p.setPen(QPen(withAlpha(ink.rim, 0.7), std::max(0.45, size * 0.045)));
    p.drawEllipse(c, R, R);
    // Glass reflection.
    p.setPen(Qt::NoPen);
    QLinearGradient shine(c.x(), c.y() - R, c.x(), c.y());
    shine.setColorAt(0.0, QColor(255, 255, 255, 230));
    shine.setColorAt(1.0, QColor(255, 255, 255, 0));
    p.setBrush(shine);
    p.drawEllipse(QPointF(c.x() - R * 0.12, c.y() - R * 0.48), R * 0.56, R * 0.36);
}

// ------------------------------------------------------------ Ringed Planet

void paintPlanet(QPainter &p, const QPointF &c, qreal size, const StateInk &ink, bool dark)
{
    const qreal R = size / 2.0 - 0.9;
    const qreal tilt = -22.0;
    const qreal rx = R * 1.95;
    const qreal ry = R * 0.52;
    const qreal ringWidth = std::max(0.9, size * 0.1);
    const QColor ring = dark ? QColor(236, 226, 200) : QColor(196, 172, 120);
    const QColor ringEdge = dark ? QColor(150, 130, 96) : QColor(120, 96, 50);

    const auto drawRing = [&](bool front) {
        p.save();
        p.translate(c);
        p.rotate(tilt);
        // The front half of the ring passes below the planet's centre line.
        p.setClipRect(front ? QRectF(-rx - 2, 0, 2 * rx + 4, ry + 3)
                            : QRectF(-rx - 2, -ry - 3, 2 * rx + 4, ry + 3));
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(ringEdge, ringWidth + 0.7));
        p.drawEllipse(QPointF(0, 0), rx, ry);
        p.setPen(QPen(ring, ringWidth));
        p.drawEllipse(QPointF(0, 0), rx, ry);
        p.setPen(QPen(withAlpha(QColor(255, 255, 255), 0.6), ringWidth * 0.3));
        p.drawEllipse(QPointF(0, 0), rx - ringWidth * 0.2, ry - ringWidth * 0.12);
        p.restore();
    };

    drawRing(false);
    // The globe, banded like a gas giant.
    QPainterPath globe;
    globe.addEllipse(c, R, R);
    p.save();
    p.setClipPath(globe);
    p.fillRect(QRectF(c.x() - R, c.y() - R, 2 * R, 2 * R), ink.base);
    p.save();
    p.translate(c);
    p.rotate(tilt);
    const std::array<std::pair<qreal, qreal>, 4> bands = {
        {{-0.62, 0.22}, {-0.12, 0.18}, {0.3, 0.26}, {0.72, 0.16}}};
    int n = 0;
    for (const auto &[at, thickness] : bands) {
        p.fillRect(QRectF(-R * 1.5, R * at, R * 3, R * thickness),
                   n % 2 == 0 ? withAlpha(ink.dark, 0.55) : withAlpha(ink.light, 0.55));
        ++n;
    }
    p.restore();
    // Spherical shading on top of the bands.
    QRadialGradient shade(c + QPointF(-R * 0.4, -R * 0.45), R * 1.6);
    shade.setColorAt(0.0, QColor(255, 255, 255, 150));
    shade.setColorAt(0.35, QColor(255, 255, 255, 0));
    shade.setColorAt(0.75, QColor(0, 0, 0, 40));
    shade.setColorAt(1.0, QColor(0, 0, 0, 120));
    p.fillRect(QRectF(c.x() - R, c.y() - R, 2 * R, 2 * R), shade);
    p.restore();
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(ink.rim, std::max(0.5, size * 0.055)));
    p.drawEllipse(c, R, R);
    drawRing(true);
}

// ------------------------------------------------------------- Spirit Flame

QPainterPath flameShape(const QRectF &box)
{
    const auto at = [&box](qreal x, qreal y) {
        return QPointF(box.left() + box.width() * x, box.top() + box.height() * y);
    };
    // A tall main tongue leaning right, a smaller tongue on the left.
    QPainterPath path;
    path.moveTo(at(0.5, 1.0));
    path.cubicTo(at(0.2, 1.0), at(0.04, 0.8), at(0.08, 0.58));
    path.cubicTo(at(0.1, 0.46), at(0.16, 0.38), at(0.2, 0.27));
    path.cubicTo(at(0.28, 0.36), at(0.33, 0.43), at(0.4, 0.45));
    path.cubicTo(at(0.37, 0.28), at(0.46, 0.12), at(0.62, 0.0));
    path.cubicTo(at(0.64, 0.16), at(0.92, 0.32), at(0.92, 0.62));
    path.cubicTo(at(0.92, 0.85), at(0.75, 1.0), at(0.5, 1.0));
    path.closeSubpath();
    return path;
}

void paintFlame(QPainter &p, const QPointF &c, qreal size, const StateInk &ink, bool offline,
                qreal overhang)
{
    const qreal w = size * 0.92;
    const qreal h = size * 1.22;
    const QRectF box(c.x() - w / 2, c.y() + size * 0.5 - h + size * 0.02, w, h);
    const QPointF auraCentre(c.x(), box.bottom() - h * 0.38);
    const qreal auraR = size * 0.5 + overhang;
    QRadialGradient aura(auraCentre, auraR);
    aura.setColorAt(0.0, withAlpha(ink.base, offline ? 0.2 : 0.55));
    aura.setColorAt(1.0, withAlpha(ink.base, 0.0));
    p.setPen(Qt::NoPen);
    p.setBrush(aura);
    p.drawEllipse(auraCentre, auraR, auraR);

    QLinearGradient body(0, box.bottom(), 0, box.top());
    body.setColorAt(0.0, ink.dark);
    body.setColorAt(0.4, ink.base);
    body.setColorAt(1.0, ink.light);
    p.setPen(QPen(ink.rim, std::max(0.5, size * 0.055)));
    p.setBrush(body);
    p.drawPath(flameShape(box));
    // The hot heart: a smaller flame low in the body.
    const QRectF heartBox(c.x() - w * 0.24, box.bottom() - h * 0.56, w * 0.46, h * 0.52);
    QLinearGradient heart(0, heartBox.bottom(), 0, heartBox.top());
    heart.setColorAt(0.0, lighter(ink.light, 0.7));
    heart.setColorAt(1.0, withAlpha(lighter(ink.light, 0.35), 0.85));
    p.setPen(Qt::NoPen);
    p.setBrush(heart);
    p.drawPath(flameShape(heartBox));
}

QCache<QString, QImage> &beadCache()
{
    static QCache<QString, QImage> cache(4 * 1024);
    return cache;
}

} // namespace

BeadArt::BeadArt(QQuickItem *parent) : QQuickPaintedItem(parent)
{
    setAntialiasing(true);
}

void BeadArt::setStyleId(const QString &styleId)
{
    if (m_styleId == styleId)
        return;
    m_styleId = styleId;
    emit styleIdChanged();
    emit overhangChanged();
    update();
}

void BeadArt::setPresence(int presence)
{
    if (m_presence == presence)
        return;
    m_presence = presence;
    emit presenceChanged();
    update();
}

void BeadArt::setBeadSize(qreal size)
{
    size = std::max(4.0, size);
    if (qFuzzyCompare(m_beadSize, size))
        return;
    m_beadSize = size;
    emit beadSizeChanged();
    emit overhangChanged();
    update();
}

void BeadArt::setDarkMode(bool dark)
{
    if (m_darkMode == dark)
        return;
    m_darkMode = dark;
    emit darkModeChanged();
    update();
}

qreal BeadArt::overhangFor(const QString &styleId, qreal beadSize)
{
    if (styleId == QLatin1String("bead.orb"))
        return std::ceil(beadSize * 0.3);
    if (styleId == QLatin1String("bead.planet"))
        return std::ceil(beadSize * 0.35);
    if (styleId == QLatin1String("bead.flame"))
        return std::ceil(beadSize * 0.25);
    if (styleId.isEmpty())
        return 0.0;
    return std::ceil(beadSize * 0.12);
}

QImage BeadArt::render(const QString &styleId, int presence, qreal beadSize, bool dark, qreal dpr)
{
    const QString key = QStringLiteral("%1|%2|%3|%4|%5")
                            .arg(styleId)
                            .arg(presence)
                            .arg(beadSize, 0, 'f', 2)
                            .arg(dark ? 1 : 0)
                            .arg(dpr, 0, 'f', 3);
    if (const QImage *cached = beadCache().object(key))
        return *cached;
    const qreal overhang = overhangFor(styleId, beadSize);
    const qreal side = beadSize + 2 * overhang;
    QImage image = transparentImage(QSizeF(side, side), dpr);
    {
        QPainter p(&image);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QPointF centre(side / 2, side / 2);
        const StateInk ink = inkFor(presence);
        const bool offline = presence == 2;
        if (styleId == QLatin1String("bead.gem"))
            paintGem(p, centre, beadSize, ink, offline);
        else if (styleId == QLatin1String("bead.heart"))
            paintHeart(p, centre, beadSize, ink);
        else if (styleId == QLatin1String("bead.star"))
            paintStar(p, centre, beadSize, ink);
        else if (styleId == QLatin1String("bead.orb"))
            paintOrb(p, centre, beadSize, ink, offline, overhang);
        else if (styleId == QLatin1String("bead.planet"))
            paintPlanet(p, centre, beadSize, ink, dark);
        else if (styleId == QLatin1String("bead.flame"))
            paintFlame(p, centre, beadSize, ink, offline, overhang);
    }
    beadCache().insert(key, new QImage(image), std::max<qsizetype>(1, image.sizeInBytes() / 1024));
    return image;
}

void BeadArt::paint(QPainter *painter)
{
    if (m_styleId.isEmpty())
        return;
    const QImage image = render(m_styleId, m_presence, m_beadSize, m_darkMode, deviceScale(*painter));
    // Centred, in case the item was laid out larger than the bead needs.
    const qreal side = m_beadSize + 2 * overhang();
    painter->drawImage(QPointF((width() - side) / 2, (height() - side) / 2), image);
}

} // namespace OpenChat
