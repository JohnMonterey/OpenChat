#include "render/TrayOrb.h"

#include <QColor>
#include <QLinearGradient>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QRadialGradient>

#include <algorithm>
#include <optional>

namespace OpenChat::TrayOrb {

namespace {

// One state's glass, lightest to darkest, and the edge drawn around it.
struct OrbInk
{
    QColor light;
    QColor base;
    QColor dark;
    QColor rim;
};

std::optional<OrbInk> inkFor(QStringView state)
{
    // In the call and quiet: a deep green that reads as "on" without drawing
    // the eye the way talking does.
    if (state == u"call")
        return OrbInk{QColor(0x6f, 0xb8, 0x3c), QColor(0x2b, 0x7a, 0x14), QColor(0x14, 0x4a, 0x08),
                      QColor(0x0b, 0x33, 0x04)};
    if (state == u"talking")
        return OrbInk{QColor(0xe4, 0xff, 0xb8), QColor(0x62, 0xe8, 0x2c), QColor(0x2f, 0xa6, 0x0e),
                      QColor(0x1e, 0x7a, 0x08)};
    if (state == u"muted")
        return OrbInk{QColor(0xff, 0xb4, 0xa6), QColor(0xe2, 0x3c, 0x28), QColor(0x9a, 0x18, 0x0c),
                      QColor(0x6c, 0x0e, 0x06)};
    if (state == u"deafened")
        return OrbInk{QColor(0x9c, 0xa4, 0xac), QColor(0x56, 0x5e, 0x66), QColor(0x2e, 0x34, 0x3a),
                      QColor(0x1a, 0x1e, 0x22)};
    return std::nullopt;
}

} // namespace

QImage render(QStringView state, int side)
{
    const std::optional<OrbInk> ink = inkFor(state);
    if (!ink || side <= 0)
        return {};

    QImage image(side, side, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing, true);

    // A rim a pixel wide at the tray's 16 px, and the orb just inside the
    // square so the rim's antialiasing is not cut off.
    const qreal rimWidth = std::max(1.0, side / 20.0);
    const qreal radius = side / 2.0 - rimWidth / 2.0 - side / 64.0;
    const QPointF centre(side / 2.0, side / 2.0);

    // The glass, lit from above left and darkening towards the far edge.
    QRadialGradient body(centre + QPointF(-radius * 0.25, -radius * 0.35), radius * 1.45);
    body.setColorAt(0.0, ink->light);
    body.setColorAt(0.45, ink->base);
    body.setColorAt(1.0, ink->dark);
    p.setPen(QPen(ink->rim, rimWidth));
    p.setBrush(body);
    p.drawEllipse(centre, radius, radius);

    // The Aero gloss: a soft white reflection over the upper half, as on the
    // presence beads.
    QLinearGradient shine(centre.x(), centre.y() - radius, centre.x(), centre.y());
    shine.setColorAt(0.0, QColor(255, 255, 255, 215));
    shine.setColorAt(1.0, QColor(255, 255, 255, 0));
    p.setPen(Qt::NoPen);
    p.setBrush(shine);
    p.drawEllipse(QPointF(centre.x(), centre.y() - radius * 0.42), radius * 0.66, radius * 0.46);
    return image;
}

QIcon icon(QStringView state)
{
    QIcon orb;
    for (const int side : {16, 20, 24, 32, 40, 48, 64}) {
        const QImage image = render(state, side);
        if (image.isNull())
            return {};
        orb.addPixmap(QPixmap::fromImage(image));
    }
    return orb;
}

} // namespace OpenChat::TrayOrb
