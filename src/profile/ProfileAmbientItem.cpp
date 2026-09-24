#include "profile/ProfileAmbientItem.h"

#include "domain/ProfilePage.h"
#include "profile/ProfileTicker.h"

#include <QPainter>
#include <QPainterPath>
#include <QQuickPaintedItem>

#include <algorithm>
#include <cmath>

namespace OpenChat {

namespace {

using Profile::Ambient;

constexpr qreal driftAmplitude = 8.0; // px

// One falling (or floating) glyph. It paints itself once; moving it moves
// its node and never repaints.
class AmbientSprite final : public QQuickPaintedItem
{
public:
    AmbientSprite(QQuickItem *parent, Ambient kind, qreal size, qreal alpha, qreal rotation, const QColor &outline)
        : QQuickPaintedItem(parent), m_kind(kind), m_size(size), m_alpha(alpha), m_rotation(rotation),
          m_outline(outline)
    {
        setAntialiasing(true);
        const qreal extent = std::ceil(glyph().boundingRect().width()) + 2;
        const qreal side = std::max(extent, std::ceil(glyph().boundingRect().height()) + 2);
        setSize(QSizeF(side, side));
    }

    void setOutline(const QColor &outline)
    {
        if (outline == m_outline)
            return;
        m_outline = outline;
        update();
    }

    void paint(QPainter *painter) override
    {
        painter->setRenderHint(QPainter::Antialiasing, true);
        const QPainterPath shape = glyph().translated(width() / 2, height() / 2);
        QColor fill(Qt::white);
        fill.setAlphaF(float(m_alpha));
        painter->fillPath(shape, fill);
        if (m_outline.alpha() > 0)
            painter->strokePath(shape, QPen(m_outline, 1.0));
    }

private:
    // Centred on the origin, the mockups' shapes (Motifs.js paintAmbient).
    [[nodiscard]] QPainterPath glyph() const
    {
        switch (m_kind) {
        case Ambient::FallingHearts:
            return ProfileMotifs::heartPath(QPointF(0, 0), m_size * 2.2, m_rotation);
        case Ambient::FallingSnow: {
            QPainterPath flake;
            flake.addEllipse(QPointF(0, 0), m_size * 0.4, m_size * 0.4);
            return flake;
        }
        case Ambient::FloatingSparkles:
            return ProfileMotifs::sparklePath(QPointF(0, 0), m_size * 0.8);
        case Ambient::FallingStars:
        case Ambient::NoAmbient:
            break;
        }
        return ProfileMotifs::sparklePath(QPointF(0, 0), m_size);
    }

    Ambient m_kind;
    qreal m_size;
    qreal m_alpha;
    qreal m_rotation;
    QColor m_outline;
};

} // namespace

ProfileAmbient::ProfileAmbient(QQuickItem *parent) : QQuickItem(parent)
{
    m_animation = std::make_unique<ProfileItemAnimation>(
        this, fps, [this](int) { step(); }, [this](bool) { emit animatingChanged(); });
}

ProfileAmbient::~ProfileAmbient() = default;

int ProfileAmbient::densityFor(const QSizeF &area)
{
    return std::clamp(int(std::lround(area.width() * area.height() / 40000.0)), minSprites, maxSprites);
}

void ProfileAmbient::setKind(int kind)
{
    if (kind < 0 || kind > int(Ambient::FloatingSparkles) || kind == m_kind)
        return;
    m_kind = kind;
    emit kindChanged();
    rebuild();
}

void ProfileAmbient::setOutlineColor(const QColor &color)
{
    if (color == m_outlineColor)
        return;
    m_outlineColor = color;
    emit outlineColorChanged();
    for (const Sprite &sprite : m_sprites)
        static_cast<AmbientSprite *>(sprite.item)->setOutline(color);
}

void ProfileAmbient::setRunning(bool running)
{
    if (running == m_running)
        return;
    m_running = running;
    emit runningChanged();
    updateAnimation();
}

void ProfileAmbient::setSeed(int seed)
{
    if (seed == m_seed)
        return;
    m_seed = seed;
    emit seedChanged();
    rebuild();
}

bool ProfileAmbient::animating() const
{
    return m_animation->running();
}

QVector<QPointF> ProfileAmbient::spritePositions() const
{
    QVector<QPointF> positions;
    positions.reserve(qsizetype(m_sprites.size()));
    for (const Sprite &sprite : m_sprites)
        positions.push_back(sprite.item->position());
    return positions;
}

void ProfileAmbient::itemChange(ItemChange change, const ItemChangeData &value)
{
    QQuickItem::itemChange(change, value);
    m_animation->itemChange(change, value);
}

void ProfileAmbient::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() == oldGeometry.size())
        return;
    if (densityFor(newGeometry.size()) != spriteCount() || m_sprites.empty()) {
        rebuild();
        return;
    }
    // Same density: keep the sprites where they are across the viewport.
    const qreal scaleX = oldGeometry.width() > 0 ? newGeometry.width() / oldGeometry.width() : 1.0;
    const qreal scaleY = oldGeometry.height() > 0 ? newGeometry.height() / oldGeometry.height() : 1.0;
    for (Sprite &sprite : m_sprites) {
        sprite.baseX *= scaleX;
        sprite.y *= scaleY;
        place(sprite);
    }
}

void ProfileAmbient::updateAnimation()
{
    m_animation->setWanted(m_running && m_kind != int(Ambient::NoAmbient) && !m_sprites.empty());
}

void ProfileAmbient::rebuild()
{
    for (Sprite &sprite : m_sprites)
        delete sprite.item;
    m_sprites.clear();
    m_time = 0;
    m_random = std::make_unique<ProfileMotifs::Random>(quint32(m_seed) * 2654435761U + 99U);
    if (m_kind != int(Ambient::NoAmbient) && width() > 0 && height() > 0) {
        const int count = densityFor(size());
        ProfileMotifs::Random &random = *m_random;
        for (int i = 0; i < count; ++i) {
            Sprite sprite;
            sprite.baseX = random.next() * width();
            sprite.y = random.next() * height();
            sprite.size = 5 + random.next() * 6;
            const qreal alpha = 0.45 + random.next() * 0.45;
            const qreal rotation = random.next() - 0.5;
            sprite.speed = 20 + random.next() * 25;
            sprite.driftPhase = random.next() * 6.283;
            sprite.driftRate = 0.6 + random.next() * 0.9;
            sprite.item = new AmbientSprite(this, Ambient(m_kind), sprite.size, alpha, rotation, m_outlineColor);
            place(sprite);
            m_sprites.push_back(sprite);
        }
    }
    emit spritesChanged();
    updateAnimation();
}

void ProfileAmbient::place(Sprite &sprite) const
{
    const qreal x = sprite.baseX + driftAmplitude * std::sin(sprite.driftPhase + m_time * sprite.driftRate);
    sprite.item->setPosition(QPointF(x - sprite.item->width() / 2, sprite.y - sprite.item->height() / 2));
}

void ProfileAmbient::step()
{
    const qreal dt = 1.0 / fps;
    m_time += dt;
    // Every kind drifts down (SPEC §8.2); a sprite that falls out of the
    // viewport comes back just above the top, somewhere new along it.
    for (Sprite &sprite : m_sprites) {
        const qreal margin = sprite.item->height();
        sprite.y += sprite.speed * dt;
        if (sprite.y > height() + margin) {
            sprite.y = -margin;
            sprite.baseX = m_random->next() * width();
        }
        place(sprite);
    }
}

} // namespace OpenChat
