#pragma once

#include <QImage>
#include <QPainterPath>
#include <QRectF>
#include <QString>

class QPainter;

namespace OpenChat {

// Draws the artwork behind an avatar key, independently of any scene graph.
//
// The interface names every profile picture by a string key: the bundled
// reference artwork ("michael", "userpfp_none", …), the procedural placeholders
// ("landscape", "beach", "mono", …) and the "blob:…" content keys AvatarStore
// registers for pictures received from contacts or chosen locally. This is the
// one place that turns such a key into pixels, so the picture on the call
// screen, in the sidebar and in a desktop notification are the same picture.
namespace AvatarPainter {

// The rounded-rectangle silhouette an avatar is clipped to. The radius is
// clamped to at most half the shorter side, so passing a very large radius
// yields a circle.
[[nodiscard]] QPainterPath makeClipPath(const QRectF &bounds, qreal cornerRadius);

// Draws the avatar into `rect` on an existing painter, clipped to
// makeClipPath(). The painter's clip and render hints are left changed; save()
// and restore() around this if that matters to the caller.
void paint(QPainter &painter, const QRectF &rect, const QString &avatarKey, qreal cornerRadius);

// Renders the avatar standalone into a square ARGB image of `sizePx` a side,
// transparent outside the clip path. Returns a null image for a non-positive
// size. This is what off-screen consumers (desktop notifications) use.
[[nodiscard]] QImage render(const QString &avatarKey, int sizePx, qreal cornerRadius);

} // namespace AvatarPainter

} // namespace OpenChat
