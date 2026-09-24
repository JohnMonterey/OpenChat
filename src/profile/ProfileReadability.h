#pragma once

#include "domain/ProfilePage.h"
#include "profile/ProfileMediaStore.h"

#include <QColor>
#include <QHash>
#include <QString>
#include <QVector>

#include <array>
#include <optional>

// The profile renderer's contrast engine (SPEC §9). A page's colours are the
// owner's; readability is the renderer's. Every viewer runs the same
// deterministic pass over the owner's theme and what sits behind the boxes,
// so everyone sees the same corrected page while the wire keeps the owner's
// values. The reference implementation is the design mockups' Color.js
// (resolve()); this is a faithful port of it: sRGB mixing rounded to 8-bit
// channels at every step, WCAG 2.x relative luminance, the same step sizes,
// so the preset table in SPEC §10 comes out to the hundredth.
namespace OpenChat::ProfileReadability {

struct Floors final {
    double body = 4.5;
    double muted = 4.5;
    double cell = 4.5;
    double strip = 4.5;
    double name = 3.0;
    double ornament = 3.0; // monogram initials against their tile
    double presence = 4.5;
};

// A header strip's fill as the renderer draws it (SPEC §4.2): four stops at
// fixed positions (gloss has its hard glass edge at 0.50), the top highlight
// line (transparent when the style has none) and the strip's bottom line.
struct StripRecipe final {
    std::array<QColor, 4> stops;
    std::array<qreal, 4> positions{0.0, 0.49, 0.50, 1.0};
    QColor highlight;
    QColor bottomLine;
    bool dark = false;
};

// One thing the renderer changed, told to the owner in the editor tab it came
// from. `role` is a Profile::InkRole, or -1 for the box opacity, a strip fill
// or the halo.
struct Adjustment final {
    Profile::EditorTab tab = Profile::EditorTab::TextTab;
    int role = -1;
    QString sentence;
};

struct Palette final {
    QColor boxFill; // the owner's box colour, alpha = the resolved opacity
    int resolvedOpacity = 100; // percent
    bool boxDark = false;      // the box over the base colour is darker than luminance 0.18
    // The box composited at the resolved opacity over every page sample: what
    // box text is measured against.
    QVector<QColor> boxBackgrounds;
    StripRecipe strip, altStrip;
    QColor headerText, altHeaderText;
    QColor body, label, link, muted, blurbSubhead;
    QColor cellLabelFill, cellValueFill, cellLabelInk, cellValueInk, tableRule;
    QColor name, name2;
    std::array<QColor, 4> presence; // Available, Away, Busy, Offline
    QColor monogramTop, monogramBottom, monogramRim, monogramInk;         // main column
    QColor altMonogramTop, altMonogramBottom, altMonogramRim, altMonogramInk; // alt (wide) boxes
    int songMaterial = 0; // 0 silver glass, 1 smoked glass
    QColor ambientOutline; // transparent unless the base is light
    bool halo = false;
    QColor haloColor;
    bool adjusted = false;
    QVector<Adjustment> adjustments;
    QHash<int, bool> inkAdjusted; // Profile::InkRole → shown differently from the owner's colour
};

struct ContrastReport final {
    double ratio = 0; // the candidate as picked, worst case over its backgrounds
    bool passes = false;
    QColor shown; // what viewers see after the renderer's pass
};

// sRGB, WCAG 2.x.
[[nodiscard]] double relativeLuminance(const QColor &c);
[[nodiscard]] double contrastRatio(const QColor &a, const QColor &b);
// `top` (with its alpha) composited over an opaque `bottom`.
[[nodiscard]] QColor over(const QColor &top, const QColor &bottom);
// t = the weight of b; channels rounded to 8 bits, the result opaque.
[[nodiscard]] QColor mix(const QColor &a, const QColor &b, qreal t);
[[nodiscard]] QColor lighten(const QColor &c, qreal t);
[[nodiscard]] QColor darken(const QColor &c, qreal t);
// luminance < 0.18: the threshold every "dark box / dark strip" rule uses.
[[nodiscard]] bool isDark(const QColor &c);
[[nodiscard]] QColor rgb(quint32 value); // 0xRRGGBB

// What sits behind a box: the base colour(s), for a pattern the motif ink as
// drawn (premixed at the pattern strength) and its declared highlight inks,
// for a picture its darkest and lightest tile extremes (only once decoded).
[[nodiscard]] QVector<QColor> pageSamples(const Profile::Theme &theme, const std::optional<ImageStats> &image);
[[nodiscard]] StripRecipe stripRecipe(const QColor &fill, Profile::HeaderStyle style);
// The readability pass over an already substituted theme (adaptive and plain
// style are the caller's business). `samples` must not be empty.
[[nodiscard]] Palette resolve(const Profile::Theme &theme, const QVector<QColor> &samples, const Floors &floors = {});
// The colour picker's readability line: `candidate` in `role`, measured
// against what it will sit on, and what the page will really show.
[[nodiscard]] ContrastReport contrastFor(const Profile::Theme &theme, const QVector<QColor> &samples,
                                         Profile::InkRole role, const QColor &candidate);

} // namespace OpenChat::ProfileReadability
