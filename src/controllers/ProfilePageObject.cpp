#include "controllers/ProfilePageObject.h"

#include "profile/ProfileFonts.h"
#include "profile/ProfileMediaStore.h"
#include "profile/ProfileReadability.h"

#include <QStringList>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace OpenChat {

using Profile::Theme;

namespace {

// SPEC §4.3's type scale, indexed by Profile::TextSize (before face factors).
constexpr std::array<int, 3> bodySizes{12, 13, 15};    // body, labels, table values
constexpr std::array<int, 3> headingSizes{13, 14, 16}; // strip titles, sub-heads
constexpr std::array<int, 3> captionSizes{11, 11, 12};
constexpr std::array<int, 3> stripHeights{28, 30, 34};

// Every Theme colour an adaptive page takes from Aero Sky (ARCH §2.5).
constexpr std::array<quint32 Theme::*, 15> aeroSkyColourFields{
    &Theme::backgroundColor1, &Theme::backgroundColor2, &Theme::motifInk,      &Theme::boxFill,
    &Theme::borderColor,      &Theme::headerFill,       &Theme::headerText,    &Theme::altHeaderFill,
    &Theme::altHeaderText,    &Theme::altBorderColor,   &Theme::bodyColor,     &Theme::labelColor,
    &Theme::linkColor,        &Theme::nameColor,        &Theme::nameColor2};

[[nodiscard]] QColor colourOf(quint32 rgb)
{
    return ProfileReadability::rgb(rgb);
}

[[nodiscard]] quint32 rgbOf(const QColor &color)
{
    return color.isValid() ? (quint32(color.rgb()) & 0xFFFFFFU) : 0U;
}

[[nodiscard]] int sizeIndex(Profile::TextSize size)
{
    return std::clamp(int(size), 0, 2);
}

[[nodiscard]] int scaled(int pixels, qreal factor)
{
    return int(std::lround(pixels * factor));
}

// The name's shrink floor exactly as ProfileNameText works it out (a
// minimum: on the Pixel grid it rounds up, never above the base), so the page
// and the item agree on it.
[[nodiscard]] int nameFloor(Profile::Font font, Profile::NameEffect effect, int base)
{
    const int grid = ProfileFonts::pixelGrid(font);
    const auto snapUp = [grid](int px) { return grid > 0 ? (px + grid - 1) / grid * grid : px; };
    const bool fancy = effect == Profile::NameEffect::GlitterName || font == Profile::Font::ScriptFont
                       || font == Profile::Font::GothicFont;
    int floorSize = fancy ? 30 : std::max(20, int(std::lround(0.7 * base)));
    if (fancy)
        base = std::max(base, snapUp(30));
    return std::min(snapUp(floorSize), base);
}

[[nodiscard]] QVariantList colourList(const std::array<QColor, 4> &colours)
{
    QVariantList list;
    for (const QColor &colour : colours)
        list.append(colour);
    return list;
}

[[nodiscard]] QVariantList realList(const std::array<qreal, 4> &values)
{
    QVariantList list;
    for (const qreal value : values)
        list.append(value);
    return list;
}

[[nodiscard]] bool filled(const QString &text)
{
    return !text.trimmed().isEmpty();
}

[[nodiscard]] QVariantMap row(const QString &label, const QString &value)
{
    return {{QStringLiteral("label"), label}, {QStringLiteral("value"), value}};
}

template<typename Enum>
[[nodiscard]] bool inRange(int value, Enum last)
{
    return value >= 0 && value <= int(last);
}

// The first code point of `word` (never half a surrogate pair).
[[nodiscard]] QString firstLetter(const QString &word)
{
    if (word.isEmpty())
        return {};
    return word.size() > 1 && word.at(0).isHighSurrogate() ? word.left(2) : word.left(1);
}

} // namespace

// ---------------------------------------------------------------------------
// ProfileRenderStyle
// ---------------------------------------------------------------------------

ProfileRenderStyle::ProfileRenderStyle(QObject *parent)
    : QObject(parent)
{
}

Theme ProfileRenderStyle::withAeroSkyColours(Theme theme, bool dark)
{
    const Theme aero = Profile::aeroSkyTheme(dark);
    for (quint32 Theme::*field : aeroSkyColourFields)
        theme.*field = aero.*field;
    theme.boxOpacity = aero.boxOpacity;
    return theme;
}

Theme ProfileRenderStyle::viewedTheme(const Theme &theme, const Viewer &viewer)
{
    // Plain style is Aero Sky in full: its own backdrop, Standard fonts, its
    // name style, no flourish, picture or ambient (ARCH §2.5, final-plain.png).
    if (viewer.plain)
        return Profile::aeroSkyTheme(viewer.dark);
    if (theme.adaptive)
        return withAeroSkyColours(theme, viewer.dark);
    return theme;
}

ProfileRenderStyle::Values ProfileRenderStyle::compute(const Theme &owner, const Viewer &viewer,
                                                       const QString &imageKey)
{
    using ProfileFonts::Role;
    const Theme theme = viewedTheme(owner, viewer);
    const bool showsImage = !viewer.plain && theme.backgroundKind == Profile::BackgroundKind::ImageBackground;
    const QString key = showsImage ? imageKey : QString();
    // A picture joins the samples once it is decoded (its tile extremes).
    const std::optional<ImageStats> stats =
        key.isEmpty() ? std::nullopt : ProfileMediaStore::instance().stats(key);
    const ProfileReadability::Palette palette =
        ProfileReadability::resolve(theme, ProfileReadability::pageSamples(theme, stats));

    Values v;
    v.plain = viewer.plain;
    v.backgroundKind = int(theme.backgroundKind);
    v.color1 = colourOf(theme.backgroundColor1);
    v.color2 = colourOf(theme.backgroundColor2);
    v.motif = int(theme.motif);
    v.motifInk = colourOf(theme.motifInk);
    v.motifOpacity = theme.motifOpacity / 100.0;
    v.motifScale = int(theme.motifScale);
    v.imageKey = key;
    v.imageMode = int(theme.imageMode);
    v.imageFixed = theme.imageFixed;

    v.boxFill = palette.boxFill;
    v.boxDark = palette.boxDark;
    v.borderColor = colourOf(theme.borderColor);
    v.altBorderColor = colourOf(theme.altHeader ? theme.altBorderColor : theme.borderColor);
    v.borderWidth = theme.borderWidth;
    v.borderStyle = theme.borderStyle == Profile::BorderStyle::DoubleBorder && theme.borderWidth < 3
                        ? int(Profile::BorderStyle::SolidBorder)
                        : int(theme.borderStyle);
    v.radiusPx = Profile::radiusPixels(theme.boxRadius);
    v.boxGlow = theme.boxGlow;
    v.tableStyle = int(theme.tableStyle);

    v.headerStyle = int(theme.headerStyle);
    v.stripStops = colourList(palette.strip.stops);
    v.stripPositions = realList(palette.strip.positions);
    v.stripHighlight = palette.strip.highlight;
    v.stripBottomLine = palette.strip.bottomLine;
    v.stripDark = palette.strip.dark;
    v.altStripStops = colourList(palette.altStrip.stops);
    v.altStripPositions = realList(palette.altStrip.positions);
    v.altStripHighlight = palette.altStrip.highlight;
    v.altStripBottomLine = palette.altStrip.bottomLine;
    v.altStripDark = palette.altStrip.dark;
    v.altHeader = theme.altHeader;
    v.headerText = palette.headerText;
    v.altHeaderText = palette.altHeaderText;

    const int size = sizeIndex(theme.textSize);
    const qreal headingFactor = ProfileFonts::sizeFactor(theme.headingFont, Role::Heading);
    v.stripHeight = stripHeights.at(size);
    v.titlePixelSize = scaled(headingSizes.at(size), headingFactor);
    v.headingFamily = ProfileFonts::family(theme.headingFont, Role::Heading);
    v.headingBold = ProfileFonts::useBold(theme.headingFont, Role::Heading);
    v.headingLift = theme.headingFont == Profile::Font::ScriptFont ? -1 : 0;
    v.headingFactor = headingFactor;
    v.monogramFamily = ProfileFonts::family(theme.headingFont, Role::Body);
    v.bodyFamily = ProfileFonts::family(theme.bodyFont, Role::Body);
    v.bodyPixelSize = scaled(bodySizes.at(size), ProfileFonts::sizeFactor(theme.bodyFont, Role::Body));
    v.labelFamily = ProfileFonts::family(theme.bodyFont, Role::Label);
    v.labelBold = ProfileFonts::useBold(theme.bodyFont, Role::Label);
    v.labelPixelSize = scaled(bodySizes.at(size), ProfileFonts::sizeFactor(theme.bodyFont, Role::Label));
    v.subheadPixelSize = scaled(headingSizes.at(size), headingFactor);
    v.captionPixelSize = captionSizes.at(size);

    v.nameFamily = ProfileFonts::family(theme.nameFont, Role::Name);
    v.nameBasePixelSize = ProfileFonts::namePixelSize(theme.nameFont, theme.nameSize);
    v.nameMinPixelSize = nameFloor(theme.nameFont, theme.nameEffect, v.nameBasePixelSize);
    v.nameEffect = int(theme.nameEffect);
    v.nameFlourish = viewer.plain ? int(Profile::Flourish::NoFlourish) : int(theme.nameFlourish);
    v.nameColor = palette.name;
    v.nameColor2 = palette.name2;

    v.bodyColor = palette.body;
    v.labelColor = palette.label;
    v.linkColor = palette.link;
    v.mutedColor = palette.muted;
    v.blurbSubheadColor = palette.blurbSubhead;
    v.cellLabelFill = palette.cellLabelFill;
    v.cellValueFill = palette.cellValueFill;
    v.cellLabelInk = palette.cellLabelInk;
    v.cellValueInk = palette.cellValueInk;
    v.tableRuleColor = palette.tableRule;
    // The palette lists Available, Away, Busy, Offline; QML indexes by the
    // Presence value, whose Offline (2) comes before Busy (3).
    v.presenceInks = {palette.presence.at(0), palette.presence.at(1), palette.presence.at(3),
                      palette.presence.at(2)};
    v.monogramTop = palette.monogramTop;
    v.monogramBottom = palette.monogramBottom;
    v.monogramRim = palette.monogramRim;
    v.monogramInk = palette.monogramInk;
    v.altMonogramTop = palette.altMonogramTop;
    v.altMonogramBottom = palette.altMonogramBottom;
    v.altMonogramRim = palette.altMonogramRim;
    v.altMonogramInk = palette.altMonogramInk;
    v.songMaterial = palette.songMaterial;
    v.textHalo = palette.halo;
    v.haloColor = palette.haloColor;
    v.ambient = int(theme.ambient);
    v.ambientOutline = palette.ambientOutline;
    v.adjusted = palette.adjusted;
    for (const ProfileReadability::Adjustment &adjustment : palette.adjustments) {
        v.adjustments.append(QVariantMap{{QStringLiteral("tab"), int(adjustment.tab)},
                                         {QStringLiteral("role"), adjustment.role},
                                         {QStringLiteral("sentence"), adjustment.sentence}});
    }
    for (auto it = palette.inkAdjusted.cbegin(); it != palette.inkAdjusted.cend(); ++it)
        v.inkAdjusted.insert(QString::number(it.key()), it.value());
    return v;
}

void ProfileRenderStyle::update(const Values &values)
{
    if (values == m_values)
        return;
    m_values = values;
    emit changed();
}

// ---------------------------------------------------------------------------
// ProfilePageObject
// ---------------------------------------------------------------------------

ProfilePageObject::ProfilePageObject(Kind kind, QObject *parent)
    : QObject(parent)
    , m_kind(kind)
    , m_page(Profile::defaultPage())
    , m_render(this)
{
    m_lists = computeLists();
    m_render.update(ProfileRenderStyle::compute(m_page.theme, m_viewer, m_media.imageKey));
}

void ProfilePageObject::load(const Profile::Page &page)
{
    assign(page, false);
}

void ProfilePageObject::edit(const Profile::Page &page)
{
    if (readOnly())
        return;
    assign(page, true);
}

void ProfilePageObject::assign(const Profile::Page &page, bool write)
{
    if (page == m_page)
        return;
    const Profile::Page old = m_page;
    m_page = page;
    const bool style = old.theme != page.theme || old.layout != page.layout || old.preset != page.preset
                       || old.revision != page.revision;
    const bool content = old.content != page.content;
    const bool media = old.background != page.background || old.song != page.song;
    const Lists lists = computeLists();
    const bool listsDiffer = lists != m_lists;
    m_lists = lists;

    if (style)
        emit styleChanged();
    if (content)
        emit contentChanged();
    if (listsDiffer)
        emit listsChanged();
    if (media)
        emit mediaChanged();
    if (style || media)
        refreshRender();
    if (write)
        emit edited();
}

void ProfilePageObject::setViewer(const ProfileRenderStyle::Viewer &viewer)
{
    if (viewer.dark == m_viewer.dark && viewer.plain == m_viewer.plain)
        return;
    const Theme shownBefore = shownTheme();
    m_viewer = viewer;
    if (shownTheme() != shownBefore)
        emit styleChanged(); // an adaptive page's colours follow the mode
    updateLists();
    refreshRender();
}

void ProfilePageObject::setMediaState(const MediaState &state)
{
    if (state == m_media)
        return;
    m_media = state;
    emit mediaChanged();
    refreshRender();
}

void ProfilePageObject::setTileResolver(TileResolver resolver)
{
    m_tiles = std::move(resolver);
    updateLists();
}

void ProfilePageObject::refreshTiles()
{
    updateLists();
}

void ProfilePageObject::refreshRender()
{
    m_render.update(ProfileRenderStyle::compute(m_page.theme, m_viewer, m_media.imageKey));
}

void ProfilePageObject::updateLists()
{
    Lists lists = computeLists();
    if (lists == m_lists)
        return;
    m_lists = std::move(lists);
    emit listsChanged();
}

Theme ProfilePageObject::shownTheme() const
{
    return m_page.theme.adaptive ? ProfileRenderStyle::withAeroSkyColours(m_page.theme, m_viewer.dark)
                                 : m_page.theme;
}

bool ProfilePageObject::styleEditedSincePreset() const
{
    return Profile::styleDiffersFromPreset(m_page);
}

QString ProfilePageObject::songKey() const
{
    return m_page.song.isSet() ? QString::fromLatin1(m_page.song.sha256.toHex()) : QString();
}

bool ProfilePageObject::moduleHasContent(const Profile::Page &page, Profile::Module module)
{
    const Profile::Content &content = page.content;
    switch (module) {
    case Profile::Module::HandleModule:
        return true; // the app's, filled from the account (hidden while unknown)
    case Profile::Module::SongModule:
        return page.song.isSet();
    case Profile::Module::InterestsModule: {
        const Profile::Interests &i = content.interests;
        return filled(i.general) || filled(i.music) || filled(i.movies) || filled(i.television)
               || filled(i.books) || filled(i.heroes);
    }
    case Profile::Module::DetailsModule: {
        const Profile::Details &d = content.details;
        return (d.hereFor & Profile::hereForMask) != 0 || d.zodiac != Profile::Zodiac::NoZodiac
               || filled(d.hometown) || filled(d.occupation) || filled(d.education) || filled(d.languages);
    }
    case Profile::Module::BlurbsModule:
        return filled(content.aboutMe) || filled(content.meet);
    case Profile::Module::TopFriendsModule:
        return !page.topFriends.isEmpty();
    }
    return false;
}

QString ProfilePageObject::initialsFor(const QString &name)
{
    // Words are runs of letters and digits; punctuation and symbols (a
    // flourish, emoji) never become a monogram.
    QStringList words;
    QString word;
    for (qsizetype i = 0; i < name.size(); ++i) {
        const QChar c = name.at(i);
        const bool pair = c.isHighSurrogate() && i + 1 < name.size() && name.at(i + 1).isLowSurrogate();
        const char32_t code = pair ? QChar::surrogateToUcs4(c, name.at(i + 1)) : c.unicode();
        const bool letter = QChar::isLetterOrNumber(code);
        if (letter)
            word.append(pair ? name.mid(i, 2) : QString(c));
        else if (!word.isEmpty())
            words.append(std::exchange(word, QString()));
        if (pair)
            ++i;
    }
    if (!word.isEmpty())
        words.append(word);
    if (words.isEmpty())
        return QStringLiteral("?");
    QString initials = firstLetter(words.first());
    if (words.size() > 1)
        initials += firstLetter(words.last());
    return initials.toUpper();
}

QVariantList ProfilePageObject::monogramTiles(const QVector<Profile::TopFriend> &friends)
{
    QVariantList tiles;
    for (qsizetype index = 0; index < friends.size(); ++index) {
        const Profile::TopFriend &person = friends.at(index);
        tiles.append(QVariantMap{{QStringLiteral("index"), int(index)},
                                 {QStringLiteral("accountId"), QString::fromLatin1(person.accountId.toHex())},
                                 {QStringLiteral("name"), person.name},
                                 {QStringLiteral("initials"), initialsFor(person.name)},
                                 {QStringLiteral("avatarKey"), QString()},
                                 {QStringLiteral("isContact"), false},
                                 {QStringLiteral("isSelf"), false}});
    }
    return tiles;
}

ProfilePageObject::Lists ProfilePageObject::computeLists() const
{
    Lists lists;
    lists.topFriends = m_tiles ? m_tiles(m_page.topFriends) : monogramTiles(m_page.topFriends);

    const QVector<Profile::ModulePlacement> modules =
        m_page.modules.isEmpty() ? Profile::defaultModules() : m_page.modules;
    for (const Profile::ModulePlacement &placement : modules) {
        const bool content = moduleHasContent(m_page, placement.module);
        lists.moduleArrangement.append(QVariantMap{{QStringLiteral("module"), int(placement.module)},
                                                   {QStringLiteral("name"), Profile::moduleName(placement.module)},
                                                   {QStringLiteral("column"), int(placement.column)},
                                                   {QStringLiteral("visible"), placement.visible},
                                                   {QStringLiteral("hasContent"), content}});
        // Viewers never see an empty box; the editor's preview shows it as a
        // placeholder to fill in.
        if (!placement.visible || (!content && !showEmptyModules()))
            continue;
        QVariantList &column = placement.column == Profile::Column::NarrowColumn ? lists.narrowModules
                                                                                 : lists.wideModules;
        column.append(int(placement.module));
    }

    const Profile::Interests &interests = m_page.content.interests;
    const std::array<std::pair<const char *, const QString *>, 6> interestFields{{
        {"General", &interests.general},
        {"Music", &interests.music},
        {"Movies", &interests.movies},
        {"Television", &interests.television},
        {"Books", &interests.books},
        {"Heroes", &interests.heroes},
    }};
    for (const auto &[label, value] : interestFields) {
        if (filled(*value))
            lists.interestRows.append(row(QString::fromLatin1(label), *value));
    }

    const Profile::Details &details = m_page.content.details;
    const QString hereFor = Profile::hereForText(details.hereFor);
    const QString zodiac = Profile::zodiacName(details.zodiac);
    const std::array<std::pair<const char *, QString>, 6> detailFields{{
        {"Here for", hereFor},
        {"Hometown", details.hometown},
        {"Zodiac sign", zodiac},
        {"Occupation", details.occupation},
        {"Education", details.education},
        {"Languages", details.languages},
    }};
    for (const auto &[label, value] : detailFields) {
        if (filled(value))
            lists.detailRows.append(row(QString::fromLatin1(label), value));
    }

    lists.hasBlurbs = filled(m_page.content.aboutMe) || filled(m_page.content.meet);

    const Theme shown = shownTheme();
    for (const quint32 value : {shown.backgroundColor1, shown.backgroundColor2, shown.motifInk, shown.boxFill,
                                shown.borderColor, shown.headerFill, shown.bodyColor, shown.linkColor})
        lists.paletteSwatches.append(colourOf(value));
    return lists;
}

// --- Writes ------------------------------------------------------------------

void ProfilePageObject::writeTheme(const std::function<void(Theme &)> &change, bool colour)
{
    if (readOnly())
        return;
    // Compared against what the viewer sees, so a control writing back the
    // value it shows is a no-op and never detaches an adaptive page.
    const Theme shown = shownTheme();
    Theme wanted = shown;
    change(wanted);
    if (wanted == shown)
        return;
    Profile::Page next = m_page;
    if (colour && next.theme.adaptive) {
        // Stop following the viewer's mode, keeping the palette on screen.
        next.theme = wanted;
        next.theme.adaptive = false;
    } else {
        change(next.theme);
    }
    assign(next, true);
}

void ProfilePageObject::writeColour(quint32 Theme::*field, const QColor &color)
{
    if (!color.isValid())
        return;
    const quint32 value = rgbOf(color);
    writeTheme([&](Theme &theme) { theme.*field = value; }, true);
}

QColor ProfilePageObject::shownColour(quint32 Theme::*field) const
{
    return colourOf(shownTheme().*field);
}

void ProfilePageObject::writeContent(const std::function<void(Profile::Content &)> &change)
{
    if (readOnly())
        return;
    Profile::Page next = m_page;
    change(next.content);
    if (next.content == m_page.content)
        return;
    assign(next, true);
}

void ProfilePageObject::writeText(QString Profile::Content::*field, const QString &text, int bound, bool multiLine)
{
    const QString value = Profile::sanitizeLive(text, bound, multiLine);
    writeContent([&](Profile::Content &content) { content.*field = value; });
}

void ProfilePageObject::writeInterest(QString Profile::Interests::*field, const QString &text)
{
    const QString value = Profile::sanitizeLive(text, Profile::TextBounds::interest, true);
    writeContent([&](Profile::Content &content) { content.interests.*field = value; });
}

void ProfilePageObject::writeDetail(QString Profile::Details::*field, const QString &text)
{
    const QString value = Profile::sanitizeLive(text, Profile::TextBounds::detail, false);
    writeContent([&](Profile::Content &content) { content.details.*field = value; });
}

void ProfilePageObject::setAdaptive(bool adaptive)
{
    if (readOnly() || adaptive == m_page.theme.adaptive)
        return;
    Profile::Page next = m_page;
    // Leaving the adaptive look keeps the colours on screen; taking it up
    // hands the colours back to Aero Sky.
    if (!adaptive)
        next.theme = shownTheme();
    next.theme.adaptive = adaptive;
    assign(next, true);
}

void ProfilePageObject::setLayout(int layout)
{
    if (readOnly() || !inRange(layout, Profile::Layout::SingleLayout) || layout == int(m_page.layout))
        return;
    Profile::Page next = m_page;
    next.layout = Profile::Layout(layout);
    assign(next, true);
}

void ProfilePageObject::setBackgroundKind(int kind)
{
    if (!inRange(kind, Profile::BackgroundKind::ImageBackground))
        return;
    writeTheme([&](Theme &theme) { theme.backgroundKind = Profile::BackgroundKind(kind); }, false);
}

QColor ProfilePageObject::backgroundColor1() const { return shownColour(&Theme::backgroundColor1); }
void ProfilePageObject::setBackgroundColor1(const QColor &color) { writeColour(&Theme::backgroundColor1, color); }
QColor ProfilePageObject::backgroundColor2() const { return shownColour(&Theme::backgroundColor2); }
void ProfilePageObject::setBackgroundColor2(const QColor &color) { writeColour(&Theme::backgroundColor2, color); }

void ProfilePageObject::setMotif(int motif)
{
    if (!inRange(motif, Profile::Motif::LinenWeave))
        return;
    writeTheme([&](Theme &theme) { theme.motif = Profile::Motif(motif); }, false);
}

QColor ProfilePageObject::motifInk() const { return shownColour(&Theme::motifInk); }
void ProfilePageObject::setMotifInk(const QColor &color) { writeColour(&Theme::motifInk, color); }

void ProfilePageObject::setMotifOpacity(int percent)
{
    const auto value = quint8(std::clamp(percent, 10, 80));
    writeTheme([&](Theme &theme) { theme.motifOpacity = value; }, false);
}

void ProfilePageObject::setMotifScale(int scale)
{
    if (!inRange(scale, Profile::MotifScale::LargeMotif))
        return;
    writeTheme([&](Theme &theme) { theme.motifScale = Profile::MotifScale(scale); }, false);
}

void ProfilePageObject::setImageMode(int mode)
{
    if (!inRange(mode, Profile::ImageMode::CenterImage))
        return;
    writeTheme([&](Theme &theme) { theme.imageMode = Profile::ImageMode(mode); }, false);
}

void ProfilePageObject::setImageFixed(bool fixed)
{
    writeTheme([&](Theme &theme) { theme.imageFixed = fixed; }, false);
}

QColor ProfilePageObject::boxFill() const { return shownColour(&Theme::boxFill); }
void ProfilePageObject::setBoxFill(const QColor &color) { writeColour(&Theme::boxFill, color); }

int ProfilePageObject::boxOpacity() const
{
    return shownTheme().boxOpacity;
}

void ProfilePageObject::setBoxOpacity(int percent)
{
    // Aero Sky's opacity is part of its mode palette, like its colours.
    const auto value = quint8(std::clamp(percent, 60, 100));
    writeTheme([&](Theme &theme) { theme.boxOpacity = value; }, true);
}

QColor ProfilePageObject::borderColor() const { return shownColour(&Theme::borderColor); }
void ProfilePageObject::setBorderColor(const QColor &color) { writeColour(&Theme::borderColor, color); }

void ProfilePageObject::setBorderWidth(int width)
{
    const auto value = quint8(std::clamp(width, 0, 4));
    writeTheme([&](Theme &theme) { theme.borderWidth = value; }, false);
}

void ProfilePageObject::setBorderStyle(int style)
{
    if (!inRange(style, Profile::BorderStyle::DoubleBorder))
        return;
    writeTheme([&](Theme &theme) { theme.borderStyle = Profile::BorderStyle(style); }, false);
}

void ProfilePageObject::setBoxRadius(int radius)
{
    if (!inRange(radius, Profile::BoxRadius::RoundCorners))
        return;
    writeTheme([&](Theme &theme) { theme.boxRadius = Profile::BoxRadius(radius); }, false);
}

void ProfilePageObject::setBoxGlow(bool glow)
{
    writeTheme([&](Theme &theme) { theme.boxGlow = glow; }, false);
}

void ProfilePageObject::setTableStyle(int style)
{
    if (!inRange(style, Profile::TableStyle::LineTable))
        return;
    writeTheme([&](Theme &theme) { theme.tableStyle = Profile::TableStyle(style); }, false);
}

QColor ProfilePageObject::altBorderColor() const { return shownColour(&Theme::altBorderColor); }
void ProfilePageObject::setAltBorderColor(const QColor &color) { writeColour(&Theme::altBorderColor, color); }

void ProfilePageObject::setHeaderStyle(int style)
{
    if (!inRange(style, Profile::HeaderStyle::NoHeader))
        return;
    writeTheme([&](Theme &theme) { theme.headerStyle = Profile::HeaderStyle(style); }, false);
}

QColor ProfilePageObject::headerFill() const { return shownColour(&Theme::headerFill); }
void ProfilePageObject::setHeaderFill(const QColor &color) { writeColour(&Theme::headerFill, color); }
QColor ProfilePageObject::headerText() const { return shownColour(&Theme::headerText); }
void ProfilePageObject::setHeaderText(const QColor &color) { writeColour(&Theme::headerText, color); }

void ProfilePageObject::setAltHeader(bool alt)
{
    writeTheme([&](Theme &theme) { theme.altHeader = alt; }, false);
}

QColor ProfilePageObject::altHeaderFill() const { return shownColour(&Theme::altHeaderFill); }
void ProfilePageObject::setAltHeaderFill(const QColor &color) { writeColour(&Theme::altHeaderFill, color); }
QColor ProfilePageObject::altHeaderText() const { return shownColour(&Theme::altHeaderText); }
void ProfilePageObject::setAltHeaderText(const QColor &color) { writeColour(&Theme::altHeaderText, color); }

void ProfilePageObject::setHeadingFont(int font)
{
    // Pixel is for names only (SPEC §11).
    if (!inRange(font, Profile::Font::MarkerFont) || !Profile::isHeadingCapable(Profile::Font(font)))
        return;
    writeTheme([&](Theme &theme) { theme.headingFont = Profile::Font(font); }, false);
}

void ProfilePageObject::setBodyFont(int font)
{
    if (!inRange(font, Profile::Font::MarkerFont) || !Profile::isBodySafe(Profile::Font(font)))
        return;
    writeTheme([&](Theme &theme) { theme.bodyFont = Profile::Font(font); }, false);
}

void ProfilePageObject::setTextSize(int size)
{
    if (!inRange(size, Profile::TextSize::LargeText))
        return;
    writeTheme([&](Theme &theme) { theme.textSize = Profile::TextSize(size); }, false);
}

QColor ProfilePageObject::bodyColor() const { return shownColour(&Theme::bodyColor); }
void ProfilePageObject::setBodyColor(const QColor &color) { writeColour(&Theme::bodyColor, color); }
QColor ProfilePageObject::labelColor() const { return shownColour(&Theme::labelColor); }
void ProfilePageObject::setLabelColor(const QColor &color) { writeColour(&Theme::labelColor, color); }
QColor ProfilePageObject::linkColor() const { return shownColour(&Theme::linkColor); }
void ProfilePageObject::setLinkColor(const QColor &color) { writeColour(&Theme::linkColor, color); }

void ProfilePageObject::setNameFont(int font)
{
    if (!inRange(font, Profile::Font::MarkerFont))
        return;
    writeTheme([&](Theme &theme) { theme.nameFont = Profile::Font(font); }, false);
}

QColor ProfilePageObject::nameColor() const { return shownColour(&Theme::nameColor); }
void ProfilePageObject::setNameColor(const QColor &color) { writeColour(&Theme::nameColor, color); }
QColor ProfilePageObject::nameColor2() const { return shownColour(&Theme::nameColor2); }
void ProfilePageObject::setNameColor2(const QColor &color) { writeColour(&Theme::nameColor2, color); }

void ProfilePageObject::setNameEffect(int effect)
{
    if (!inRange(effect, Profile::NameEffect::ShadowName))
        return;
    writeTheme([&](Theme &theme) { theme.nameEffect = Profile::NameEffect(effect); }, false);
}

void ProfilePageObject::setNameSize(int size)
{
    if (!inRange(size, Profile::NameSize::ExtraLargeName))
        return;
    writeTheme([&](Theme &theme) { theme.nameSize = Profile::NameSize(size); }, false);
}

void ProfilePageObject::setNameFlourish(int flourish)
{
    if (!inRange(flourish, Profile::Flourish::FlowerFlourish))
        return;
    writeTheme([&](Theme &theme) { theme.nameFlourish = Profile::Flourish(flourish); }, false);
}

void ProfilePageObject::setAmbient(int ambient)
{
    if (!inRange(ambient, Profile::Ambient::FloatingSparkles))
        return;
    writeTheme([&](Theme &theme) { theme.ambient = Profile::Ambient(ambient); }, false);
}

void ProfilePageObject::setDisplayName(const QString &text)
{
    writeText(&Profile::Content::displayName, text, Profile::TextBounds::displayName, false);
}

void ProfilePageObject::setHeadline(const QString &text)
{
    writeText(&Profile::Content::headline, text, Profile::TextBounds::headline, false);
}

QString ProfilePageObject::infoLine(int index) const
{
    return m_page.content.infoLines.value(index);
}

void ProfilePageObject::setInfoLine(int index, const QString &text)
{
    const QString value = Profile::sanitizeLive(text, Profile::TextBounds::infoLine, false);
    writeContent([&](Profile::Content &content) {
        // Three fixed slots while editing, so clearing line 1 never moves
        // lines 2 and 3 up under the cursor; normalisation drops the empty
        // ones when the page is saved.
        QStringList lines = content.infoLines;
        while (lines.size() < Profile::TextBounds::infoLines)
            lines.append(QString());
        lines[index] = value;
        while (!lines.isEmpty() && lines.last().isEmpty())
            lines.removeLast();
        content.infoLines = lines;
    });
}

void ProfilePageObject::setMood(int mood)
{
    if (mood < 0 || mood > Profile::maxMood)
        return;
    writeContent([&](Profile::Content &content) { content.mood = Profile::Mood(mood); });
}

void ProfilePageObject::setInterestGeneral(const QString &text) { writeInterest(&Profile::Interests::general, text); }
void ProfilePageObject::setInterestMusic(const QString &text) { writeInterest(&Profile::Interests::music, text); }
void ProfilePageObject::setInterestMovies(const QString &text) { writeInterest(&Profile::Interests::movies, text); }
void ProfilePageObject::setInterestTelevision(const QString &text)
{
    writeInterest(&Profile::Interests::television, text);
}
void ProfilePageObject::setInterestBooks(const QString &text) { writeInterest(&Profile::Interests::books, text); }
void ProfilePageObject::setInterestHeroes(const QString &text) { writeInterest(&Profile::Interests::heroes, text); }

void ProfilePageObject::setHereFor(int bits)
{
    const auto value = quint8(bits & Profile::hereForMask);
    writeContent([&](Profile::Content &content) { content.details.hereFor = value; });
}

void ProfilePageObject::setHometown(const QString &text) { writeDetail(&Profile::Details::hometown, text); }

void ProfilePageObject::setZodiac(int zodiac)
{
    if (!inRange(zodiac, Profile::Zodiac::Pisces))
        return;
    writeContent([&](Profile::Content &content) { content.details.zodiac = Profile::Zodiac(zodiac); });
}

void ProfilePageObject::setOccupation(const QString &text) { writeDetail(&Profile::Details::occupation, text); }
void ProfilePageObject::setEducation(const QString &text) { writeDetail(&Profile::Details::education, text); }
void ProfilePageObject::setLanguages(const QString &text) { writeDetail(&Profile::Details::languages, text); }

void ProfilePageObject::setAboutMe(const QString &text)
{
    writeText(&Profile::Content::aboutMe, text, Profile::TextBounds::aboutMe, true);
}

void ProfilePageObject::setMeet(const QString &text)
{
    writeText(&Profile::Content::meet, text, Profile::TextBounds::meet, true);
}

void ProfilePageObject::setSongTitle(const QString &text)
{
    writeText(&Profile::Content::songTitle, text, Profile::TextBounds::songTitle, false);
}

void ProfilePageObject::setSongArtist(const QString &text)
{
    writeText(&Profile::Content::songArtist, text, Profile::TextBounds::songArtist, false);
}

} // namespace OpenChat
