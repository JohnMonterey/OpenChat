#pragma once

#include "domain/ProfilePage.h"

#include <QColor>
#include <QImage>
#include <QPainterPath>
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QVector>

class QPainter;

// The profile backdrop's painters (SPEC §8.1; the mockups' Motifs.js is the
// reference). A backdrop is the base colour or vertical gradient plus, for a
// pattern, one of 17 motifs; the stub page uses an internal "aurora".
//
// Most motifs repeat, so they are painted once into a transparent tile per
// (motif, ink, opacity, scale, device scale) and the tile is repeated over the
// viewport. Inside a tile every shape is drawn in the ink at the pattern
// opacity with CompositionMode_Source, holes (skull eyes, heart cracks) are
// cleared, and highlight inks replace the ink at their own opacity, so the
// tile composited over any base equals "ink premixed with the base" exactly,
// gradients included, and overlapping shapes never darken each other. Motifs
// built on a viewport-wide field (the cyber grid, the Aero bubbles, the
// halftone field and the zebra bands) are painted straight onto the
// viewport; all of them are seeded, so every viewer sees the same page.
namespace OpenChat::ProfileMotifs {

// An extra ink a motif draws besides its main one, at its own opacity over
// the base. The readability pass counts these as page samples.
struct HighlightInk final {
    QColor color;
    qreal opacity = 1.0;
};

struct BackdropSpec final {
    Profile::BackgroundKind kind = Profile::BackgroundKind::PatternBackground;
    QColor color1 = QColor(0xC7, 0xE1, 0xF5);
    QColor color2 = QColor(0xF1, 0xF8, 0xFD);
    Profile::Motif motif = Profile::Motif::Bubbles;
    Profile::MotifScale scale = Profile::MotifScale::MediumMotif;
    QColor ink = QColor(0x94, 0xC6, 0xEC);
    qreal opacity = 0.45; // 0…1
    bool aurora = false;  // the stub's calm backdrop: the bubbles' ribbons only
    // The dark-base look of the ribbons and bubble rims; a base colour darker
    // than luminance 0.18 selects it on its own.
    bool darkBase = false;
};

// S ×0.72, M ×1.0, L ×1.4 of a motif's base tile.
[[nodiscard]] qreal scaleFactor(Profile::MotifScale scale);
// false for the viewport motifs (CyberGrid, Bubbles, Halftone, Zebra).
[[nodiscard]] bool isTileMotif(Profile::Motif motif);
// The motif's repeat at DPR 1 before device rounding, empty for a viewport motif.
[[nodiscard]] QSizeF tilePeriod(Profile::Motif motif, Profile::MotifScale scale);
// The transparent tile for a repeating motif at `dpr` device pixels per
// logical pixel (its period rounded to whole device pixels so it repeats
// without seams); a null image for a viewport motif. Cached (4 MB).
[[nodiscard]] QImage motifTile(Profile::Motif motif, const QColor &ink, qreal opacity, Profile::MotifScale scale,
                               qreal dpr);
[[nodiscard]] QVector<HighlightInk> highlightInks(Profile::Motif motif, const QColor &ink, qreal opacity);

// Paints the whole backdrop into `rect` (base, pattern, aurora), `zoom`
// scaling everything (0.55 for preset miniatures, 0.5 for pattern swatches).
void paintBackdrop(QPainter &painter, const QRectF &rect, const BackdropSpec &spec, qreal zoom = 1.0);
// The motif layer alone, over whatever `painter` already holds.
void paintMotif(QPainter &painter, const QRectF &rect, const BackdropSpec &spec);

void clearTileCache();
[[nodiscard]] int cachedTileCount();

// Shapes shared with the name art, the ambient sprites and the thumbnails,
// with the mockups' geometry (rotations in radians, clockwise on screen).
[[nodiscard]] QPainterPath heartPath(const QPointF &centre, qreal size, qreal rotation = 0.0);
[[nodiscard]] QPainterPath sparklePath(const QPointF &centre, qreal radius);
[[nodiscard]] QPainterPath fivePointStar(const QPointF &centre, qreal radius, qreal rotation = 0.0);

// The mockups' seeded generator (mulberry32), so a motif's scatter is the
// same on every machine.
class Random final
{
public:
    explicit Random(quint32 seed) : m_state(seed) {}
    [[nodiscard]] double next();

private:
    quint32 m_state;
};

} // namespace OpenChat::ProfileMotifs
