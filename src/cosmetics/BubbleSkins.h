#pragma once

#include <QColor>
#include <QImage>
#include <QList>
#include <QPainterPath>
#include <QRectF>
#include <QString>

class QPainter;

namespace OpenChat {

// One collectible chat-bubble skin, as the catalogue lists it.
struct BubbleSkinInfo
{
    QString id;
    QString name;
    QString description;
};

// The geometry BubbleBackground lays a bubble out with. A skin paints inside
// `path` (the rounded body plus speech tail) and may use `body` — the rounded
// rectangle without the tail column — to place lighting that should not spill
// into the tail.
struct BubbleShape
{
    QPainterPath path;
    QRectF bounds;
    QRectF body;
    bool outgoing = false;
    qreal radius = 6.0;
};

// Procedurally drawn bubble materials. The classic bubble is not a skin: an
// empty id, "classic" or any unknown id means "draw the classic gradient", and
// every function here reports that by returning false / an invalid colour.
//
// Each skin's texture is generated once per process from a fixed seed, then
// cached and tiled, so a list of many skinned bubbles costs one generation.
namespace BubbleSkins {

[[nodiscard]] QList<BubbleSkinInfo> catalog();
[[nodiscard]] bool isSkin(const QString &id);
[[nodiscard]] QColor textColor(const QString &id);
[[nodiscard]] QColor secondaryTextColor(const QString &id);

// Paints the skin's material, lighting and outline for the shape. The result
// covers exactly the classic bubble's footprint: the fill is antialiased to the
// path and the outline straddles it the way the classic 1 px border does.
void paint(QPainter *painter, const QString &id, const BubbleShape &shape,
           qreal devicePixelRatio);

// The cached tile a skin repeats (generated on first use). Exposed for tests
// and for the gallery's texture dump; null for the classic bubble.
[[nodiscard]] QImage texture(const QString &id);

} // namespace BubbleSkins

} // namespace OpenChat
