#pragma once

#include <QRectF>
#include <QString>

class QPainter;

// The artwork of every avatar frame. AvatarFrame owns sizing, caching and
// animation; these functions only draw one frame at one phase.
namespace OpenChat::FramePainters {

struct Canvas
{
    QRectF bounds;    // the whole frame image, logical pixels
    QRectF picture;   // where the display picture sits inside it
    qreal radius = 5; // the picture's corner radius
    qreal margin = 6; // left/right/bottom reach past the picture
    qreal scale = 1;  // picture size relative to the 44 px sidebar picture
    bool dark = false;
    int phase = 0;
    int phases = 1;
    qreal dpr = 1;
};

qreal sideMargin(qreal avatarSize);
qreal extraTop(const QString &frameId, qreal avatarSize);
int phaseCount(const QString &frameId);
int phaseInterval(const QString &frameId);
void paint(QPainter &painter, const QString &frameId, const Canvas &canvas);

} // namespace OpenChat::FramePainters
