#include "profile/ProfilePresetThumbItem.h"

#include "cosmetics/CosmeticPaint.h"
#include "domain/ProfilePage.h"
#include "profile/ProfileFonts.h"
#include "profile/ProfileMotifs.h"
#include "profile/ProfileReadability.h"

#include <QCache>
#include <QFontMetricsF>
#include <QLinearGradient>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace OpenChat {

namespace {

using Profile::HeaderStyle;
namespace Readability = ProfileReadability;

constexpr qreal cornerRadius = 5;
constexpr qreal backdropZoom = 0.55;

struct ThumbCache final {
    QMutex mutex;
    QCache<QString, QImage> images{2 * 1024}; // KB: ten presets in both modes at 2x
};

ThumbCache &thumbCache()
{
    static ThumbCache cache;
    return cache;
}

bool validPreset(int preset)
{
    return (preset >= int(Profile::Preset::AeroSkyPreset) && preset <= int(Profile::Preset::ChromeY2KPreset))
           || preset == int(Profile::Preset::CustomPreset);
}

QColor withAlpha(QColor colour, qreal alpha)
{
    colour.setAlphaF(float(std::clamp(alpha, 0.0, 1.0)));
    return colour;
}

// The mockups' single-background readability rule, 5% steps.
QColor held(const QColor &ink, const QColor &background, double floor)
{
    if (Readability::contrastRatio(ink, background) >= floor)
        return ink;
    const QColor target = Readability::relativeLuminance(background) > 0.18 ? QColor(Qt::black) : QColor(Qt::white);
    for (double t = 0.05; t <= 1.0; t += 0.05) {
        const QColor c = Readability::mix(ink, target, t);
        if (Readability::contrastRatio(c, background) >= floor)
            return c;
    }
    return target;
}

QPainterPath rounded(qreal x, qreal y, qreal w, qreal h, qreal r)
{
    return CosmeticPaint::roundedRect(QRectF(x, y, w, h), r);
}

} // namespace

ProfilePresetThumb::ProfilePresetThumb(QQuickItem *parent) : QQuickPaintedItem(parent)
{
    setAntialiasing(true);
    setImplicitSize(defaultWidth, defaultHeight);
}

void ProfilePresetThumb::setPreset(int preset)
{
    if (!validPreset(preset) || preset == m_preset)
        return;
    m_preset = preset;
    emit presetChanged();
    update();
}

void ProfilePresetThumb::setOwnerName(const QString &name)
{
    if (name == m_ownerName)
        return;
    m_ownerName = name;
    emit ownerNameChanged();
    update();
}

void ProfilePresetThumb::setDark(bool dark)
{
    if (dark == m_dark)
        return;
    m_dark = dark;
    emit darkChanged();
    update();
}

void ProfilePresetThumb::paintThumb(QPainter &c) const
{
    Profile::Theme t = Profile::presetTheme(Profile::Preset(m_preset));
    if (t.adaptive)
        t = Profile::aeroSkyTheme(m_dark);
    const qreal w = width();
    const qreal h = height();
    c.setRenderHint(QPainter::Antialiasing, true);
    c.save();
    c.setClipPath(rounded(0, 0, w, h, cornerRadius));

    // The backdrop, at small scale so a motif reads as a pattern.
    ProfileMotifs::BackdropSpec spec;
    spec.kind = t.backgroundKind;
    spec.color1 = Readability::rgb(t.backgroundColor1);
    spec.color2 = Readability::rgb(t.backgroundColor2);
    spec.motif = t.motif;
    spec.scale = Profile::MotifScale::SmallMotif;
    spec.ink = Readability::rgb(t.motifInk);
    spec.opacity = t.motifOpacity / 100.0;
    ProfileMotifs::paintBackdrop(c, QRectF(0, 0, w, h), spec, backdropZoom);

    const QColor boxColour = Readability::rgb(t.boxFill);
    const qreal opacity = t.boxOpacity / 100.0;
    const QColor fill = withAlpha(boxColour, opacity);
    const QColor strip = Readability::rgb(t.headerFill);
    const QColor altStrip = t.altHeader ? Readability::rgb(t.altHeaderFill) : strip;
    const QColor border = Readability::rgb(t.borderColor);
    const QColor altBorder = t.altHeader ? Readability::rgb(t.altBorderColor) : border;
    const qreal r = std::min(3.0, Profile::radiusPixels(t.boxRadius) / 2.0);
    const QColor behind = Readability::mix(spec.color1, boxColour, opacity);
    const QColor body = withAlpha(held(Readability::rgb(t.bodyColor), behind, 3), 0.55);
    const QColor link = withAlpha(held(Readability::rgb(t.linkColor), behind, 3), 0.8);

    const auto box = [&](qreal x, qreal y, qreal bw, qreal bh, bool withHeader, bool alt) {
        c.fillPath(rounded(x, y + 1, bw, bh, r), QColor(0, 0, 0, 46));
        c.fillPath(rounded(x, y, bw, bh, r), fill);
        if (withHeader && t.headerStyle != HeaderStyle::NoHeader) {
            c.save();
            c.setClipPath(rounded(x, y, bw, bh, r), Qt::IntersectClip);
            const QColor hc = alt ? altStrip : strip;
            if (t.headerStyle == HeaderStyle::GlossHeader || t.headerStyle == HeaderStyle::GradientHeader) {
                QLinearGradient g(0, y, 0, y + 7);
                g.setColorAt(0, Readability::lighten(hc, Readability::isDark(hc) ? 0.18 : 0.32));
                g.setColorAt(0.5, Readability::lighten(hc, 0.06));
                g.setColorAt(0.51, hc);
                g.setColorAt(1, Readability::darken(hc, 0.1));
                c.fillRect(QRectF(x, y, bw, 7), g);
            } else {
                c.fillRect(QRectF(x, y, bw, 7), hc);
            }
            c.restore();
        } else if (withHeader) {
            // Style "none": a title stroke and its rule.
            c.fillRect(QRectF(x + 4, y + 3, bw * 0.45, 2), withAlpha(Readability::rgb(t.headerText), 0.7));
            c.fillRect(QRectF(x + 4, y + 7, bw - 8, 1), withAlpha(alt ? altBorder : border, 0.8));
        }
        if (t.borderWidth > 0) {
            QPen pen(alt ? altBorder : border, std::min(1.5, qreal(t.borderWidth)));
            if (t.borderStyle == Profile::BorderStyle::DashedBorder)
                pen.setDashPattern({2 / pen.widthF(), 1.5 / pen.widthF()});
            else if (t.borderStyle == Profile::BorderStyle::DottedBorder)
                pen.setDashPattern({0.5 / pen.widthF(), 1.5 / pen.widthF()});
            c.strokePath(rounded(x + 0.5, y + 0.5, bw - 1, bh - 1, r), pen);
        }
    };
    const auto lines = [&](qreal x, qreal y, qreal lw, int n, const QColor &ink) {
        for (int i = 0; i < n; ++i)
            c.fillRect(QRectF(x, y + i * 5, lw * (i == n - 1 ? 0.6 : 1), 2), ink);
    };

    const qreal m = std::round(w * 0.07);
    const qreal colL = std::round((w - 2 * m - 4) * 0.41);
    const qreal colR = w - 2 * m - 4 - colL;
    const qreal top = std::round(h * 0.1);

    // The identity card with the owner's name in the preset's name face.
    box(m, top, colL, std::round(h * 0.52), false, false);
    QString family = ProfileFonts::family(t.nameFont, ProfileFonts::Role::Name);
    if (family.isEmpty())
        family = ProfileFonts::interfaceFamily();
    const int fontSize = std::max(1, int(std::lround(h * 0.13 * ProfileFonts::sizeFactor(t.nameFont,
                                                                                       ProfileFonts::Role::Name))));
    QFont font(family);
    font.setPixelSize(fontSize);
    font.setBold(ProfileFonts::useBold(t.nameFont, ProfileFonts::Role::Name));
    const QFontMetricsF metrics(font);
    const qreal nx = m + 4;
    const qreal ny = top + fontSize * 0.75; // the line's middle
    QPainterPath name;
    name.addText(QPointF(nx, ny + (metrics.ascent() - metrics.descent()) / 2), font, m_ownerName);
    const QColor nameColour = Readability::rgb(t.nameColor);
    const QColor nameColour2 = Readability::rgb(t.nameColor2);
    const auto nameEffect = t.nameEffect;
    const bool glows = nameEffect == Profile::NameEffect::GlowName || nameEffect == Profile::NameEffect::GlitterName
                       || nameEffect == Profile::NameEffect::ChromeName;
    if (glows) {
        const QColor glow = nameEffect == Profile::NameEffect::GlowName ? nameColour2 : nameColour;
        CosmeticPaint::drawBlurred(c, name.boundingRect(), 5, [&](QPainter &layer) { layer.fillPath(name, glow); });
    }
    if (nameEffect == Profile::NameEffect::ChromeName) {
        QLinearGradient steel(0, ny - fontSize / 2.0, 0, ny + fontSize / 2.0);
        steel.setColorAt(0, Qt::white);
        steel.setColorAt(0.5, QColor(0x5B, 0x6A, 0x82));
        steel.setColorAt(0.56, nameColour2);
        steel.setColorAt(1, Qt::white);
        c.fillPath(name, steel);
    } else {
        c.fillPath(name, held(nameColour, behind, 3));
    }
    if (nameEffect == Profile::NameEffect::ShadowName) {
        c.fillPath(name.translated(1, 1), nameColour2);
        c.fillPath(name, nameColour);
    }
    if (nameEffect == Profile::NameEffect::GlitterName) {
        c.fillPath(ProfileMotifs::sparklePath(QPointF(nx + fontSize * 1.6, ny - fontSize * 0.35), 2.5), Qt::white);
        c.fillPath(ProfileMotifs::sparklePath(QPointF(nx + fontSize * 0.4, ny - fontSize * 0.2), 2), Qt::white);
    }

    // A stand-in photo and the info lines.
    const qreal photo = std::round(colL * 0.36);
    const qreal photoTop = top + fontSize * 1.5;
    c.fillRect(QRectF(m + 4, photoTop, photo, photo), QColor(0x8A, 0xA4, 0xB8));
    c.setPen(Qt::NoPen);
    c.setBrush(QColor(0xC9, 0xA3, 0x8A));
    c.drawEllipse(QPointF(m + 4 + photo / 2, photoTop + photo * 0.42), photo * 0.22, photo * 0.22);
    c.fillRect(QRectF(m + 4 + photo * 0.18, photoTop + photo * 0.7, photo * 0.64, photo * 0.3), QColor(0x2F, 0x44, 0x58));
    lines(m + 8 + photo, photoTop + 2, colL - photo - 12, 3, body);

    // Contacting.
    const qreal contactTop = top + std::round(h * 0.52) + 4;
    box(m, contactTop, colL, h - contactTop + 6, true, false);
    lines(m + 4, contactTop + 11, colL * 0.35, 2, link);
    lines(m + 4 + colL * 0.5, contactTop + 11, colL * 0.35, 2, link);

    // The wide column: banner, blurbs, Friend Space.
    const qreal rx = m + colL + 4;
    box(rx, top, colR, std::round(h * 0.14), false, false);
    lines(rx + 5, top + 4, colR * 0.55, 1, body);
    const qreal blurbsTop = top + std::round(h * 0.14) + 4;
    box(rx, blurbsTop, colR, std::round(h * 0.3), true, true);
    lines(rx + 5, blurbsTop + 11, colR - 10, 3, body);
    const qreal friendsTop = blurbsTop + std::round(h * 0.3) + 4;
    box(rx, friendsTop, colR, h - friendsTop + 6, true, true);
    const qreal tile = (colR - 10 - 9) / 4;
    const QColor tiles[] = {QColor(0x7E, 0x97, 0xAB), QColor(0xB6, 0x94, 0x82), QColor(0x8F, 0x9A, 0xA4),
                            Readability::darken(border, 0.2)};
    for (int i = 0; i < 4; ++i)
        c.fillRect(QRectF(rx + 5 + i * (tile + 3), friendsTop + 11, tile, tile), tiles[i]);
    c.restore();
}

QImage ProfilePresetThumb::render(qreal dpr) const
{
    const QString key = QStringLiteral("%1|%2|%3|%4x%5@%6")
                            .arg(m_preset)
                            .arg(m_ownerName)
                            .arg(int(m_dark))
                            .arg(width())
                            .arg(height())
                            .arg(dpr, 0, 'f', 3);
    ThumbCache &cache = thumbCache();
    {
        QMutexLocker locker(&cache.mutex);
        if (const QImage *cached = cache.images.object(key))
            return *cached;
    }
    QImage image = CosmeticPaint::transparentImage(QSizeF(std::max(1.0, width()), std::max(1.0, height())), dpr);
    {
        QPainter painter(&image);
        paintThumb(painter);
    }
    QMutexLocker locker(&cache.mutex);
    cache.images.insert(key, new QImage(image), std::max<qsizetype>(1, image.sizeInBytes() / 1024));
    return image;
}

int ProfilePresetThumb::cachedCount()
{
    ThumbCache &cache = thumbCache();
    QMutexLocker locker(&cache.mutex);
    return int(cache.images.count());
}

void ProfilePresetThumb::paint(QPainter *painter)
{
    if (width() <= 0 || height() <= 0)
        return;
    painter->drawImage(QPointF(0, 0), render(CosmeticPaint::deviceScale(*painter)));
}

} // namespace OpenChat
