#pragma once

#include "profile/ProfileMotifs.h"

#include <QColor>
#include <QPointF>
#include <QQuickItem>
#include <QVector>

#include <memory>
#include <vector>

namespace OpenChat {

class ProfileItemAnimation;

// A page's falling stars, hearts or snow, or floating sparkles (SPEC §8.2):
// up to 28 small child sprites, each painted once, moved on the shared ticker
// at 30 fps (falling 20–45 px/s with a ±8 px sine drift, respawning at the
// top).
// It never repaints the viewport: moving a sprite only moves its node. It sits
// above the backdrop and below the boxes, so it never covers text, and it
// holds still whenever the page is not really on screen or animation is not
// allowed (Low memory mode, reduced motion).
class ProfileAmbient : public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(int kind READ kind WRITE setKind NOTIFY kindChanged) // Profile.Ambient
    // The link ink at α .4 over light backdrops; transparent otherwise.
    Q_PROPERTY(QColor outlineColor READ outlineColor WRITE setOutlineColor NOTIFY outlineColorChanged)
    // The page wants motion (it is settled, not covered, not in plain style).
    Q_PROPERTY(bool running READ running WRITE setRunning NOTIFY runningChanged)
    Q_PROPERTY(int seed READ seed WRITE setSeed NOTIFY seedChanged)
    // Whether the sprites are moving right now.
    Q_PROPERTY(bool animating READ animating NOTIFY animatingChanged)
    Q_PROPERTY(int spriteCount READ spriteCount NOTIFY spritesChanged)

public:
    static constexpr int minSprites = 8;
    static constexpr int maxSprites = 28;
    static constexpr int fps = 30;

    explicit ProfileAmbient(QQuickItem *parent = nullptr);
    ~ProfileAmbient() override;

    [[nodiscard]] int kind() const noexcept { return m_kind; }
    void setKind(int kind);
    [[nodiscard]] QColor outlineColor() const { return m_outlineColor; }
    void setOutlineColor(const QColor &color);
    [[nodiscard]] bool running() const noexcept { return m_running; }
    void setRunning(bool running);
    [[nodiscard]] int seed() const noexcept { return m_seed; }
    void setSeed(int seed);
    [[nodiscard]] bool animating() const;
    [[nodiscard]] int spriteCount() const noexcept { return int(m_sprites.size()); }

    // clamp(area / 40 000, 8, 28).
    [[nodiscard]] static int densityFor(const QSizeF &area);
    // Where each sprite is now (tests).
    [[nodiscard]] QVector<QPointF> spritePositions() const;

signals:
    void kindChanged();
    void outlineColorChanged();
    void runningChanged();
    void seedChanged();
    void animatingChanged();
    void spritesChanged();

protected:
    void itemChange(ItemChange change, const ItemChangeData &value) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

private:
    struct Sprite final {
        QQuickItem *item = nullptr; // owned by this item (a child)
        qreal baseX = 0;            // the drift's centre line
        qreal y = 0;
        qreal speed = 0;            // px/s
        qreal driftPhase = 0;
        qreal driftRate = 0;        // rad/s
        qreal size = 0;             // the glyph's size parameter, 5–11 px
    };

    void rebuild();
    void step();
    void place(Sprite &sprite) const;
    void updateAnimation();

    int m_kind = 0;
    QColor m_outlineColor = QColor(0, 0, 0, 0);
    bool m_running = true;
    int m_seed = 1;
    qreal m_time = 0;
    std::vector<Sprite> m_sprites;
    std::unique_ptr<ProfileMotifs::Random> m_random; // respawn positions, seeded
    std::unique_ptr<ProfileItemAnimation> m_animation;
};

} // namespace OpenChat
