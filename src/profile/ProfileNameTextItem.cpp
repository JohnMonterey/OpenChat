#include "profile/ProfileNameTextItem.h"

#include "cosmetics/CosmeticPaint.h"
#include "domain/ProfilePage.h"
#include "profile/ProfileFonts.h"
#include "profile/ProfileMotifs.h"
#include "profile/ProfileReadability.h"
#include "profile/ProfileTicker.h"

#include <QCache>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QLinearGradient>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <limits>

namespace OpenChat {

namespace {

using Profile::NameEffect;
namespace Readability = ProfileReadability;

constexpr double pi = 3.14159265358979323846;
constexpr int glitterFps = 8;
constexpr int twinkleFrames = 10; // one glint twinkles over 1.2 s at 8 fps
constexpr int phaseCycle = 240;   // 30 s of distinct grain before it repeats
constexpr int grainSeed = 3;      // the mockups' seed

struct FrameCache final {
    QMutex mutex;
    QCache<QString, QImage> images{4 * 1024}; // KB
};

FrameCache &frameCache()
{
    static FrameCache cache;
    return cache;
}

QColor withAlpha(QColor colour, qreal alpha)
{
    colour.setAlphaF(float(std::clamp(alpha, 0.0, 1.0)));
    return colour;
}

// A four-point glint with a soft glow in `glow` (the canvas shadow of the
// mockups: blur `blur` logical px), then crisp white.
void glint(QPainter &p, const QPointF &centre, qreal radius, const QColor &glow, qreal blur)
{
    const QPainterPath star = ProfileMotifs::sparklePath(centre, radius);
    CosmeticPaint::drawBlurred(p, star.boundingRect(), blur, [&](QPainter &layer) { layer.fillPath(star, glow); });
    p.fillPath(star, Qt::white);
}

// The glyphs' silhouette blurred by `blur` in `colour`, under whatever is
// drawn next (a canvas text shadow with no offset).
void shadowOf(QPainter &p, const QPainterPath &glyphs, const QColor &colour, qreal blur)
{
    CosmeticPaint::drawBlurred(p, glyphs.boundingRect(), blur,
                               [&](QPainter &layer) { layer.fillPath(glyphs, colour); });
}

} // namespace

ProfileNameText::ProfileNameText(QQuickItem *parent) : QQuickPaintedItem(parent)
{
    setAntialiasing(true);
    m_animation = std::make_unique<ProfileItemAnimation>(
        this, glitterFps,
        [this](int) { setPhase(m_phase >= phaseCycle ? 1 : m_phase + 1); },
        [this](bool running) {
            if (!running)
                setPhase(0); // stopped means the stable frame
        });
    // Metrics change when the bundled faces are registered after this item
    // was laid out.
    if (qGuiApp)
        connect(qGuiApp, &QGuiApplication::fontDatabaseChanged, this, &ProfileNameText::relayout);
    relayout();
}

ProfileNameText::~ProfileNameText() = default;

void ProfileNameText::setText(const QString &text)
{
    if (text == m_text)
        return;
    m_text = text;
    emit textChanged();
    relayout();
}

void ProfileNameText::setFlourish(int flourish)
{
    if (flourish < 0 || flourish > int(Profile::Flourish::FlowerFlourish) || flourish == m_flourish)
        return;
    m_flourish = flourish;
    emit flourishChanged();
    relayout();
}

void ProfileNameText::setFontFamily(const QString &family)
{
    if (family == m_fontFamily)
        return;
    m_fontFamily = family;
    emit fontFamilyChanged();
    relayout();
}

void ProfileNameText::setBasePixelSize(int size)
{
    size = std::clamp(size, 1, 256);
    if (size == m_basePixelSize)
        return;
    m_basePixelSize = size;
    emit basePixelSizeChanged();
    relayout();
}

void ProfileNameText::setMinPixelSize(int size)
{
    size = std::max(-1, size);
    if (size == m_minPixelSize)
        return;
    m_minPixelSize = size;
    emit minPixelSizeChanged();
    relayout();
}

void ProfileNameText::setColor(const QColor &color)
{
    if (color == m_color)
        return;
    m_color = color;
    emit colorChanged();
    update();
}

void ProfileNameText::setColor2(const QColor &color)
{
    if (color == m_color2)
        return;
    m_color2 = color;
    emit color2Changed();
    update();
}

void ProfileNameText::setEffect(int effect)
{
    if (effect < 0 || effect > int(NameEffect::ShadowName) || effect == m_effect)
        return;
    m_effect = effect;
    emit effectChanged();
    relayout(); // Glitter is a fancy name: it has a 30 px floor
}

void ProfileNameText::setDarkBox(bool dark)
{
    if (dark == m_darkBox)
        return;
    m_darkBox = dark;
    emit darkBoxChanged();
    update();
}

void ProfileNameText::setAvailableWidth(qreal width)
{
    width = std::max(0.0, width);
    if (qFuzzyCompare(width + 1.0, m_availableWidth + 1.0))
        return;
    m_availableWidth = width;
    emit availableWidthChanged();
    relayout();
}

void ProfileNameText::setAnimate(bool animate)
{
    if (animate == m_animate)
        return;
    m_animate = animate;
    emit animateChanged();
    updateAnimation();
}

void ProfileNameText::setPhase(int phase)
{
    phase = std::max(0, phase);
    if (phase == m_phase)
        return;
    m_phase = phase;
    emit phaseChanged();
    if (m_effect == int(NameEffect::GlitterName))
        update();
}

bool ProfileNameText::animating() const
{
    return m_animation->running();
}

void ProfileNameText::itemChange(ItemChange change, const ItemChangeData &value)
{
    QQuickPaintedItem::itemChange(change, value);
    m_animation->itemChange(change, value);
}

void ProfileNameText::updateAnimation()
{
    m_animation->setWanted(m_animate && m_effect == int(NameEffect::GlitterName) && !m_shown.isEmpty());
}

QFont ProfileNameText::fontAt(int pixelSize) const
{
    QFont font(m_family);
    font.setPixelSize(std::max(1, pixelSize));
    font.setBold(m_bold);
    return font;
}

void ProfileNameText::relayout()
{
    const std::optional<Profile::Font> face = ProfileFonts::fontForFamily(m_fontFamily);
    const Profile::Font resolved = face.value_or(Profile::Font::InterfaceFont);
    m_family = m_fontFamily.isEmpty() ? ProfileFonts::interfaceFamily() : m_fontFamily;
    m_bold = ProfileFonts::useBold(resolved, ProfileFonts::Role::Name);
    const int grid = ProfileFonts::pixelGrid(resolved);
    const bool fancy = m_effect == int(NameEffect::GlitterName) || resolved == Profile::Font::ScriptFont
                       || resolved == Profile::Font::GothicFont;

    // The size ladder: the base (Pixel on its grid; fancy names at 30 or
    // more, so an M script name renders at 30), down to the floor.
    int base = m_basePixelSize;
    if (grid > 0)
        base = std::max(grid, base / grid * grid);
    if (fancy)
        base = std::max(base, 30);
    int floorSize = 0;
    if (m_minPixelSize >= 0)
        floorSize = m_minPixelSize;
    else if (grid > 0)
        floorSize = std::max(grid, int(0.7 * base) / grid * grid);
    else
        floorSize = fancy ? 30 : std::max(20, int(std::lround(0.7 * base)));
    if (fancy)
        floorSize = std::max(floorSize, 30);
    floorSize = std::min(floorSize, base);

    const auto flourish = Profile::Flourish(m_flourish);
    const QString prefix = Profile::flourishPrefix(flourish);
    const QString suffix = Profile::flourishSuffix(flourish);
    const QString full = m_text.isEmpty() ? QString() : prefix + m_text + suffix;
    const qreal limit = m_availableWidth > 0 ? m_availableWidth : std::numeric_limits<qreal>::max();
    const auto advance = [this](int px, const QString &text) {
        return QFontMetricsF(fontAt(px)).horizontalAdvance(text);
    };

    // 1. Shrink to fit, never below the floor (Pixel: whole grid steps).
    int size = base;
    const qreal natural = advance(size, full);
    if (natural > limit) {
        size = std::max(floorSize, int(std::floor(size * limit / natural)));
        if (grid > 0)
            size = std::max(floorSize, size / grid * grid);
        const int step = grid > 0 ? grid : 1;
        while (size - step >= floorSize && advance(size, full) > limit)
            size -= step;
    }
    // 2. Only then elide, keeping the flourish while the name still has room.
    QString shown = full;
    bool elided = false;
    if (advance(size, full) > limit) {
        elided = true;
        const QFontMetricsF metrics(fontAt(size));
        const qreal nameRoom = limit - metrics.horizontalAdvance(prefix) - metrics.horizontalAdvance(suffix);
        if (!prefix.isEmpty() && nameRoom >= metrics.horizontalAdvance(QStringLiteral("M…")))
            shown = prefix + metrics.elidedText(m_text, Qt::ElideRight, nameRoom) + suffix;
        else
            shown = metrics.elidedText(m_text, Qt::ElideRight, limit);
    }

    const qreal textWidth = std::min(limit, advance(size, shown));
    const qreal padX = std::ceil(size * 0.36);
    const qreal padY = std::ceil(size * 0.26);
    const qreal boxHeight = std::ceil(size * ProfileFonts::nameLineFactor(resolved));
    const bool changed = size != m_size || elided != m_elided || shown != m_shown
                         || !qFuzzyCompare(textWidth + 1, m_textWidth + 1) || padX != m_padX || padY != m_padY
                         || boxHeight != m_boxHeight;
    m_size = size;
    m_elided = elided;
    m_shown = shown;
    m_textWidth = textWidth;
    m_padX = padX;
    m_padY = padY;
    m_boxHeight = boxHeight;
    m_baselineShift = ProfileFonts::nameBaselineShift(resolved);
    setImplicitSize(std::ceil(textWidth) + 2 * padX, boxHeight + 2 * padY);
    if (changed)
        emit layoutChanged();
    updateAnimation();
    update();
}

QString ProfileNameText::cacheKey(qreal dpr) const
{
    return QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8|%9")
        .arg(m_shown, m_family)
        .arg(int(m_bold))
        .arg(m_size)
        .arg(m_color.rgba(), 8, 16)
        .arg(m_color2.rgba(), 8, 16)
        .arg(m_effect)
        .arg(int(m_darkBox))
        .arg(QStringLiteral("%1x%2@%3|%4").arg(width()).arg(height()).arg(dpr, 0, 'f', 3).arg(m_textWidth));
}

void ProfileNameText::paintFrame(QPainter &p, int phase) const
{
    if (m_shown.isEmpty())
        return;
    p.setRenderHint(QPainter::Antialiasing, true);
    const QFont font = fontAt(m_size);
    const QFontMetricsF metrics(font);
    const qreal s = m_size;
    const qreal x = m_padX;
    const qreal w = m_textWidth;
    // The line's middle, and the baseline that centres the em box on it (the
    // mockups' textBaseline "middle"), nudged for Pacifico and Unifraktur.
    const qreal mid = m_padY + m_boxHeight / 2 + m_baselineShift * s;
    const qreal baseline = mid + (metrics.ascent() - metrics.descent()) / 2;
    QPainterPath glyphs;
    glyphs.addText(QPointF(x, baseline), font, m_shown);
    const bool light = !m_darkBox;

    switch (NameEffect(m_effect)) {
    case NameEffect::PlainName:
        p.fillPath(glyphs, m_color);
        break;
    case NameEffect::GlowName: {
        // The Windows 7 caption glow: two blurred passes, then crisp.
        const qreal blur = std::max(8.0, 0.4 * s);
        shadowOf(p, glyphs, m_color2, blur);
        shadowOf(p, glyphs, m_color2, blur);
        p.fillPath(glyphs, m_color);
        break;
    }
    case NameEffect::OutlineName:
        p.strokePath(glyphs, QPen(m_color2, std::max(3.0, s / 8), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.fillPath(glyphs, m_color);
        break;
    case NameEffect::GradientName: {
        QLinearGradient ramp(0, mid - s * 0.5, 0, mid + s * 0.45);
        ramp.setColorAt(0, m_color);
        ramp.setColorAt(1, m_color2);
        p.fillPath(glyphs, ramp);
        break;
    }
    case NameEffect::ShadowName: {
        const qreal offset = std::max(2.0, std::round(s / 14));
        p.fillPath(glyphs.translated(offset, offset), m_color2);
        p.fillPath(glyphs, m_color);
        break;
    }
    case NameEffect::GlitterName: {
        // 1. A soft halo in the name colour.
        shadowOf(p, glyphs, withAlpha(m_color, light ? 0.45 : 0.8), s * (light ? 0.22 : 0.30));
        p.fillPath(glyphs, m_color);
        // 2. A keyline, so the letterforms stay crisp under the sparkle.
        p.strokePath(glyphs, QPen(Readability::darken(m_color, light ? 0.42 : 0.55), std::max(2.0, s / 13),
                                  Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        // 3. A vertical sheen.
        QLinearGradient sheen(0, mid - s * 0.5, 0, mid + s * 0.45);
        sheen.setColorAt(0, Readability::lighten(m_color, 0.5));
        sheen.setColorAt(0.55, m_color);
        sheen.setColorAt(1, Readability::darken(m_color, 0.1));
        p.fillPath(glyphs, sheen);
        // 4. Sparkle grain inside the letters, reseeded every frame.
        {
            p.save();
            p.setClipPath(glyphs, Qt::IntersectClip);
            ProfileMotifs::Random random(quint32(grainSeed + phase * 7));
            const int grains = int(std::lround(w * s / 40));
            for (int i = 0; i < grains; ++i) {
                const qreal px = x + random.next() * w;
                const qreal py = mid - s * 0.6 + random.next() * s * 1.2;
                const QColor grain = random.next() < 0.7 ? withAlpha(Qt::white, 0.55 + random.next() * 0.45)
                                                         : withAlpha(m_color2, 0.95);
                const qreal side = random.next() < 0.25 ? 2 : 1;
                p.fillRect(QRectF(std::round(px), std::round(py), side, side), grain);
            }
            p.restore();
        }
        // 5. Four-point glints riding the letter tops; in an animated frame
        //    one of them twinkles.
        ProfileMotifs::Random random(grainSeed);
        const int count = std::max(2, int(std::lround(w / 70)));
        const int twinkling = (phase / twinkleFrames) % count;
        const qreal pulse = 0.35 + 0.65 * std::abs(std::cos(pi * (phase % twinkleFrames) / twinkleFrames));
        const QColor glow = light ? withAlpha(m_color, 0.9) : withAlpha(Qt::white, 0.9);
        for (int i = 0; i < count; ++i) {
            const qreal gx = x + (i + 0.25 + random.next() * 0.5) * w / count;
            const qreal gy = mid - s * (0.2 + random.next() * 0.28);
            qreal radius = s * (0.1 + random.next() * 0.07);
            if (phase != 0 && i == twinkling)
                radius *= pulse;
            glint(p, QPointF(gx, gy), radius, glow, 5);
        }
        break;
    }
    case NameEffect::ChromeName: {
        const qreal top = mid - s * 0.52;
        const qreal bottom = mid + s * 0.46;
        // 1. A glow in the name colour.
        shadowOf(p, glyphs, withAlpha(m_color, 0.9), s * 0.42);
        p.fillPath(glyphs, withAlpha(m_color, 0.9));
        // 2. A hard drop shadow; 3. a navy outline (the dark rim that exempts
        //    chrome from the contrast floor).
        p.fillPath(glyphs.translated(2, 3), QColor(0, 0, 0, 140));
        p.strokePath(glyphs, QPen(QColor(0x0B, 0x10, 0x30), std::max(2.0, s / 16), Qt::SolidLine, Qt::RoundCap,
                                  Qt::RoundJoin));
        // 4. A steel ramp with a hard horizon at 50%.
        QLinearGradient steel(0, top, 0, bottom);
        steel.setColorAt(0, Qt::white);
        steel.setColorAt(0.36, Readability::lighten(m_color2, 0.72));
        steel.setColorAt(0.49, Readability::lighten(m_color2, 0.2));
        steel.setColorAt(0.5, Readability::darken(m_color2, 0.45));
        steel.setColorAt(0.62, m_color2);
        steel.setColorAt(0.86, Readability::lighten(m_color2, 0.8));
        steel.setColorAt(1, Readability::lighten(m_color, 0.6));
        p.fillPath(glyphs, steel);
        // 5. Two white glints with a cyan glow.
        const QColor cyan(160, 240, 255, 242);
        glint(p, QPointF(x + w * 0.12, top + s * 0.12), s * 0.18, cyan, 6);
        glint(p, QPointF(x + w * 0.83, top + s * 0.3), s * 0.13, cyan, 6);
        break;
    }
    }
}

QImage ProfileNameText::renderFrame(qreal dpr) const
{
    const QSizeF size(std::max(1.0, width()), std::max(1.0, height()));
    const bool live = m_phase != 0 && m_effect == int(NameEffect::GlitterName);
    if (!live) {
        const QString key = cacheKey(dpr);
        FrameCache &cache = frameCache();
        {
            QMutexLocker locker(&cache.mutex);
            if (const QImage *cached = cache.images.object(key))
                return *cached;
        }
        QImage frame = CosmeticPaint::transparentImage(size, dpr);
        {
            QPainter painter(&frame);
            paintFrame(painter, 0);
        }
        QMutexLocker locker(&cache.mutex);
        cache.images.insert(key, new QImage(frame), std::max<qsizetype>(1, frame.sizeInBytes() / 1024));
        return frame;
    }
    QImage frame = CosmeticPaint::transparentImage(size, dpr);
    QPainter painter(&frame);
    paintFrame(painter, m_phase);
    return frame;
}

int ProfileNameText::cachedFrameCount()
{
    FrameCache &cache = frameCache();
    QMutexLocker locker(&cache.mutex);
    return int(cache.images.count());
}

void ProfileNameText::clearFrameCache()
{
    FrameCache &cache = frameCache();
    QMutexLocker locker(&cache.mutex);
    cache.images.clear();
}

void ProfileNameText::paint(QPainter *painter)
{
    if (m_shown.isEmpty())
        return;
    if (m_phase != 0 && m_effect == int(NameEffect::GlitterName)) {
        paintFrame(*painter, m_phase); // animated frames are painted live, never cached
        return;
    }
    painter->drawImage(QPointF(0, 0), renderFrame(CosmeticPaint::deviceScale(*painter)));
}

} // namespace OpenChat
