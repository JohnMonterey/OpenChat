#pragma once

#include <QColor>
#include <QFont>
#include <QImage>
#include <QQuickPaintedItem>
#include <QString>

#include <memory>

namespace OpenChat {

class ProfileItemAnimation;

// The owner's styled name on the identity card (SPEC §7; the mockups'
// NameArt). One line: a name that does not fit first shrinks, down to
// `minPixelSize`, and only then elides. Effects are painted from the glyph
// outlines (QPainterPath::addText), never shaders. The flourish is painted
// with the effect but is never part of `accessibleName`, and neither appears
// anywhere but here: the plain display name is the identity everywhere else.
//
// The item reaches 0.36 × size beyond the text on the left and right and 0.26
// × size above and below, so glows, outlines and glints are never clipped,
// and it paints nothing outside itself. QML places it at x = −glyphLeft,
// y = −glyphTop so the glyphs start at the text edge.
//
// Only the static frame is cached; glitter's animated frames (the grain
// reseeded at 8 fps, one glint twinkling over 1.2 s) are painted live from
// the shared ticker, and only while the item is really on screen and
// animation is allowed.
class ProfileNameText : public QQuickPaintedItem
{
    Q_OBJECT
    Q_PROPERTY(QString text READ text WRITE setText NOTIFY textChanged)
    Q_PROPERTY(int flourish READ flourish WRITE setFlourish NOTIFY flourishChanged) // Profile.Flourish
    // "" = the interface font (QML passes Theme.uiFont when it has it).
    Q_PROPERTY(QString fontFamily READ fontFamily WRITE setFontFamily NOTIFY fontFamilyChanged)
    Q_PROPERTY(int basePixelSize READ basePixelSize WRITE setBasePixelSize NOTIFY basePixelSizeChanged)
    // −1 (the default): 30 for fancy names (Glitter, Script, Gothic), else
    // max(20, 0.7 × base); Pixel names step down its 8 px grid.
    Q_PROPERTY(int minPixelSize READ minPixelSize WRITE setMinPixelSize NOTIFY minPixelSizeChanged)
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged)
    Q_PROPERTY(QColor color2 READ color2 WRITE setColor2 NOTIFY color2Changed)
    Q_PROPERTY(int effect READ effect WRITE setEffect NOTIFY effectChanged) // Profile.NameEffect
    Q_PROPERTY(bool darkBox READ darkBox WRITE setDarkBox NOTIFY darkBoxChanged)
    // The width the text may take; 0 = its natural width.
    Q_PROPERTY(qreal availableWidth READ availableWidth WRITE setAvailableWidth NOTIFY availableWidthChanged)
    Q_PROPERTY(bool animate READ animate WRITE setAnimate NOTIFY animateChanged)
    // The glitter frame shown; 0 is the static frame. Tests set it directly.
    Q_PROPERTY(int phase READ phase WRITE setPhase NOTIFY phaseChanged)
    Q_PROPERTY(int renderedPixelSize READ renderedPixelSize NOTIFY layoutChanged)
    Q_PROPERTY(bool elided READ elided NOTIFY layoutChanged)
    Q_PROPERTY(qreal glyphLeft READ glyphLeft NOTIFY layoutChanged)
    Q_PROPERTY(qreal glyphTop READ glyphTop NOTIFY layoutChanged)
    // The width of the text line itself (without the overhang).
    Q_PROPERTY(qreal textWidth READ textWidth NOTIFY layoutChanged)
    // What screen readers get: the plain name, never the flourish.
    Q_PROPERTY(QString accessibleName READ text NOTIFY textChanged)
    // What is painted: flourish and elision included.
    Q_PROPERTY(QString paintedText READ paintedText NOTIFY layoutChanged)

public:
    explicit ProfileNameText(QQuickItem *parent = nullptr);
    ~ProfileNameText() override;

    [[nodiscard]] QString text() const { return m_text; }
    void setText(const QString &text);
    [[nodiscard]] int flourish() const noexcept { return m_flourish; }
    void setFlourish(int flourish);
    [[nodiscard]] QString fontFamily() const { return m_fontFamily; }
    void setFontFamily(const QString &family);
    [[nodiscard]] int basePixelSize() const noexcept { return m_basePixelSize; }
    void setBasePixelSize(int size);
    [[nodiscard]] int minPixelSize() const noexcept { return m_minPixelSize; }
    void setMinPixelSize(int size);
    [[nodiscard]] QColor color() const { return m_color; }
    void setColor(const QColor &color);
    [[nodiscard]] QColor color2() const { return m_color2; }
    void setColor2(const QColor &color);
    [[nodiscard]] int effect() const noexcept { return m_effect; }
    void setEffect(int effect);
    [[nodiscard]] bool darkBox() const noexcept { return m_darkBox; }
    void setDarkBox(bool dark);
    [[nodiscard]] qreal availableWidth() const noexcept { return m_availableWidth; }
    void setAvailableWidth(qreal width);
    [[nodiscard]] bool animate() const noexcept { return m_animate; }
    void setAnimate(bool animate);
    [[nodiscard]] int phase() const noexcept { return m_phase; }
    void setPhase(int phase);

    [[nodiscard]] int renderedPixelSize() const noexcept { return m_size; }
    [[nodiscard]] bool elided() const noexcept { return m_elided; }
    [[nodiscard]] qreal glyphLeft() const noexcept { return m_padX; }
    [[nodiscard]] qreal glyphTop() const noexcept { return m_padY; }
    [[nodiscard]] qreal textWidth() const noexcept { return m_textWidth; }
    [[nodiscard]] QString paintedText() const { return m_shown; }
    // Whether the shared ticker is driving the glitter right now.
    [[nodiscard]] bool animating() const;

    // The current frame at `dpr`, as the item paints it.
    [[nodiscard]] QImage renderFrame(qreal dpr) const;
    // Static frames held by the process-wide cache (tests).
    [[nodiscard]] static int cachedFrameCount();
    static void clearFrameCache();

    void paint(QPainter *painter) override;

signals:
    void textChanged();
    void flourishChanged();
    void fontFamilyChanged();
    void basePixelSizeChanged();
    void minPixelSizeChanged();
    void colorChanged();
    void color2Changed();
    void effectChanged();
    void darkBoxChanged();
    void availableWidthChanged();
    void animateChanged();
    void phaseChanged();
    void layoutChanged();

protected:
    void itemChange(ItemChange change, const ItemChangeData &value) override;

private:
    void relayout();
    void updateAnimation();
    [[nodiscard]] QFont fontAt(int pixelSize) const;
    [[nodiscard]] QString cacheKey(qreal dpr) const;
    void paintFrame(QPainter &painter, int phase) const;

    QString m_text;
    int m_flourish = 0;
    QString m_fontFamily;
    int m_basePixelSize = 28;
    int m_minPixelSize = -1;
    QColor m_color = QColor(0x1C, 0x3D, 0x63);
    QColor m_color2 = QColor(0xFF, 0xFF, 0xFF);
    int m_effect = 0;
    bool m_darkBox = false;
    qreal m_availableWidth = 0;
    bool m_animate = true;
    int m_phase = 0;

    // Layout, recomputed by relayout().
    QString m_family;       // resolved family
    bool m_bold = false;
    int m_size = 28;
    bool m_elided = false;
    QString m_shown;
    qreal m_textWidth = 0;
    qreal m_padX = 0;
    qreal m_padY = 0;
    qreal m_boxHeight = 0;
    qreal m_baselineShift = 0; // fraction of the size

    std::unique_ptr<ProfileItemAnimation> m_animation;
};

} // namespace OpenChat
