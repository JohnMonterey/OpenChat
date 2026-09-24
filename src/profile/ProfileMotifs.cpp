#include "profile/ProfileMotifs.h"

#include "cosmetics/CosmeticPaint.h"
#include "profile/ProfileReadability.h"

#include <QCache>
#include <QLinearGradient>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace OpenChat::ProfileMotifs {

namespace {

using Profile::Motif;
namespace Readability = ProfileReadability;

constexpr double pi = 3.14159265358979323846;
// Tiles smaller than this (logical px) are repeated into a bigger block first,
// so a viewport is covered by a few hundred blits rather than thousands.
constexpr qreal minBlockSide = 96.0;
constexpr int tileCacheKb = 4 * 1024;

QColor withAlpha(QColor colour, qreal alpha)
{
    colour.setAlphaF(float(std::clamp(alpha, 0.0, 1.0)));
    return colour;
}

qreal degrees(qreal radians)
{
    return radians * 180.0 / pi;
}

// ------------------------------------------------------------ tile painting
//
// Inside a tile a shape REPLACES what is under it (Source) and a hole clears
// it, so ink-over-ink never darkens and a hole shows the page's base.

void fillInk(QPainter &p, const QPainterPath &path, const QColor &ink)
{
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.fillPath(path, ink);
}

void strokeInk(QPainter &p, const QPainterPath &path, const QColor &ink, qreal width,
               Qt::PenCapStyle cap = Qt::FlatCap, Qt::PenJoinStyle join = Qt::MiterJoin)
{
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.strokePath(path, QPen(ink, width, Qt::SolidLine, cap, join));
}

void cutHole(QPainter &p, const QPainterPath &path)
{
    p.setCompositionMode(QPainter::CompositionMode_Clear);
    p.fillPath(path, Qt::black);
}

void cutStroke(QPainter &p, const QPainterPath &path, qreal width, Qt::PenCapStyle cap, Qt::PenJoinStyle join)
{
    p.setCompositionMode(QPainter::CompositionMode_Clear);
    p.strokePath(path, QPen(Qt::black, width, Qt::SolidLine, cap, join));
}

QPainterPath circlePath(const QPointF &centre, qreal radius)
{
    QPainterPath path;
    path.addEllipse(centre, radius, radius);
    return path;
}

QPainterPath linePath(const QPointF &a, const QPointF &b)
{
    QPainterPath path;
    path.moveTo(a);
    path.lineTo(b);
    return path;
}

// The inks one tile motif paints with, alpha = the opacity each is drawn at.
struct Inks final {
    QColor main;
    QColor high;  // first declared highlight
    QColor high2; // second declared highlight
};

Inks inksFor(Motif motif, const QColor &ink, qreal opacity)
{
    Inks inks;
    inks.main = withAlpha(ink, opacity);
    const QVector<HighlightInk> highs = highlightInks(motif, ink, opacity);
    if (!highs.isEmpty())
        inks.high = withAlpha(highs.at(0).color, highs.at(0).opacity);
    if (highs.size() > 1)
        inks.high2 = withAlpha(highs.at(1).color, highs.at(1).opacity);
    return inks;
}

// A bone of the skull-and-crossbones: a round-capped bar with two knuckles
// at each end.
void bone(QPainter &p, const QPointF &a, const QPointF &b, qreal t, const QColor &ink)
{
    strokeInk(p, linePath(a, b), ink, t, Qt::RoundCap, Qt::RoundJoin);
    const qreal dx = b.x() - a.x();
    const qreal dy = b.y() - a.y();
    const qreal length = std::hypot(dx, dy);
    const QPointF normal(-dy / length * t * 0.62, dx / length * t * 0.62);
    for (const QPointF &end : {a, b}) {
        fillInk(p, circlePath(end + normal, t * 0.72), ink);
        fillInk(p, circlePath(end - normal, t * 0.72), ink);
    }
}

void skull(QPainter &p, qreal cx, qreal cy, qreal s, qreal rotation, const Inks &inks, bool bones, bool bow)
{
    p.save();
    p.translate(cx, cy);
    p.rotate(degrees(rotation));
    if (bones) {
        bone(p, QPointF(-0.62 * s, -0.42 * s), QPointF(0.62 * s, 0.62 * s), 0.12 * s, inks.main);
        bone(p, QPointF(0.62 * s, -0.42 * s), QPointF(-0.62 * s, 0.62 * s), 0.12 * s, inks.main);
    }
    // The dome: an arc from 0.78π round to 2.22π (clockwise on screen), then
    // the jaw.
    QPainterPath head;
    const QRectF dome(-0.4 * s, -0.1 * s - 0.4 * s, 0.8 * s, 0.8 * s);
    head.arcMoveTo(dome, -0.78 * 180.0);
    head.arcTo(dome, -0.78 * 180.0, -1.44 * 180.0);
    head.lineTo(0.22 * s, 0.2 * s);
    head.lineTo(0.22 * s, 0.36 * s);
    head.quadTo(QPointF(0.22 * s, 0.42 * s), QPointF(0.14 * s, 0.42 * s));
    head.lineTo(-0.14 * s, 0.42 * s);
    head.quadTo(QPointF(-0.22 * s, 0.42 * s), QPointF(-0.22 * s, 0.36 * s));
    head.lineTo(-0.22 * s, 0.2 * s);
    head.closeSubpath();
    fillInk(p, head, inks.main);

    QPainterPath holes;
    holes.addEllipse(QPointF(-0.15 * s, -0.04 * s), 0.11 * s, 0.12 * s);
    holes.addEllipse(QPointF(0.15 * s, -0.04 * s), 0.11 * s, 0.12 * s);
    QPainterPath nose;
    nose.moveTo(0, 0.1 * s);
    nose.lineTo(0.05 * s, 0.19 * s);
    nose.lineTo(-0.05 * s, 0.19 * s);
    nose.closeSubpath();
    holes.addPath(nose);
    cutHole(p, holes);
    QPainterPath teeth;
    for (int i = -1; i <= 1; ++i) {
        teeth.moveTo(i * 0.075 * s, 0.3 * s);
        teeth.lineTo(i * 0.075 * s, 0.42 * s);
    }
    cutStroke(p, teeth, std::max(1.0, 0.035 * s), Qt::FlatCap, Qt::MiterJoin);

    if (bow) {
        p.save();
        p.translate(0.2 * s, -0.4 * s);
        p.rotate(degrees(0.35));
        QPainterPath loops;
        loops.moveTo(0, 0);
        loops.quadTo(QPointF(-0.22 * s, -0.2 * s), QPointF(-0.24 * s, 0.02 * s));
        loops.quadTo(QPointF(-0.2 * s, 0.16 * s), QPointF(0, 0));
        loops.quadTo(QPointF(0.22 * s, -0.2 * s), QPointF(0.24 * s, 0.02 * s));
        loops.quadTo(QPointF(0.2 * s, 0.16 * s), QPointF(0, 0));
        loops.closeSubpath();
        loops.addEllipse(QPointF(0, 0), 0.055 * s, 0.055 * s);
        loops.setFillRule(Qt::WindingFill);
        fillInk(p, loops, inks.high2);
        p.restore();
    }
    p.restore();
}

void note(QPainter &p, qreal cx, qreal cy, qreal s, qreal rotation, const QColor &ink, bool beamed)
{
    p.save();
    p.translate(cx, cy);
    p.rotate(degrees(rotation));
    const qreal stem = std::max(1.2, s * 0.08);
    QPainterPath heads;
    heads.addEllipse(QPointF(0, 0), s * 0.26, s * 0.19);
    if (beamed)
        heads.addEllipse(QPointF(s * 0.7, s * 0.08), s * 0.26, s * 0.19);
    fillInk(p, heads, ink);
    QPainterPath stems = linePath(QPointF(s * 0.23, 0), QPointF(s * 0.23, -s * 0.95));
    if (beamed) {
        stems.addPath(linePath(QPointF(s * 0.93, s * 0.08), QPointF(s * 0.93, -s * 0.87)));
        strokeInk(p, stems, ink, stem);
        strokeInk(p, linePath(QPointF(s * 0.2, -s * 0.9), QPointF(s * 0.96, -s * 0.82)), ink, s * 0.2);
    } else {
        stems.moveTo(s * 0.23, -s * 0.95);
        stems.quadTo(QPointF(s * 0.62, -s * 0.62), QPointF(s * 0.5, -s * 0.28));
        strokeInk(p, stems, ink, stem);
    }
    p.restore();
}

void flower(QPainter &p, qreal cx, qreal cy, qreal r, qreal rotation, const QColor &ink, const QColor &centre)
{
    for (int i = 0; i < 5; ++i) {
        const qreal a = rotation + i * pi * 2.0 / 5.0;
        fillInk(p, circlePath(QPointF(cx + std::cos(a) * r * 0.55, cy + std::sin(a) * r * 0.55), r * 0.45), ink);
    }
    fillInk(p, circlePath(QPointF(cx, cy), r * 0.3), centre);
}

// One cell of a scatter motif, drawn whole at (x, y); T = the cell pitch.
using CellPainter = std::function<void(QPainter &, qreal x, qreal y, qreal T)>;

void starsCell(QPainter &p, qreal x, qreal y, qreal T, const Inks &k)
{
    fillInk(p, fivePointStar(QPointF(x + 0.22 * T, y + 0.25 * T), 0.13 * T, 0.2), k.main);
    fillInk(p, fivePointStar(QPointF(x + 0.7 * T, y + 0.62 * T), 0.09 * T, -0.3), k.main);
    const qreal width = std::max(1.5, 0.02 * T);
    strokeInk(p, fivePointStar(QPointF(x + 0.78 * T, y + 0.16 * T), 0.07 * T, 0.5), k.main, width, Qt::FlatCap,
              Qt::RoundJoin);
    strokeInk(p, fivePointStar(QPointF(x + 0.3 * T, y + 0.8 * T), 0.06 * T, 0.1), k.main, width, Qt::FlatCap,
              Qt::RoundJoin);
}

void heartsCell(QPainter &p, qreal x, qreal y, qreal T, const Inks &k)
{
    fillInk(p, heartPath(QPointF(x + 0.25 * T, y + 0.3 * T), 0.26 * T, -0.25), k.main);
    fillInk(p, heartPath(QPointF(x + 0.72 * T, y + 0.7 * T), 0.18 * T, 0.3), k.main);
    strokeInk(p, heartPath(QPointF(x + 0.78 * T, y + 0.2 * T), 0.12 * T, 0.1), k.main, std::max(1.5, 0.018 * T));
}

void skullsCell(QPainter &p, qreal x, qreal y, qreal T, const Inks &k)
{
    skull(p, x + 0.26 * T, y + 0.3 * T, 0.3 * T, -0.18, k, true, false);
    skull(p, x + 0.76 * T, y + 0.76 * T, 0.24 * T, 0.22, k, false, true);
    fillInk(p, fivePointStar(QPointF(x + 0.8 * T, y + 0.2 * T), 0.1 * T, 0.3), k.high);
    fillInk(p, fivePointStar(QPointF(x + 0.56 * T, y + 0.94 * T), 0.05 * T, -0.2), k.high);
    strokeInk(p, fivePointStar(QPointF(x + 0.18 * T, y + 0.8 * T), 0.1 * T, 0.12), k.main,
              std::max(1.5, 0.018 * T), Qt::FlatCap, Qt::RoundJoin);
    fillInk(p, heartPath(QPointF(x + 0.52 * T, y + 0.48 * T), 0.12 * T, -0.35), k.main);
    fillInk(p, heartPath(QPointF(x + 0.02 * T, y + 0.56 * T), 0.07 * T, 0.3), k.main);
    fillInk(p, sparklePath(QPointF(x + 0.97 * T, y + 0.46 * T), 0.05 * T), k.high);
    fillInk(p, sparklePath(QPointF(x + 0.42 * T, y + 0.1 * T), 0.035 * T), k.high);
}

void sparklesCell(QPainter &p, qreal x, qreal y, qreal T, const Inks &k)
{
    fillInk(p, sparklePath(QPointF(x + 0.22 * T, y + 0.26 * T), 0.14 * T), k.main);
    fillInk(p, sparklePath(QPointF(x + 0.74 * T, y + 0.7 * T), 0.1 * T), k.main);
    fillInk(p, heartPath(QPointF(x + 0.7 * T, y + 0.2 * T), 0.13 * T, 0.25), k.main);
    fillInk(p, heartPath(QPointF(x + 0.2 * T, y + 0.78 * T), 0.09 * T, -0.3), k.main);
    fillInk(p, sparklePath(QPointF(x + 0.46 * T, y + 0.5 * T), 0.06 * T), k.high);
    fillInk(p, sparklePath(QPointF(x + 0.95 * T, y + 0.45 * T), 0.045 * T), k.high);
    fillInk(p, circlePath(QPointF(x + 0.4 * T, y + 0.12 * T), 0.018 * T), k.high);
    fillInk(p, circlePath(QPointF(x + 0.9 * T, y + 0.92 * T), 0.02 * T), k.high);
    fillInk(p, circlePath(QPointF(x + 0.55 * T, y + 0.86 * T), 0.014 * T), k.high);
}

void notesCell(QPainter &p, qreal x, qreal y, qreal T, const Inks &k)
{
    note(p, x + 0.2 * T, y + 0.4 * T, 0.26 * T, -0.15, k.main, false);
    note(p, x + 0.6 * T, y + 0.85 * T, 0.2 * T, 0.12, k.main, true);
    fillInk(p, fivePointStar(QPointF(x + 0.78 * T, y + 0.22 * T), 0.05 * T, 0.2), k.main);
}

void flowersCell(QPainter &p, qreal x, qreal y, qreal T, const Inks &k)
{
    flower(p, x + 0.25 * T, y + 0.28 * T, 0.18 * T, 0.2, k.main, k.high);
    flower(p, x + 0.74 * T, y + 0.72 * T, 0.13 * T, 0.7, k.main, k.high);
    fillInk(p, circlePath(QPointF(x + 0.75 * T, y + 0.2 * T), 0.03 * T), k.main);
}

void brokenHeartsCell(QPainter &p, qreal x, qreal y, qreal T, const Inks &k)
{
    struct Item final { qreal x, y, size, rotation; };
    static constexpr Item items[] = {{0.26, 0.3, 0.3, -0.2}, {0.76, 0.74, 0.22, 0.25}, {0.8, 0.18, 0.12, 0.4}};
    for (const Item &item : items) {
        const qreal cx = x + item.x * T;
        const qreal cy = y + item.y * T;
        const qreal size = item.size * T;
        fillInk(p, heartPath(QPointF(cx, cy), size, item.rotation), k.main);
        p.save();
        p.translate(cx, cy);
        p.rotate(degrees(item.rotation));
        QPainterPath crack;
        crack.moveTo(0, -0.24 * size);
        crack.lineTo(-0.08 * size, -0.06 * size);
        crack.lineTo(0.07 * size, 0.06 * size);
        crack.lineTo(-0.05 * size, 0.2 * size);
        crack.lineTo(0, 0.4 * size);
        cutStroke(p, crack, std::max(1.5, size * 0.07), Qt::FlatCap, Qt::MiterJoin);
        p.restore();
    }
}

QImage blankTile(int widthPx, int heightPx, qreal dpr)
{
    QImage image(std::max(1, widthPx), std::max(1, heightPx), QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::transparent);
    return image;
}

int devicePixels(qreal logical, qreal dpr)
{
    return std::max(1, int(std::lround(logical * dpr)));
}

// A scatter motif's lattice: cells T apart, every other row shifted by T/2
// (the mockups' tiled()). Its repeat is T × 2T; every cell that reaches into
// the tile is drawn whole, in row-major order, so shapes that cross a tile
// edge line up with their copies and overlap the same way on both sides.
QImage scatterTile(qreal pitch, qreal dpr, const CellPainter &cell,
                   const std::function<void(QPainter &, qreal T)> &underlay = {})
{
    const int pitchPx = devicePixels(pitch, dpr);
    const qreal T = pitchPx / dpr;
    QImage image = blankTile(pitchPx, 2 * pitchPx, dpr);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing, true);
    if (underlay)
        underlay(p, T);
    for (int j = -2; j <= 3; ++j) {
        for (int i = -2; i <= 2; ++i)
            cell(p, i * T + (j & 1) * T * 0.5, j * T, T);
    }
    p.end();
    return image;
}

QImage checkerTile(qreal square, qreal dpr, const Inks &k)
{
    const int squarePx = devicePixels(square, dpr);
    const qreal q = squarePx / dpr;
    QImage image = blankTile(2 * squarePx, 2 * squarePx, dpr);
    QPainter p(&image);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.fillRect(QRectF(0, 0, q, q), k.main);
    p.fillRect(QRectF(q, q, q, q), k.main);
    return image;
}

QImage polkaTile(qreal pitch, qreal dpr, const Inks &k)
{
    const int pitchPx = devicePixels(pitch, dpr);
    const qreal P = pitchPx / dpr;
    QImage image = blankTile(pitchPx, 2 * pitchPx, dpr);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing, true);
    for (int row = -1; row <= 2; ++row) {
        for (int col = -1; col <= 1; ++col)
            fillInk(p, circlePath(QPointF(col * P + (row & 1) * P * 0.5, row * P), P * 0.22), k.main);
    }
    return image;
}

QImage pinstripeTile(qreal pitch, qreal lineWidth, qreal dpr, const Inks &k)
{
    const int pitchPx = devicePixels(pitch, dpr);
    const qreal P = pitchPx / dpr;
    QImage image = blankTile(pitchPx, pitchPx, dpr);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing, true);
    const qreal x = std::fmod(6.0, P);
    strokeInk(p, linePath(QPointF(x, -1), QPointF(x, P + 1)), k.main, lineWidth);
    return image;
}

QImage plaidTile(qreal pitch, qreal dpr, const QColor &ink, qreal opacity)
{
    const int pitchPx = devicePixels(pitch, dpr);
    const qreal P = pitchPx / dpr;
    QImage image = blankTile(pitchPx, pitchPx, dpr);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing, true);
    // Plaid is woven: its bands and threads are translucent and cross over
    // each other (SourceOver), as in the mockup.
    const QColor band = withAlpha(ink, opacity * 0.45);
    p.fillRect(QRectF(0, 0, P * 0.34, P), band);
    p.fillRect(QRectF(0, 0, P, P * 0.34), band);
    const QPen thread(withAlpha(ink, std::min(1.0, opacity * 0.9)), 1.5);
    p.setPen(thread);
    p.drawLine(QPointF(P * 0.62, -1), QPointF(P * 0.62, P + 1));
    p.drawLine(QPointF(-1, P * 0.62), QPointF(P + 1, P * 0.62));
    return image;
}

QImage leopardTile(qreal cell, qreal dpr, const Inks &k)
{
    // Four rosettes across and five rows down (rows 0.8 cells apart), so the
    // row offsets (0.8 · row mod 2 · 0.4 cells) come back round at the edge.
    constexpr int columns = 4;
    constexpr int rows = 5;
    const int sidePx = devicePixels(cell * columns, dpr);
    const qreal side = sidePx / dpr;
    const qreal L = side / columns;
    struct Rosette final {
        QPointF centre;
        qreal radius = 0;
        std::vector<qreal> spots;
    };
    std::vector<Rosette> rosettes;
    Random random(3);
    for (int row = 0; row < rows; ++row) {
        const qreal y = row * L * 0.8;
        const qreal offset = std::fmod(0.8 * row, 2.0) * L * 0.4;
        for (int column = 0; column < columns; ++column) {
            Rosette rosette;
            const qreal cx = column * L + random.next() * L * 0.6 + offset;
            const qreal cy = y + random.next() * L * 0.4;
            rosette.centre = QPointF(cx, cy);
            const int spots = 4 + int(std::floor(random.next() * 3));
            rosette.radius = L * (0.16 + random.next() * 0.08);
            for (int m = 0; m < spots; ++m)
                rosette.spots.push_back(double(m) / spots * 6.28 + random.next() * 0.6);
            rosettes.push_back(std::move(rosette));
        }
    }
    QImage image = blankTile(sidePx, sidePx, dpr);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing, true);
    for (const Rosette &rosette : rosettes) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const QPointF c = rosette.centre + QPointF(dx * side, dy * side);
                const qreal r = rosette.radius;
                QPainterPath body;
                body.addEllipse(c, r * 0.8, r * 0.65);
                fillInk(p, body, k.main);
                QPainterPath spots;
                for (const qreal a : rosette.spots)
                    spots.addEllipse(c + QPointF(std::cos(a) * r, std::sin(a) * r * 0.8), r * 0.32, r * 0.22);
                spots.setFillRule(Qt::WindingFill);
                fillInk(p, spots, k.high);
            }
        }
    }
    return image;
}

QImage linenTile(qreal pitch3, qreal dpr, const QColor &ink, qreal opacity)
{
    // 64 hairline threads each way, 3 px apart at M, each with its own
    // jittered strength; threads cross over each other (SourceOver), as in
    // the mockup. (The mockup's weave ignores the Size knob; here it scales
    // like every other motif, so the editor's S / M / L is never a no-op.)
    constexpr int threads = 64;
    const int sidePx = devicePixels(threads * pitch3, dpr);
    const qreal side = sidePx / dpr;
    const qreal pitch = side / threads;
    QImage image = blankTile(sidePx, sidePx, dpr);
    QPainter p(&image);
    Random random(11);
    for (int i = 0; i < threads; ++i)
        p.fillRect(QRectF(0, i * pitch, side, 1.0), withAlpha(ink, opacity * (0.25 + random.next() * 0.6)));
    for (int i = 0; i < threads; ++i)
        p.fillRect(QRectF(i * pitch, 0, 1.0, side), withAlpha(ink, opacity * (0.15 + random.next() * 0.45)));
    return image;
}

// Repeats a small tile into a block of at least minBlockSide a side.
QImage expandedTile(const QImage &tile)
{
    const qreal dpr = tile.devicePixelRatio();
    const QSizeF logical(tile.width() / dpr, tile.height() / dpr);
    const int across = std::max(1, int(std::ceil(minBlockSide / logical.width())));
    const int down = std::max(1, int(std::ceil(minBlockSide / logical.height())));
    if (across == 1 && down == 1)
        return tile;
    QImage block = blankTile(tile.width() * across, tile.height() * down, dpr);
    QPainter p(&block);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    for (int y = 0; y < down; ++y) {
        for (int x = 0; x < across; ++x)
            p.drawImage(QPointF(x * tile.width() / dpr, y * tile.height() / dpr), tile);
    }
    return block;
}

QImage renderTile(Motif motif, const QColor &ink, qreal opacity, qreal s, qreal dpr)
{
    const Inks k = inksFor(motif, ink, opacity);
    switch (motif) {
    case Motif::Stars:
        return scatterTile(130 * s, dpr, [&k](QPainter &p, qreal x, qreal y, qreal T) { starsCell(p, x, y, T, k); });
    case Motif::Hearts:
        return scatterTile(120 * s, dpr, [&k](QPainter &p, qreal x, qreal y, qreal T) { heartsCell(p, x, y, T, k); });
    case Motif::Skulls:
        return scatterTile(168 * s, dpr, [&k](QPainter &p, qreal x, qreal y, qreal T) { skullsCell(p, x, y, T, k); });
    case Motif::Sparkles:
        return scatterTile(110 * s, dpr,
                           [&k](QPainter &p, qreal x, qreal y, qreal T) { sparklesCell(p, x, y, T, k); });
    case Motif::MusicNotes:
        return scatterTile(120 * s, dpr, [&k](QPainter &p, qreal x, qreal y, qreal T) { notesCell(p, x, y, T, k); });
    case Motif::Flowers:
        return scatterTile(110 * s, dpr,
                           [&k](QPainter &p, qreal x, qreal y, qreal T) { flowersCell(p, x, y, T, k); });
    case Motif::BrokenHearts: {
        // Faint 1 px pinstripes under the hearts, eleven to a cell so they
        // repeat with it (the mockup's 14 px, give or take a fraction).
        const QColor stripe = withAlpha(ink, opacity * 0.35);
        const auto stripes = [stripe](QPainter &p, qreal T) {
            p.save();
            p.setCompositionMode(QPainter::CompositionMode_Source);
            const qreal pitch = T / 11.0;
            for (int i = 0; i < 11; ++i)
                p.fillRect(QRectF(std::floor(i * pitch + pitch * 0.5), 0, 1.0, 2 * T), stripe);
            p.restore();
        };
        return scatterTile(
            150 * s, dpr, [&k](QPainter &p, qreal x, qreal y, qreal T) { brokenHeartsCell(p, x, y, T, k); }, stripes);
    }
    case Motif::Checkerboard:
        return checkerTile(30 * s, dpr, k);
    case Motif::PolkaDots:
        return polkaTile(34 * s, dpr, k);
    case Motif::Pinstripes:
        return pinstripeTile(16 * s, std::max(1.0, 1.4 * s), dpr, k);
    case Motif::Plaid:
        return plaidTile(90 * s, dpr, ink, opacity);
    case Motif::Leopard:
        return leopardTile(64 * s, dpr, k);
    case Motif::LinenWeave:
        return linenTile(3 * s, dpr, ink, opacity);
    case Motif::Zebra:
    case Motif::Halftone:
    case Motif::CyberGrid:
    case Motif::Bubbles:
        break;
    }
    return {};
}

struct TileCache final {
    QMutex mutex;
    QCache<QString, QImage> images{tileCacheKb};
};

TileCache &tileCache()
{
    static TileCache cache;
    return cache;
}

// ------------------------------------------------------------ viewport motifs

QPainterPath ribbonPath(qreal w, qreal y0)
{
    QPainterPath path;
    path.moveTo(-100, y0 + 120);
    path.cubicTo(QPointF(w * 0.3, y0 - 120), QPointF(w * 0.6, y0 + 160), QPointF(w + 120, y0 - 60));
    return path;
}

// Three soft white ribbons: the Aero aurora (bubbles and the stub's backdrop).
void paintRibbons(QPainter &p, qreal w, qreal h, bool dark, Random &random, qreal lightBase)
{
    for (int i = 0; i < 3; ++i) {
        const qreal y0 = h * (0.15 + i * 0.3) + random.next() * 60;
        const qreal alpha = dark ? 0.025 + 0.012 * i : lightBase + 0.05 * i;
        const qreal width = 70 + random.next() * 80;
        p.strokePath(ribbonPath(w, y0), QPen(withAlpha(Qt::white, alpha), width, Qt::SolidLine, Qt::RoundCap));
    }
}

void paintBubbles(QPainter &p, qreal w, qreal h, const BackdropSpec &spec, qreal s)
{
    const bool dark = spec.darkBase || Readability::isDark(spec.color1);
    Random random(5);
    paintRibbons(p, w, h, dark, random, 0.10);
    const int count = int(std::lround(w * h / 22000.0 * (1.0 / (s * s))));
    const QColor rim = dark ? withAlpha(Readability::lighten(spec.ink, 0.3), 0.28) : withAlpha(Qt::white, 0.35);
    const QColor glint = dark ? withAlpha(Readability::lighten(spec.ink, 0.5), 0.3) : withAlpha(Qt::white, 0.55);
    for (int b = 0; b < count; ++b) {
        const qreal bx = random.next() * w;
        const qreal by = random.next() * h;
        const qreal br = (8 + std::pow(random.next(), 2.2) * 70) * s;
        const qreal alpha = spec.opacity * (0.25 + random.next() * 0.45);
        p.setPen(Qt::NoPen);
        p.setBrush(withAlpha(spec.ink, alpha));
        p.drawEllipse(QPointF(bx, by), br, br);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(rim, 1));
        p.drawEllipse(QPointF(bx, by), br, br);
        // A glint arc on the upper left, 1.1π to 1.45π clockwise on screen.
        QPainterPath arc;
        const QRectF ring(bx - br * 0.72, by - br * 0.72, br * 1.44, br * 1.44);
        arc.arcMoveTo(ring, -1.1 * 180.0);
        arc.arcTo(ring, -1.1 * 180.0, -0.35 * 180.0);
        p.strokePath(arc, QPen(glint, std::max(1.0, br * 0.08)));
    }
}

void paintAurora(QPainter &p, qreal w, qreal h, const BackdropSpec &spec)
{
    const bool dark = spec.darkBase || Readability::isDark(spec.color1);
    Random random(5);
    paintRibbons(p, w, h, dark, random, 0.12);
}

void paintCyberGrid(QPainter &p, qreal w, qreal h, const BackdropSpec &spec, qreal s)
{
    const qreal horizon = std::round(h * 0.6);
    const QColor glow(0xFF, 0x2F, 0xD0);
    QLinearGradient band(0, horizon - 90, 0, horizon + 30);
    band.setColorAt(0, withAlpha(glow, 0));
    band.setColorAt(0.75, withAlpha(glow, 0.22));
    band.setColorAt(1, withAlpha(glow, 0));
    p.fillRect(QRectF(0, horizon - 90, w, 120), band);

    Random random(21);
    const double starCount = w * horizon / 2600.0;
    for (int i = 0; i < starCount; ++i) {
        const qreal sx = random.next() * w;
        const qreal sy = random.next() * (horizon - 20);
        const qreal big = random.next();
        const QColor star = withAlpha(Qt::white, 0.25 + random.next() * 0.6);
        if (big > 0.93) {
            p.fillPath(sparklePath(QPointF(sx, sy), 4 + random.next() * 5), star);
        } else {
            const qreal side = big > 0.6 ? 2 : 1;
            p.fillRect(QRectF(sx, sy, side, side), star);
        }
    }

    const qreal vx = w / 2;
    const qreal spacing = 70 * s;
    p.setPen(QPen(withAlpha(spec.ink, spec.opacity), 1));
    for (int i = -40; i <= 40; ++i)
        p.drawLine(QPointF(vx + i * spacing * 0.12, horizon), QPointF(vx + i * spacing * 1.6, h + 40));
    for (int k = 1; k <= 14; ++k) {
        const qreal t = k / 14.0;
        const qreal y = horizon + (h - horizon) * t * t;
        p.setPen(QPen(withAlpha(spec.ink, spec.opacity * (0.4 + 0.6 * t)), 1));
        p.drawLine(QPointF(0, y), QPointF(w, y));
    }
    p.setPen(QPen(withAlpha(Readability::lighten(spec.ink, 0.5), std::min(1.0, spec.opacity * 2)), 1.5));
    p.drawLine(QPointF(0, horizon), QPointF(w, horizon));
}

void paintHalftone(QPainter &p, qreal w, qreal h, const BackdropSpec &spec, qreal s)
{
    // The dot size follows a low-frequency field across the whole viewport,
    // which is why this motif is not a tile.
    const qreal G = 13 * s;
    QPainterPath dots;
    int row = 0;
    for (qreal y = 0; y < h + G; y += G, ++row) {
        for (qreal x = (row & 1) * G * 0.5; x < w + G; x += G) {
            const qreal f = 0.5 + 0.5 * std::sin(x / 190 + y / 260) * std::cos(y / 170 - x / 420);
            const qreal radius = G * 0.46 * (0.18 + 0.82 * f);
            if (radius > 0.6)
                dots.addEllipse(QPointF(x, y), radius, radius);
        }
    }
    p.fillPath(dots, withAlpha(spec.ink, spec.opacity));
}

void paintZebra(QPainter &p, qreal w, qreal h, const BackdropSpec &spec, qreal s)
{
    // Procedural bands, each with its own phases, tilted 0.5 across the whole
    // viewport: not a repeating tile. The bands are one path filled once, so
    // where two touch they do not darken each other.
    const qreal Z = 34 * s;
    const qreal tilt = 0.5;
    Random random(7);
    QPainterPath bands;
    bands.setFillRule(Qt::WindingFill);
    const auto addBand = [&bands](const std::vector<QPointF> &top, const std::vector<QPointF> &bottom) {
        QPainterPath band;
        band.moveTo(top.front());
        for (std::size_t i = 1; i < top.size(); ++i)
            band.lineTo(top[i]);
        for (auto it = bottom.rbegin(); it != bottom.rend(); ++it)
            band.lineTo(*it);
        band.closeSubpath();
        bands.addPath(band);
    };
    // The mockup starts 0.9 viewport heights up; a window much wider than
    // tall needs the tilted bands to start higher still to reach its corner.
    const qreal start = std::min(-h * 0.9, -(w + 40) * tilt - Z);
    for (qreal zy = start; zy < h * 1.3; zy += Z) {
        const qreal ph = random.next() * 6.28;
        const qreal ph2 = random.next() * 6.28;
        const qreal ph3 = random.next() * 6.28;
        const qreal amp = (12 + random.next() * 12) * s;
        std::vector<QPointF> top;
        std::vector<QPointF> bottom;
        for (qreal zx = -20; zx <= w + 20; zx += 5) {
            const qreal yc = zy + zx * tilt + std::sin(zx / (58 * s) + ph) * amp
                             + std::sin(zx / (23 * s) + ph3) * amp * 0.25;
            const qreal thickness = Z * 0.55 * std::pow(std::abs(std::sin(zx / (80 * s) + ph2)), 0.7)
                                    * (0.75 + 0.25 * std::sin(zx / (17 * s) + ph3));
            if (thickness > 0.8) {
                top.emplace_back(zx, yc - thickness / 2);
                bottom.emplace_back(zx, yc + thickness / 2);
            } else {
                if (top.size() > 1)
                    addBand(top, bottom);
                top.clear();
                bottom.clear();
            }
        }
        if (top.size() > 1)
            addBand(top, bottom);
    }
    p.fillPath(bands, withAlpha(spec.ink, spec.opacity));
}

} // namespace

double Random::next()
{
    m_state += 0x6D2B79F5U;
    quint32 t = m_state;
    t = (t ^ (t >> 15)) * (t | 1U);
    t ^= t + (t ^ (t >> 7)) * (t | 61U);
    return double(t ^ (t >> 14)) / 4294967296.0;
}

qreal scaleFactor(Profile::MotifScale scale)
{
    switch (scale) {
    case Profile::MotifScale::SmallMotif:
        return 0.72;
    case Profile::MotifScale::MediumMotif:
        break;
    case Profile::MotifScale::LargeMotif:
        return 1.4;
    }
    return 1.0;
}

bool isTileMotif(Motif motif)
{
    switch (motif) {
    case Motif::Zebra:
    case Motif::Halftone:
    case Motif::CyberGrid:
    case Motif::Bubbles:
        return false;
    default:
        return true;
    }
}

QSizeF tilePeriod(Motif motif, Profile::MotifScale scale)
{
    const qreal s = scaleFactor(scale);
    switch (motif) {
    case Motif::Stars:
        return {130 * s, 260 * s};
    case Motif::Hearts:
    case Motif::MusicNotes:
        return {120 * s, 240 * s};
    case Motif::Skulls:
        return {168 * s, 336 * s};
    case Motif::Sparkles:
    case Motif::Flowers:
        return {110 * s, 220 * s};
    case Motif::BrokenHearts:
        return {150 * s, 300 * s};
    case Motif::Checkerboard:
        return {60 * s, 60 * s};
    case Motif::PolkaDots:
        return {34 * s, 68 * s};
    case Motif::Pinstripes:
        return {16 * s, 16 * s};
    case Motif::Plaid:
        return {90 * s, 90 * s};
    case Motif::Leopard:
        return {256 * s, 256 * s};
    case Motif::LinenWeave:
        return {192 * s, 192 * s};
    case Motif::Zebra:
    case Motif::Halftone:
    case Motif::CyberGrid:
    case Motif::Bubbles:
        break;
    }
    return {};
}

QVector<HighlightInk> highlightInks(Motif motif, const QColor &ink, qreal opacity)
{
    switch (motif) {
    case Motif::Skulls:
        return {{Readability::lighten(ink, 0.25), std::min(1.0, opacity + 0.2)},
                {Readability::lighten(ink, 0.55), std::min(1.0, opacity + 0.25)}};
    case Motif::Sparkles:
        return {{QColor(Qt::white), 0.85}};
    case Motif::Flowers:
        return {{Readability::lighten(ink, 0.7), std::min(1.0, opacity + 0.2)}};
    case Motif::Leopard:
        // The rosettes' spots: the ink itself, a quarter stronger.
        return {{QColor(ink.rgb()), std::min(1.0, opacity + 0.25)}};
    default:
        return {};
    }
}

QImage motifTile(Motif motif, const QColor &ink, qreal opacity, Profile::MotifScale scale, qreal dpr)
{
    if (!isTileMotif(motif))
        return {};
    dpr = std::clamp(dpr, 0.25, 8.0);
    opacity = std::clamp(opacity, 0.0, 1.0);
    const QString key = QStringLiteral("%1|%2|%3|%4|%5")
                            .arg(int(motif))
                            .arg(ink.rgb(), 8, 16, QLatin1Char('0'))
                            .arg(opacity, 0, 'f', 4)
                            .arg(int(scale))
                            .arg(dpr, 0, 'f', 4);
    TileCache &cache = tileCache();
    {
        QMutexLocker locker(&cache.mutex);
        if (const QImage *cached = cache.images.object(key))
            return *cached;
    }
    const QImage tile = expandedTile(renderTile(motif, QColor(ink.rgb()), opacity, scaleFactor(scale), dpr));
    const int cost = std::max<qsizetype>(1, tile.sizeInBytes() / 1024);
    QMutexLocker locker(&cache.mutex);
    cache.images.insert(key, new QImage(tile), cost);
    return tile;
}

void clearTileCache()
{
    TileCache &cache = tileCache();
    QMutexLocker locker(&cache.mutex);
    cache.images.clear();
}

int cachedTileCount()
{
    TileCache &cache = tileCache();
    QMutexLocker locker(&cache.mutex);
    return int(cache.images.count());
}

void paintMotif(QPainter &painter, const QRectF &rect, const BackdropSpec &spec)
{
    if (rect.isEmpty())
        return;
    const qreal s = scaleFactor(spec.scale);
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter.setClipRect(rect, Qt::IntersectClip);
    painter.translate(rect.topLeft());
    const qreal w = rect.width();
    const qreal h = rect.height();
    const QColor ink(spec.ink.rgb());
    if (isTileMotif(spec.motif)) {
        const QImage tile = motifTile(spec.motif, ink, spec.opacity, spec.scale, CosmeticPaint::deviceScale(painter));
        if (!tile.isNull()) {
            const qreal tw = tile.width() / tile.devicePixelRatio();
            const qreal th = tile.height() / tile.devicePixelRatio();
            for (qreal y = 0; y < h; y += th) {
                for (qreal x = 0; x < w; x += tw)
                    painter.drawImage(QPointF(x, y), tile);
            }
        }
    } else {
        BackdropSpec opaque = spec;
        opaque.ink = ink;
        switch (spec.motif) {
        case Motif::CyberGrid:
            paintCyberGrid(painter, w, h, opaque, s);
            break;
        case Motif::Bubbles:
            paintBubbles(painter, w, h, opaque, s);
            break;
        case Motif::Halftone:
            paintHalftone(painter, w, h, opaque, s);
            break;
        case Motif::Zebra:
            paintZebra(painter, w, h, opaque, s);
            break;
        default:
            break;
        }
    }
    painter.restore();
}

void paintBackdrop(QPainter &painter, const QRectF &rect, const BackdropSpec &spec, qreal zoom)
{
    if (rect.isEmpty())
        return;
    zoom = zoom > 0 ? zoom : 1.0;
    painter.save();
    painter.setClipRect(rect, Qt::IntersectClip);
    painter.translate(rect.topLeft());
    painter.scale(zoom, zoom);
    const QRectF area(0, 0, rect.width() / zoom, rect.height() / zoom);
    const QColor c1(spec.color1.rgb());
    const QColor c2(spec.color2.rgb());
    // A picture's base is its first colour alone (Fit and Center show it
    // around the picture); a pattern or gradient runs top to bottom.
    const bool gradient = (spec.kind == Profile::BackgroundKind::GradientBackground
                           || spec.kind == Profile::BackgroundKind::PatternBackground || spec.aurora)
                          && c1 != c2;
    if (gradient) {
        QLinearGradient base(0, 0, 0, area.height());
        base.setColorAt(0, c1);
        base.setColorAt(1, c2);
        painter.fillRect(area, base);
    } else {
        painter.fillRect(area, c1);
    }
    if (spec.kind == Profile::BackgroundKind::PatternBackground && !spec.aurora)
        paintMotif(painter, area, spec);
    if (spec.aurora) {
        painter.setRenderHint(QPainter::Antialiasing, true);
        paintAurora(painter, area.width(), area.height(), spec);
    }
    painter.restore();
}

QPainterPath heartPath(const QPointF &centre, qreal size, qreal rotation)
{
    const qreal k = size / 100.0;
    QPainterPath path;
    path.moveTo(0, 38 * k);
    path.cubicTo(QPointF(-62 * k, -2 * k), QPointF(-38 * k, -58 * k), QPointF(0, -22 * k));
    path.cubicTo(QPointF(38 * k, -58 * k), QPointF(62 * k, -2 * k), QPointF(0, 38 * k));
    path.closeSubpath();
    QTransform transform;
    transform.translate(centre.x(), centre.y());
    transform.rotateRadians(rotation);
    return transform.map(path);
}

QPainterPath sparklePath(const QPointF &centre, qreal radius)
{
    return CosmeticPaint::glintPath(centre, radius, 0.12);
}

QPainterPath fivePointStar(const QPointF &centre, qreal radius, qreal rotation)
{
    return CosmeticPaint::starPath(centre, radius, radius * 0.46, 5, degrees(rotation));
}

} // namespace OpenChat::ProfileMotifs
