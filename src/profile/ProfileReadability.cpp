#include "profile/ProfileReadability.h"

#include "profile/ProfileMotifs.h"

#include <algorithm>
#include <cmath>

namespace OpenChat::ProfileReadability {

namespace {

using Profile::HeaderStyle;
using Profile::InkRole;

const QColor white(255, 255, 255);
const QColor black(0, 0, 0);

// JavaScript's Math.round (halves go up), which the reference rounds with.
int roundHalfUp(double value)
{
    return int(std::floor(value + 0.5));
}

double linearChannel(int value)
{
    const double c = value / 255.0;
    return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

QColor withAlpha(QColor colour, qreal alpha)
{
    colour.setAlphaF(float(std::clamp(alpha, 0.0, 1.0)));
    return colour;
}

double minContrast(const QColor &fg, const QVector<QColor> &backgrounds)
{
    double worst = 99;
    for (const QColor &bg : backgrounds)
        worst = std::min(worst, contrastRatio(fg, bg));
    return worst;
}

QVector<QColor> stopList(const StripRecipe &recipe)
{
    return {recipe.stops[0], recipe.stops[1], recipe.stops[2], recipe.stops[3]};
}

// Moves `fg` towards white or black until it clears `floor` against every
// background, keeping whichever pole passes with the smaller shift (both
// passing at one step: the one with more total contrast). When nothing
// passes, the pole with the best worst case, and `failed` is set: the last
// resort (a halo) is the caller's.
QColor ensureAll(const QColor &fg, const QVector<QColor> &backgrounds, double floor, double step, double bound,
                 bool *failed = nullptr)
{
    const auto ok = [&](const QColor &c) { return minContrast(c, backgrounds) >= floor; };
    if (failed)
        *failed = false;
    if (ok(fg))
        return fg;
    for (double t = step; t <= bound; t += step) {
        const QColor w = mix(fg, white, t);
        const QColor k = mix(fg, black, t);
        const bool okw = ok(w);
        const bool okk = ok(k);
        if (okw && okk) {
            double sw = 0;
            double sk = 0;
            for (const QColor &bg : backgrounds) {
                sw += contrastRatio(w, bg);
                sk += contrastRatio(k, bg);
            }
            return sw >= sk ? w : k;
        }
        if (okw)
            return w;
        if (okk)
            return k;
    }
    if (failed)
        *failed = true;
    return minContrast(white, backgrounds) >= minContrast(black, backgrounds) ? white : black;
}

// The reference's ensureN: 2% steps.
QColor ensureN(const QColor &fg, const QVector<QColor> &backgrounds, double floor, bool *failed = nullptr)
{
    return ensureAll(fg, backgrounds, floor, 0.02, 1.0001, failed);
}

// The reference's single-background rule (table cells): 5% steps towards the
// pole on the far side of the background.
QColor ensureOne(const QColor &fg, const QColor &bg, double floor)
{
    if (contrastRatio(fg, bg) >= floor)
        return fg;
    const QColor target = relativeLuminance(bg) > 0.18 ? black : white;
    for (double t = 0.05; t <= 1.0; t += 0.05) {
        const QColor c = mix(fg, target, t);
        if (contrastRatio(c, bg) >= floor)
            return c;
    }
    return target;
}

QString inkWord(InkRole role)
{
    switch (role) {
    case InkRole::BodyInk:
        return QStringLiteral("text");
    case InkRole::LabelInk:
        return QStringLiteral("label");
    case InkRole::LinkInk:
        return QStringLiteral("link");
    case InkRole::NameInk:
        return QStringLiteral("name");
    case InkRole::HeaderTextInk:
    case InkRole::AltHeaderTextInk:
        return QStringLiteral("strip text");
    }
    return QStringLiteral("text");
}

QString shiftSentence(InkRole role, const QColor &from, const QColor &to, const QString &where)
{
    const QString shade = relativeLuminance(to) < relativeLuminance(from) ? QStringLiteral("darker")
                                                                          : QStringLiteral("lighter");
    return QStringLiteral("Your %1 colour is faint on %2, so contacts see it a shade %3. "
                          "Pick a stronger colour to keep it exact.")
        .arg(inkWord(role), where, shade);
}

struct StripResult final {
    StripRecipe recipe;
    QColor text;
};

} // namespace

double relativeLuminance(const QColor &c)
{
    return 0.2126 * linearChannel(c.red()) + 0.7152 * linearChannel(c.green()) + 0.0722 * linearChannel(c.blue());
}

double contrastRatio(const QColor &a, const QColor &b)
{
    const double la = relativeLuminance(a);
    const double lb = relativeLuminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

QColor mix(const QColor &a, const QColor &b, qreal t)
{
    const auto channel = [t](int x, int y) { return std::clamp(roundHalfUp(x + (y - x) * t), 0, 255); };
    return QColor(channel(a.red(), b.red()), channel(a.green(), b.green()), channel(a.blue(), b.blue()));
}

QColor over(const QColor &top, const QColor &bottom)
{
    return mix(bottom, QColor(top.rgb()), top.alphaF());
}

QColor lighten(const QColor &c, qreal t)
{
    return mix(c, white, t);
}

QColor darken(const QColor &c, qreal t)
{
    return mix(c, black, t);
}

bool isDark(const QColor &c)
{
    return relativeLuminance(c) < 0.18;
}

QColor rgb(quint32 value)
{
    return QColor::fromRgb(QRgb(0xFF000000U | (value & 0xFFFFFFU)));
}

QVector<QColor> pageSamples(const Profile::Theme &theme, const std::optional<ImageStats> &image)
{
    using Profile::BackgroundKind;
    const QColor c1 = rgb(theme.backgroundColor1);
    const QColor c2 = rgb(theme.backgroundColor2);
    QVector<QColor> samples{c1};
    const bool gradient = (theme.backgroundKind == BackgroundKind::GradientBackground
                           || theme.backgroundKind == BackgroundKind::PatternBackground)
                          && c2 != c1;
    if (gradient) {
        samples.push_back(c2);
        samples.push_back(mix(c1, c2, 0.5));
    }
    if (theme.backgroundKind == BackgroundKind::PatternBackground) {
        const QColor ink = rgb(theme.motifInk);
        const qreal opacity = theme.motifOpacity / 100.0;
        samples.push_back(mix(c1, ink, opacity));
        if (gradient)
            samples.push_back(mix(c2, ink, opacity));
        for (const ProfileMotifs::HighlightInk &high : ProfileMotifs::highlightInks(theme.motif, ink, opacity))
            samples.push_back(mix(c1, high.color, high.opacity));
    }
    // A picture sits on its first colour; until it is decoded only that counts.
    if (theme.backgroundKind == BackgroundKind::ImageBackground && image) {
        samples.push_back(QColor(image->darkest.rgb()));
        samples.push_back(QColor(image->lightest.rgb()));
    }
    return samples;
}

StripRecipe stripRecipe(const QColor &fillColour, HeaderStyle style)
{
    const QColor fill(fillColour.rgb());
    StripRecipe recipe;
    recipe.dark = isDark(fill);
    const bool dark = recipe.dark;
    switch (style) {
    case HeaderStyle::GlossHeader:
        recipe.stops = {lighten(fill, dark ? 0.12 : 0.34), lighten(fill, dark ? 0.05 : 0.16), fill,
                        dark ? darken(fill, 0.12) : darken(fill, 0.06)};
        recipe.highlight = withAlpha(white, dark ? 0.22 : 0.69);
        break;
    case HeaderStyle::GradientHeader: {
        const QColor top = lighten(fill, dark ? 0.10 : 0.20);
        const QColor bottom = darken(fill, 0.10);
        const QColor middle = mix(top, bottom, 0.5);
        recipe.stops = {top, middle, middle, bottom};
        recipe.highlight = withAlpha(white, dark ? 0.13 : 0.44);
        break;
    }
    case HeaderStyle::FlatHeader:
    case HeaderStyle::NoHeader:
        recipe.stops = {fill, fill, fill, fill};
        recipe.highlight = QColor(255, 255, 255, 0);
        break;
    }
    recipe.bottomLine = darken(recipe.stops[3], dark ? 0.35 : 0.14);
    return recipe;
}

Palette resolve(const Profile::Theme &theme, const QVector<QColor> &samplesIn, const Floors &floors)
{
    const QVector<QColor> samples = samplesIn.isEmpty() ? QVector<QColor>{rgb(theme.backgroundColor1)} : samplesIn;
    Palette out;
    const QColor fill = rgb(theme.boxFill);
    const QColor ownerBody = rgb(theme.bodyColor);
    const QColor ownerLabel = rgb(theme.labelColor);
    const QColor ownerLink = rgb(theme.linkColor);
    const QColor ownerName = rgb(theme.nameColor);
    const bool chrome = theme.nameEffect == Profile::NameEffect::ChromeName;

    const auto boxesAt = [&](int percent, const QVector<QColor> &behind) {
        QVector<QColor> boxes;
        boxes.reserve(behind.size());
        for (const QColor &sample : behind)
            boxes.push_back(mix(sample, fill, percent / 100.0));
        return boxes;
    };
    const auto inksPass = [&](const QVector<QColor> &boxes) {
        return minContrast(ownerBody, boxes) >= floors.body && minContrast(ownerLabel, boxes) >= floors.body
               && minContrast(ownerLink, boxes) >= floors.body
               && (chrome || minContrast(ownerName, boxes) >= floors.name);
    };

    // 1. Raise the box opacity (2% steps) until the owner's own inks pass:
    //    this fixes bleed-through without touching anyone's colours.
    const int chosen = std::clamp(int(theme.boxOpacity), 0, 100);
    int percent = chosen;
    while (percent < 100 && !inksPass(boxesAt(percent, samples)))
        percent = std::min(100, percent + 2);
    if (percent > chosen) {
        // Blame the background when the box over the plain base colour would
        // have kept every ink that can be readable at all readable: then only
        // the pattern, gradient or picture made it fail. (An ink too faint
        // even on a solid box forces 100% by itself; the Text tab tells the
        // owner about that one.)
        const QVector<QColor> solid{fill};
        const QVector<QColor> overBase = boxesAt(chosen, {samples.first()});
        const auto keeps = [&](const QColor &ink, double floor) {
            return minContrast(ink, solid) < floor || minContrast(ink, overBase) >= floor;
        };
        const bool baseAlonePasses = keeps(ownerBody, floors.body) && keeps(ownerLabel, floors.body)
                                     && keeps(ownerLink, floors.body) && (chrome || keeps(ownerName, floors.name));
        out.adjustments.push_back(
            {baseAlonePasses ? Profile::EditorTab::BackgroundTab : Profile::EditorTab::BoxesTab, -1,
             QStringLiteral("Boxes are drawn %1% solid (you chose %2%) so text stays clear over this background.")
                 .arg(percent)
                 .arg(chosen)});
    }
    out.resolvedOpacity = percent;
    out.boxFill = fill;
    out.boxFill.setAlpha(roundHalfUp(percent * 255.0 / 100.0));
    const QVector<QColor> boxes = boxesAt(percent, samples);
    out.boxBackgrounds = boxes;
    const QColor box = boxes.first();
    out.boxDark = isDark(box);
    const bool dark = out.boxDark;

    // An ink no single colour can make readable is kept at its best and
    // gets the last-resort halo (step 4).
    bool anyFailed = false;
    QColor failedInk;
    const auto noteFailure = [&](bool failed, const QColor &shown) {
        if (failed && !anyFailed) {
            anyFailed = true;
            failedInk = shown;
        }
    };

    // 2. Move each ink that still fails towards black or white.
    const auto fixInk = [&](InkRole role, const QColor &owner, double floor, Profile::EditorTab tab) {
        bool failed = false;
        const QColor shown = ensureN(owner, boxes, floor, &failed);
        if (shown != owner) {
            out.adjustments.push_back({tab, int(role), shiftSentence(role, owner, shown, QStringLiteral("your boxes"))});
            out.inkAdjusted.insert(int(role), true);
        }
        noteFailure(failed, shown);
        return shown;
    };
    out.body = fixInk(InkRole::BodyInk, ownerBody, floors.body, Profile::EditorTab::TextTab);
    out.label = fixInk(InkRole::LabelInk, ownerLabel, floors.body, Profile::EditorTab::TextTab);
    out.link = fixInk(InkRole::LinkInk, ownerLink, floors.body, Profile::EditorTab::TextTab);
    out.name = chrome ? ownerName : fixInk(InkRole::NameInk, ownerName, floors.name, Profile::EditorTab::NameTab);
    out.name2 = rgb(theme.nameColor2);
    out.muted = ensureN(mix(out.body, box, 0.32), boxes, floors.muted);

    // 3. Strips: the renderer owns the strip fill, so it moves first (4%
    //    steps, up to 48%); only then does the owner's title colour shift.
    const auto resolveStrip = [&](const QColor &ownerFill, const QColor &ownerText, InkRole role, bool alt) {
        StripResult result;
        bool failed = false;
        if (theme.headerStyle == HeaderStyle::NoHeader) {
            result.recipe = stripRecipe(ownerFill, HeaderStyle::NoHeader);
            result.text = ensureN(ownerText, boxes, floors.strip, &failed);
            noteFailure(failed, result.text);
            if (result.text != ownerText) {
                out.adjustments.push_back({Profile::EditorTab::BoxesTab, int(role),
                                           shiftSentence(role, ownerText, result.text, QStringLiteral("your boxes"))});
                out.inkAdjusted.insert(int(role), true);
            }
            return result;
        }
        QColor stripFill = ownerFill;
        result.recipe = stripRecipe(stripFill, theme.headerStyle);
        const bool lightText = relativeLuminance(ownerText) > relativeLuminance(ownerFill);
        for (int k = 0; k < 12 && minContrast(ownerText, stopList(result.recipe)) < floors.strip; ++k) {
            stripFill = lightText ? darken(ownerFill, 0.04 * (k + 1)) : lighten(ownerFill, 0.04 * (k + 1));
            result.recipe = stripRecipe(stripFill, theme.headerStyle);
        }
        if (stripFill != ownerFill) {
            out.adjustments.push_back(
                {Profile::EditorTab::BoxesTab, -1,
                 QStringLiteral("%1 strip %2 slightly so its title stays readable.")
                     .arg(alt ? QStringLiteral("Right-column header") : QStringLiteral("Header"),
                          lightText ? QStringLiteral("deepened") : QStringLiteral("lightened"))});
        }
        // A glossy strip spans a wide range; a title that fits none of it
        // even at 48% keeps its best pole and is haloed.
        result.text = ensureN(ownerText, stopList(result.recipe), floors.strip, &failed);
        noteFailure(failed, result.text);
        if (result.text != ownerText) {
            out.adjustments.push_back({Profile::EditorTab::BoxesTab, int(role),
                                       shiftSentence(role, ownerText, result.text, QStringLiteral("its strip"))});
            out.inkAdjusted.insert(int(role), true);
        }
        return result;
    };
    const StripResult main = resolveStrip(rgb(theme.headerFill), rgb(theme.headerText), InkRole::HeaderTextInk, false);
    out.strip = main.recipe;
    out.headerText = main.text;
    if (theme.altHeader) {
        const StripResult alt =
            resolveStrip(rgb(theme.altHeaderFill), rgb(theme.altHeaderText), InkRole::AltHeaderTextInk, true);
        out.altStrip = alt.recipe;
        out.altHeaderText = alt.text;
        // Classic '06's brown-orange sub-heads: the alt strip's text colour,
        // held against the boxes.
        out.blurbSubhead = ensureN(rgb(theme.altHeaderText), boxes, floors.body);
    } else {
        out.altStrip = out.strip;
        out.altHeaderText = out.headerText;
        out.blurbSubhead = out.label;
    }

    // Tables: the cells are tinted from the box towards its accent (the
    // border, or the strip when there is no border); each ink is held against
    // its own cell.
    const QColor accent = theme.borderWidth > 0 ? rgb(theme.borderColor) : rgb(theme.headerFill);
    out.cellLabelFill = mix(box, accent, dark ? 0.20 : 0.40);
    out.cellValueFill = mix(box, accent, dark ? 0.09 : 0.18);
    out.cellLabelInk = ensureOne(out.label, out.cellLabelFill, floors.cell);
    out.cellValueInk = ensureOne(out.body, out.cellValueFill, floors.cell);
    out.tableRule = withAlpha(out.label, dark ? 0.24 : 0.18);

    // Presence inks are renderer constants for light or dark boxes (SPEC §5.1).
    static const std::array<QColor, 4> lightPresence{QColor(0x2E, 0x7D, 0x32), QColor(0x8A, 0x5A, 0x00),
                                                     QColor(0xB3, 0x26, 0x1E), QColor(0x5F, 0x6B, 0x78)};
    static const std::array<QColor, 4> darkPresence{QColor(0x7D, 0xDC, 0x6A), QColor(0xFF, 0xC9, 0x4D),
                                                    QColor(0xFF, 0x8A, 0x7A), QColor(0xA9, 0xB4, 0xBF)};
    for (std::size_t i = 0; i < out.presence.size(); ++i)
        out.presence[i] = ensureN(dark ? darkPresence[i] : lightPresence[i], boxes, floors.presence);

    // Monogram tiles (SPEC §5.8): quiet glass tinted from the box towards the
    // box's own border; initials in the label ink a quarter of the way to
    // the tile, held at 3:1 against both of its stops.
    const auto monogram = [&](const QColor &tint, QColor &top, QColor &bottom, QColor &rim, QColor &ink) {
        top = mix(box, tint, dark ? 0.12 : 0.10);
        bottom = mix(box, tint, dark ? 0.26 : 0.34);
        rim = mix(box, tint, dark ? 0.50 : 0.55);
        ink = ensureAll(mix(out.label, mix(top, bottom, 0.5), 0.25), {top, bottom}, floors.ornament, 0.05, 1.0);
    };
    monogram(accent, out.monogramTop, out.monogramBottom, out.monogramRim, out.monogramInk);
    const QColor altAccent = !theme.altHeader       ? accent
                             : theme.borderWidth > 0 ? rgb(theme.altBorderColor)
                                                     : rgb(theme.altHeaderFill);
    monogram(altAccent, out.altMonogramTop, out.altMonogramBottom, out.altMonogramRim, out.altMonogramInk);

    out.songMaterial = dark ? 1 : 0;
    // Ambient sprites are white; over a light base they get a faint outline
    // in the link ink so they do not vanish (SPEC §8.2).
    out.ambientOutline = relativeLuminance(rgb(theme.backgroundColor1)) > 0.5 ? withAlpha(out.link, 0.4)
                                                                              : QColor(0, 0, 0, 0);

    // 4. Last resort: no single colour passes every sample. Keep the best one
    //    and give the text a 1 px halo in the opposite pole.
    if (anyFailed) {
        out.halo = true;
        out.haloColor = withAlpha(relativeLuminance(failedInk) > 0.5 ? black : white, 0.7);
        out.adjustments.push_back({Profile::EditorTab::BackgroundTab, -1,
                                   QStringLiteral("Some text gets a thin outline so it stays readable over "
                                                  "this background.")});
    } else {
        out.haloColor = QColor(0, 0, 0, 0);
    }
    out.adjusted = !out.adjustments.isEmpty();
    return out;
}

ContrastReport contrastFor(const Profile::Theme &theme, const QVector<QColor> &samples, InkRole role,
                           const QColor &candidate)
{
    Profile::Theme trial = theme;
    const quint32 value = candidate.rgb() & 0xFFFFFFU;
    switch (role) {
    case InkRole::BodyInk:
        trial.bodyColor = value;
        break;
    case InkRole::LabelInk:
        trial.labelColor = value;
        break;
    case InkRole::LinkInk:
        trial.linkColor = value;
        break;
    case InkRole::NameInk:
        trial.nameColor = value;
        break;
    case InkRole::HeaderTextInk:
        trial.headerText = value;
        break;
    case InkRole::AltHeaderTextInk:
        trial.altHeaderText = value;
        break;
    }
    const Palette palette = resolve(trial, samples);
    const QColor picked(candidate.rgb());
    ContrastReport report;
    const Floors floors;
    switch (role) {
    case InkRole::BodyInk:
    case InkRole::LabelInk:
    case InkRole::LinkInk:
        report.ratio = minContrast(picked, palette.boxBackgrounds);
        report.passes = report.ratio >= floors.body;
        report.shown = role == InkRole::BodyInk ? palette.body : role == InkRole::LabelInk ? palette.label : palette.link;
        break;
    case InkRole::NameInk:
        report.ratio = minContrast(picked, palette.boxBackgrounds);
        report.passes = trial.nameEffect == Profile::NameEffect::ChromeName || report.ratio >= floors.name;
        report.shown = palette.name;
        break;
    case InkRole::HeaderTextInk:
    case InkRole::AltHeaderTextInk: {
        const bool alt = role == InkRole::AltHeaderTextInk && trial.altHeader;
        // Measured on the owner's own strip: a deepened strip still means the
        // pick was too faint for it.
        const QColor ownerFill = rgb(alt ? trial.altHeaderFill : trial.headerFill);
        report.ratio = trial.headerStyle == HeaderStyle::NoHeader
                           ? minContrast(picked, palette.boxBackgrounds)
                           : minContrast(picked, stopList(stripRecipe(ownerFill, trial.headerStyle)));
        report.passes = report.ratio >= floors.strip;
        report.shown = alt ? palette.altHeaderText : palette.headerText;
        break;
    }
    }
    return report;
}

} // namespace OpenChat::ProfileReadability
