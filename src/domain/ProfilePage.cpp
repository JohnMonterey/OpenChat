#include "domain/ProfilePage.h"

#include "domain/Identifiers.h"
#include "domain/ProfilePageCodec.h"

#include <QTextBoundaryFinder>

#include <algorithm>
#include <array>

namespace OpenChat::Profile {

namespace {

constexpr quint32 colourMask = 0xFFFFFF;
constexpr quint8 minMotifOpacity = 10, maxMotifOpacity = 80;
constexpr quint8 minBoxOpacity = 60, maxBoxOpacity = 100;
constexpr quint8 maxBorderWidth = 4;
constexpr int moduleCount = 6;

// Every enum here counts up from 0 (Module from 1) with a quint8 underlying
// type, so "in range" is one comparison against the last enumerator. Values
// out of range can reach a Page through static_cast from QML ints or the
// codec, and are reset rather than trusted.
template<typename Enum>
[[nodiscard]] constexpr Enum validOr(Enum value, Enum last, Enum fallback) noexcept
{
    return static_cast<int>(value) <= static_cast<int>(last) ? value : fallback;
}

[[nodiscard]] bool isShippedPreset(Preset preset) noexcept
{
    return static_cast<int>(preset) <= static_cast<int>(Preset::ChromeY2KPreset);
}

[[nodiscard]] bool isKnownModule(Module module) noexcept
{
    const int id = static_cast<int>(module);
    return id >= static_cast<int>(Module::HandleModule) && id <= static_cast<int>(Module::TopFriendsModule);
}

[[nodiscard]] Column defaultColumn(Module module) noexcept
{
    return module == Module::BlurbsModule || module == Module::TopFriendsModule ? Column::WideColumn
                                                                                 : Column::NarrowColumn;
}

// ---------------------------------------------------------------------------
// Text sanitising (ARCH §2.4)
// ---------------------------------------------------------------------------

constexpr char32_t zeroWidthNonJoiner = 0x200C;
constexpr char32_t zeroWidthJoiner = 0x200D;

enum class CharClass {
    Newline,   // kept in paragraphs
    Space,     // any whitespace; collapsed in full sanitising
    Drop,      // removed unconditionally
    Joiner,    // U+200D, kept only inside an emoji sequence
    NonJoiner, // U+200C, kept only between two non-ASCII letters
    Selector,  // U+FE00–FE0F, kept only right after a base character
    Mark,      // Mn/Me, capped per cluster in full sanitising
    Base,
};

[[nodiscard]] bool isBidiControl(char32_t c) noexcept
{
    return c == 0x061C || c == 0x200E || c == 0x200F || (c >= 0x202A && c <= 0x202E)
        || (c >= 0x2066 && c <= 0x2069);
}

// The default-ignorable code points that render as nothing and have no
// legitimate use in a name or a blurb. ZWJ, ZWNJ and the variation selectors
// are also default-ignorable but carry meaning in context, so they are
// classified separately.
[[nodiscard]] bool isDroppedIgnorable(char32_t c) noexcept
{
    return c == 0x00AD || c == 0x034F || c == 0x115F || c == 0x1160 || c == 0x17B4 || c == 0x17B5
        || (c >= 0x180B && c <= 0x180F) || c == 0x200B || (c >= 0x2060 && c <= 0x206F) || c == 0x3164
        || c == 0xFEFF || c == 0xFFA0 || (c >= 0x1BCA0 && c <= 0x1BCA3) || (c >= 0x1D173 && c <= 0x1D17A)
        || (c >= 0xE0000 && c <= 0xE0FFF);
}

[[nodiscard]] bool isVariationSelector(char32_t c) noexcept
{
    return c >= 0xFE00 && c <= 0xFE0F;
}

[[nodiscard]] bool isEmoji(char32_t c) noexcept
{
    return c >= 0x1F000 || (c >= 0x2600 && c <= 0x27BF) || (c >= 0x2B00 && c <= 0x2BFF);
}

[[nodiscard]] bool isMarkCategory(char32_t c) noexcept
{
    const QChar::Category category = QChar::category(c);
    return category == QChar::Mark_NonSpacing || category == QChar::Mark_SpacingCombining
        || category == QChar::Mark_Enclosing;
}

[[nodiscard]] bool isNonAsciiLetter(char32_t c) noexcept
{
    return c >= 0x80 && QChar::isLetter(c);
}

[[nodiscard]] CharClass classify(char32_t c, bool multiLine) noexcept
{
    if (c == U'\n')
        return multiLine ? CharClass::Newline : CharClass::Space;
    // A tab is whitespace people paste from spreadsheets; dropping it like
    // the other C0 controls would glue the words on either side together.
    if (c == U'\t')
        return CharClass::Space;
    if (c < 0x20 || (c >= 0x7F && c <= 0x9F)) // C0, DEL, C1 (including NEL)
        return CharClass::Drop;
    if (c == 0x2028 || c == 0x2029 || isBidiControl(c) || isDroppedIgnorable(c))
        return CharClass::Drop;
    if (c == zeroWidthJoiner)
        return CharClass::Joiner;
    if (c == zeroWidthNonJoiner)
        return CharClass::NonJoiner;
    if (isVariationSelector(c))
        return CharClass::Selector;
    if (QChar::isSpace(c))
        return CharClass::Space;
    const QChar::Category category = QChar::category(c);
    if (category == QChar::Mark_NonSpacing || category == QChar::Mark_Enclosing)
        return CharClass::Mark;
    return CharClass::Base;
}

// Code points with CR/CRLF turned into LF, unpaired surrogates dropped, and
// every unconditionally dropped character removed. Removing those first means
// the context rules below always look at the neighbours that survive.
[[nodiscard]] QVector<char32_t> survivingCodePoints(const QString &text, bool multiLine)
{
    QVector<char32_t> points;
    points.reserve(text.size());
    const qsizetype size = text.size();
    for (qsizetype i = 0; i < size; ++i) {
        const QChar unit = text.at(i);
        char32_t c = unit.unicode();
        if (unit.isHighSurrogate()) {
            if (i + 1 >= size || !text.at(i + 1).isLowSurrogate())
                continue;
            c = QChar::surrogateToUcs4(unit, text.at(i + 1));
            ++i;
        } else if (unit.isLowSurrogate()) {
            continue;
        } else if (c == U'\r') {
            c = U'\n';
            if (i + 1 < size && text.at(i + 1) == u'\n')
                ++i;
        }
        if (classify(c, multiLine) != CharClass::Drop)
            points.push_back(c);
    }
    return points;
}

// A variation selector changes how the character before it is drawn, so it
// is only meaningful directly after a visible, non-combining character.
[[nodiscard]] bool acceptsSelector(const QVector<char32_t> &out) noexcept
{
    if (out.isEmpty())
        return false;
    const char32_t previous = out.constLast();
    return previous != U'\n' && !QChar::isSpace(previous) && previous != zeroWidthJoiner
        && previous != zeroWidthNonJoiner && !isVariationSelector(previous) && !isMarkCategory(previous);
}

// ZWJ glues emoji into one picture (woman + laptop, white flag + rainbow);
// the left side may carry its variation selector.
[[nodiscard]] bool joinsAfter(const QVector<char32_t> &out) noexcept
{
    if (out.isEmpty())
        return false;
    const char32_t previous = out.constLast();
    if (isEmoji(previous))
        return true;
    return isVariationSelector(previous) && out.size() >= 2 && isEmoji(out.at(out.size() - 2));
}

// ZWNJ keeps two letters from joining (Persian, Indic scripts). The letter on
// the left may carry combining marks such as a virama.
[[nodiscard]] bool letterBefore(const QVector<char32_t> &out) noexcept
{
    for (qsizetype i = out.size() - 1; i >= 0; --i) {
        if (!isMarkCategory(out.at(i)))
            return isNonAsciiLetter(out.at(i));
    }
    return false;
}

// One pass over the surviving code points. `full` adds the combining-mark
// cap and holds whitespace back until visible text follows it on the same
// line, which collapses runs, trims every line's end and the whole text, and
// allows at most one blank line in a row. Without `full` (typing), whitespace
// is kept exactly as typed and a joiner at the very end is kept while its
// right-hand neighbour has not been typed yet.
//
// Idempotent: every character this keeps is kept again on a second pass,
// because the neighbours it was judged by are kept too.
[[nodiscard]] QString filterText(const QVector<char32_t> &points, bool multiLine, bool full)
{
    QVector<char32_t> out;
    out.reserve(points.size());
    int heldNewlines = 0;
    bool heldSpace = false;
    int markRun = 0;

    const auto holding = [&] { return heldNewlines > 0 || heldSpace; };
    const auto release = [&] {
        if (!holding())
            return;
        if (!out.isEmpty()) { // leading whitespace of the whole text is trimmed
            for (int n = 0; n < std::min(heldNewlines, 2); ++n)
                out.push_back(U'\n');
            if (heldSpace)
                out.push_back(U' ');
        }
        heldNewlines = 0;
        heldSpace = false;
        markRun = 0;
    };

    const qsizetype count = points.size();
    for (qsizetype i = 0; i < count; ++i) {
        const char32_t c = points.at(i);
        const bool atEnd = i + 1 == count;
        switch (classify(c, multiLine)) {
        case CharClass::Drop:
            break;
        case CharClass::Newline:
            if (full) {
                heldSpace = false; // the line's trailing whitespace goes
                ++heldNewlines;
            } else {
                out.push_back(U'\n');
                markRun = 0;
            }
            break;
        case CharClass::Space:
            if (full) {
                heldSpace = true;
            } else {
                // Typing keeps whitespace, but a line break or tab in a
                // single-line field becomes a plain space.
                out.push_back(c == U'\n' || c == U'\t' ? U' ' : c);
                markRun = 0;
            }
            break;
        case CharClass::Joiner: {
            const bool rightOk = atEnd ? !full : isEmoji(points.at(i + 1));
            if (!holding() && joinsAfter(out) && rightOk)
                out.push_back(c);
            break;
        }
        case CharClass::NonJoiner: {
            const bool rightOk = atEnd ? !full : isNonAsciiLetter(points.at(i + 1));
            if (!holding() && letterBefore(out) && rightOk)
                out.push_back(c);
            break;
        }
        case CharClass::Selector:
            if (!holding() && acceptsSelector(out))
                out.push_back(c);
            break;
        case CharClass::Mark:
            if (full) {
                release();
                // The "Zalgo" guard: marks stacked beyond two per base grow
                // over the neighbouring lines and boxes.
                if (markRun >= 2)
                    break;
                ++markRun;
            }
            out.push_back(c);
            break;
        case CharClass::Base:
            release();
            out.push_back(c);
            markRun = 0;
            break;
        }
    }
    // Whitespace still held at the end is the text's trailing whitespace.
    return QString::fromUcs4(out.constData(), out.size());
}

// Whether the UTF-16 unit at `index` belongs to the character before it: the
// second half of a surrogate pair, or a combining mark (whose base is before
// it).
[[nodiscard]] bool continuesACharacter(const QString &text, qsizetype index) noexcept
{
    const QChar unit = text.at(index);
    if (unit.isLowSurrogate())
        return index > 0 && text.at(index - 1).isHighSurrogate();
    char32_t c = unit.unicode();
    if (unit.isHighSurrogate() && index + 1 < text.size() && text.at(index + 1).isLowSurrogate())
        c = QChar::surrogateToUcs4(unit, text.at(index + 1));
    return isMarkCategory(c);
}

// Cuts at the last grapheme-cluster boundary at or before maxLength, so a
// surrogate pair, a base with its marks or an emoji sequence is never split.
//
// The boundary finder is not trusted alone: after a run of an Indic or Thai
// script, Qt 6.11 reports a boundary inside the surrogate pair of an emoji
// that follows. Full sanitising would drop the half character in its second
// pass, but typing gets no second pass, so the cut itself steps back until it
// no longer splits a pair or separates a mark from its base.
[[nodiscard]] QString truncateAtCluster(const QString &text, qsizetype maxLength)
{
    if (text.size() <= maxLength)
        return text;
    if (maxLength <= 0)
        return {};
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
    finder.setPosition(maxLength);
    qsizetype cut = maxLength;
    if (!finder.isAtBoundary())
        cut = std::max<qsizetype>(finder.toPreviousBoundary(), 0);
    while (cut > 0 && continuesACharacter(text, cut)) // cut < text.size(): the text is longer than the bound
        --cut;
    return text.left(cut);
}

[[nodiscard]] QString sanitize(const QString &text, qsizetype maxLength, bool multiLine, bool full)
{
    if (maxLength <= 0)
        return {};
    QString result = filterText(survivingCodePoints(text, multiLine), multiLine, full);
    if (result.size() > maxLength) {
        result = truncateAtCluster(result, maxLength);
        // The cut can leave trailing whitespace or a joiner with nothing to
        // join; one more pass trims those, as full sanitising promises.
        if (full)
            result = filterText(survivingCodePoints(result, multiLine), multiLine, full);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Normalisation
// ---------------------------------------------------------------------------

[[nodiscard]] MediaRef normalizedBackground(const MediaRef &ref)
{
    if (!ref.isSet() || ref.bytes == 0 || ref.bytes > quint32(maxBackgroundImageBytes)
        || ref.width > maxBackgroundDimension || ref.height > maxBackgroundDimension)
        return {};
    MediaRef result = ref;
    result.durationMs = 0;
    return result;
}

[[nodiscard]] MediaRef normalizedSong(const MediaRef &ref)
{
    if (!ref.isSet() || ref.bytes == 0 || ref.bytes > quint32(maxSongBytes)
        || ref.durationMs > maxSongRefDurationMs)
        return {};
    MediaRef result = ref;
    result.width = 0;
    result.height = 0;
    return result;
}

[[nodiscard]] Theme normalizedTheme(Theme theme, bool hasBackgroundImage)
{
    const Theme defaults;
    theme.backgroundKind =
        validOr(theme.backgroundKind, BackgroundKind::ImageBackground, defaults.backgroundKind);
    if (theme.backgroundKind == BackgroundKind::ImageBackground && !hasBackgroundImage)
        theme.backgroundKind = BackgroundKind::SolidBackground;
    theme.motif = validOr(theme.motif, Motif::LinenWeave, defaults.motif);
    theme.motifOpacity = std::clamp(theme.motifOpacity, minMotifOpacity, maxMotifOpacity);
    theme.motifScale = validOr(theme.motifScale, MotifScale::LargeMotif, defaults.motifScale);
    theme.imageMode = validOr(theme.imageMode, ImageMode::CenterImage, defaults.imageMode);
    theme.boxOpacity = std::clamp(theme.boxOpacity, minBoxOpacity, maxBoxOpacity);
    theme.borderWidth = std::min(theme.borderWidth, maxBorderWidth);
    theme.borderStyle = validOr(theme.borderStyle, BorderStyle::DoubleBorder, defaults.borderStyle);
    theme.boxRadius = validOr(theme.boxRadius, BoxRadius::RoundCorners, defaults.boxRadius);
    theme.tableStyle = validOr(theme.tableStyle, TableStyle::LineTable, defaults.tableStyle);
    theme.headerStyle = validOr(theme.headerStyle, HeaderStyle::NoHeader, defaults.headerStyle);
    theme.headingFont = validOr(theme.headingFont, Font::MarkerFont, defaults.headingFont);
    if (!isHeadingCapable(theme.headingFont))
        theme.headingFont = Font::InterfaceFont;
    theme.bodyFont = validOr(theme.bodyFont, Font::MarkerFont, defaults.bodyFont);
    if (!isBodySafe(theme.bodyFont))
        theme.bodyFont = Font::InterfaceFont;
    theme.textSize = validOr(theme.textSize, TextSize::LargeText, defaults.textSize);
    theme.nameFont = validOr(theme.nameFont, Font::MarkerFont, defaults.nameFont);
    theme.nameEffect = validOr(theme.nameEffect, NameEffect::ShadowName, defaults.nameEffect);
    theme.nameSize = validOr(theme.nameSize, NameSize::ExtraLargeName, defaults.nameSize);
    theme.nameFlourish = validOr(theme.nameFlourish, Flourish::FlowerFlourish, defaults.nameFlourish);
    theme.ambient = validOr(theme.ambient, Ambient::FloatingSparkles, defaults.ambient);
    for (quint32 *colour : {&theme.backgroundColor1, &theme.backgroundColor2, &theme.motifInk, &theme.boxFill,
                            &theme.borderColor, &theme.headerFill, &theme.headerText, &theme.altHeaderFill,
                            &theme.altHeaderText, &theme.altBorderColor, &theme.bodyColor, &theme.labelColor,
                            &theme.linkColor, &theme.nameColor, &theme.nameColor2})
        *colour &= colourMask;
    return theme;
}

// Unknown and repeated modules go (the first placement wins), a column out of
// range falls back to the module's default one, and every module missing is
// appended to its default column, visible. The result lists the narrow
// column first, so two arrangements that look the same compare equal and
// encode to the same bytes.
[[nodiscard]] QVector<ModulePlacement> normalizedModules(const QVector<ModulePlacement> &modules)
{
    QVector<ModulePlacement> result;
    result.reserve(moduleCount);
    std::array<bool, moduleCount + 1> seen{};
    for (const ModulePlacement &placement : modules) {
        if (!isKnownModule(placement.module) || seen[static_cast<int>(placement.module)])
            continue;
        seen[static_cast<int>(placement.module)] = true;
        ModulePlacement kept = placement;
        if (static_cast<int>(kept.column) > static_cast<int>(Column::WideColumn))
            kept.column = defaultColumn(kept.module);
        result.push_back(kept);
    }
    for (const ModulePlacement &placement : defaultModules()) {
        if (!seen[static_cast<int>(placement.module)])
            result.push_back(placement);
    }
    std::stable_partition(result.begin(), result.end(), [](const ModulePlacement &placement) {
        return placement.column == Column::NarrowColumn;
    });
    return result;
}

[[nodiscard]] QVector<TopFriend> normalizedTopFriends(const QVector<TopFriend> &friends)
{
    QVector<TopFriend> result;
    for (const TopFriend &topFriend : friends) {
        if (result.size() == maxTopFriends)
            break;
        if (!AccountId::fromBytes(topFriend.accountId))
            continue;
        const bool repeated = std::any_of(result.cbegin(), result.cend(), [&](const TopFriend &kept) {
            return kept.accountId == topFriend.accountId;
        });
        if (repeated)
            continue;
        result.push_back({topFriend.accountId, sanitizeLine(topFriend.name, TextBounds::friendName)});
    }
    return result;
}

[[nodiscard]] Content normalizedContent(const Content &content)
{
    Content result;
    result.displayName = sanitizeLine(content.displayName, TextBounds::displayName);
    result.headline = sanitizeLine(content.headline, TextBounds::headline);
    for (const QString &line : content.infoLines) {
        if (result.infoLines.size() == TextBounds::infoLines)
            break;
        const QString clean = sanitizeLine(line, TextBounds::infoLine);
        if (!clean.isEmpty())
            result.infoLines.push_back(clean);
    }
    result.mood = static_cast<int>(content.mood) <= maxMood ? content.mood : Mood::NoMood;

    const Interests &interests = content.interests;
    result.interests.general = sanitizeParagraphs(interests.general, TextBounds::interest);
    result.interests.music = sanitizeParagraphs(interests.music, TextBounds::interest);
    result.interests.movies = sanitizeParagraphs(interests.movies, TextBounds::interest);
    result.interests.television = sanitizeParagraphs(interests.television, TextBounds::interest);
    result.interests.books = sanitizeParagraphs(interests.books, TextBounds::interest);
    result.interests.heroes = sanitizeParagraphs(interests.heroes, TextBounds::interest);

    const Details &details = content.details;
    result.details.hereFor = details.hereFor & hereForMask;
    result.details.hometown = sanitizeLine(details.hometown, TextBounds::detail);
    result.details.zodiac = validOr(details.zodiac, Zodiac::Pisces, Zodiac::NoZodiac);
    result.details.occupation = sanitizeLine(details.occupation, TextBounds::detail);
    result.details.education = sanitizeLine(details.education, TextBounds::detail);
    result.details.languages = sanitizeLine(details.languages, TextBounds::detail);

    result.aboutMe = sanitizeParagraphs(content.aboutMe, TextBounds::aboutMe);
    result.meet = sanitizeParagraphs(content.meet, TextBounds::meet);
    result.songTitle = sanitizeLine(content.songTitle, TextBounds::songTitle);
    result.songArtist = sanitizeLine(content.songArtist, TextBounds::songArtist);
    return result;
}

// ---------------------------------------------------------------------------
// Presets (SPEC §10; one setter per column of the design table)
// ---------------------------------------------------------------------------

void setBackground(Theme &theme, BackgroundKind kind, quint32 colour1, quint32 colour2)
{
    theme.backgroundKind = kind;
    theme.backgroundColor1 = colour1;
    theme.backgroundColor2 = colour2;
}

void setMotif(Theme &theme, Motif motif, quint32 ink, quint8 opacity, MotifScale scale)
{
    theme.motif = motif;
    theme.motifInk = ink;
    theme.motifOpacity = opacity;
    theme.motifScale = scale;
}

void setBox(Theme &theme, quint32 fill, quint8 opacity, quint32 border, quint8 width, BorderStyle style,
            BoxRadius radius)
{
    theme.boxFill = fill;
    theme.boxOpacity = opacity;
    theme.borderColor = border;
    theme.borderWidth = width;
    theme.borderStyle = style;
    theme.boxRadius = radius;
}

// Without an "alt" column in the table the wide column repeats the main strip
// and border, so the alt fields equal the main ones. Call after setBox.
void setStrip(Theme &theme, HeaderStyle style, quint32 fill, quint32 text)
{
    theme.headerStyle = style;
    theme.headerFill = fill;
    theme.headerText = text;
    theme.altHeader = false;
    theme.altHeaderFill = fill;
    theme.altHeaderText = text;
    theme.altBorderColor = theme.borderColor;
}

void setAltStrip(Theme &theme, quint32 fill, quint32 text, quint32 border)
{
    theme.altHeader = true;
    theme.altHeaderFill = fill;
    theme.altHeaderText = text;
    theme.altBorderColor = border;
}

void setFonts(Theme &theme, Font heading, Font body, Font name)
{
    theme.headingFont = heading;
    theme.bodyFont = body;
    theme.nameFont = name;
}

void setInks(Theme &theme, quint32 body, quint32 label, quint32 link)
{
    theme.bodyColor = body;
    theme.labelColor = label;
    theme.linkColor = link;
}

void setName(Theme &theme, NameSize size, quint32 colour, quint32 colour2, NameEffect effect,
             Flourish flourish)
{
    theme.nameSize = size;
    theme.nameColor = colour;
    theme.nameColor2 = colour2;
    theme.nameEffect = effect;
    theme.nameFlourish = flourish;
}

struct MoodInfo {
    const char16_t *label;
    MoodFace face;
};

// Indexed by Mood value - 1.
constexpr std::array<MoodInfo, maxMood> moods{{
    {u"amused", MoodFace::GrinFace},       {u"artistic", MoodFace::SmileFace},
    {u"blah", MoodFace::FlatFace},         {u"bored", MoodFace::FlatFace},
    {u"bouncy", MoodFace::GrinFace},       {u"bubbly", MoodFace::GrinFace},
    {u"busy", MoodFace::FlatFace},         {u"calm", MoodFace::SmileFace},
    {u"cheerful", MoodFace::GrinFace},     {u"chill", MoodFace::SmileFace},
    {u"confused", MoodFace::FlatFace},     {u"content", MoodFace::SmileFace},
    {u"cranky", MoodFace::FrownFace},      {u"creative", MoodFace::SmileFace},
    {u"curious", MoodFace::SmileFace},     {u"determined", MoodFace::FlatFace},
    {u"dreamy", MoodFace::SleepyFace},     {u"energetic", MoodFace::GrinFace},
    {u"excited", MoodFace::GrinFace},      {u"flirty", MoodFace::WinkFace},
    {u"geeky", MoodFace::SmileFace},       {u"giddy", MoodFace::GrinFace},
    {u"grateful", MoodFace::SmileFace},    {u"groggy", MoodFace::SleepyFace},
    {u"happy", MoodFace::GrinFace},        {u"hopeful", MoodFace::SmileFace},
    {u"hungry", MoodFace::FlatFace},       {u"hyper", MoodFace::GrinFace},
    {u"inspired", MoodFace::SmileFace},    {u"lazy", MoodFace::SleepyFace},
    {u"loved", MoodFace::SmileFace},       {u"mellow", MoodFace::SmileFace},
    {u"melancholy", MoodFace::FrownFace},  {u"nerdy", MoodFace::SmileFace},
    {u"nostalgic", MoodFace::SmileFace},   {u"optimistic", MoodFace::SmileFace},
    {u"peaceful", MoodFace::SmileFace},    {u"pensive", MoodFace::FlatFace},
    {u"rebellious", MoodFace::WinkFace},   {u"relaxed", MoodFace::SmileFace},
    {u"rockin'", MoodFace::GrinFace},      {u"sleepy", MoodFace::SleepyFace},
    {u"silly", MoodFace::WinkFace},        {u"stressed", MoodFace::FrownFace},
    {u"thankful", MoodFace::SmileFace},    {u"tired", MoodFace::SleepyFace},
    {u"weird", MoodFace::WinkFace},        {u"working", MoodFace::FlatFace},
}};

[[nodiscard]] const MoodInfo *moodInfo(Mood mood) noexcept
{
    const int id = static_cast<int>(mood);
    return id >= 1 && id <= maxMood ? &moods[static_cast<size_t>(id - 1)] : nullptr;
}

} // namespace

Theme defaultTheme()
{
    return Theme{};
}

Theme aeroSkyTheme(bool dark)
{
    Theme theme;
    theme.adaptive = false;
    if (!dark)
        return theme;
    setBackground(theme, BackgroundKind::PatternBackground, 0x16293A, 0x0F1B26);
    setMotif(theme, Motif::Bubbles, 0x3F6F94, 45, MotifScale::MediumMotif);
    setBox(theme, 0x1B2A37, 90, 0x567F9D, 1, BorderStyle::SolidBorder, BoxRadius::SoftCorners);
    setStrip(theme, HeaderStyle::GlossHeader, 0x355871, 0xE3F1FB);
    setInks(theme, 0xE0EAF3, 0x9CC9EB, 0x80BDE6);
    setName(theme, NameSize::MediumName, 0xF2F8FC, 0x4B93CC, NameEffect::GlowName, Flourish::NoFlourish);
    return theme;
}

Theme presetTheme(Preset preset)
{
    Theme theme;
    if (preset == Preset::AeroSkyPreset || !isShippedPreset(preset))
        return theme; // Aero Sky is the default theme and the only adaptive one
    theme.adaptive = false;
    switch (preset) {
    case Preset::AeroSkyPreset:
    case Preset::CustomPreset:
        break;
    case Preset::Classic06Preset:
        // "solid #E5E5E5", motif none: the motif fields keep their (unused) defaults.
        setBackground(theme, BackgroundKind::SolidBackground, 0xE5E5E5, 0xE5E5E5);
        setBox(theme, 0xFFFFFF, 100, 0x6699CC, 1, BorderStyle::SolidBorder, BoxRadius::SquareCorners);
        setStrip(theme, HeaderStyle::FlatHeader, 0x4574A8, 0xFFFFFF);
        setAltStrip(theme, 0xFFCC99, 0x9A3B00, 0xF2B27A);
        setFonts(theme, Font::InterfaceFont, Font::InterfaceFont, Font::InterfaceFont);
        setInks(theme, 0x000000, 0x1F4E7A, 0x003399);
        setName(theme, NameSize::MediumName, 0x000000, 0x000000, NameEffect::PlainName, Flourish::NoFlourish);
        theme.tableStyle = TableStyle::CellTable;
        break;
    case Preset::SceneQueenPreset:
        setBackground(theme, BackgroundKind::PatternBackground, 0x0A0A0C, 0x0A0A0C);
        setMotif(theme, Motif::Skulls, 0xFF4FA8, 80, MotifScale::MediumMotif);
        setBox(theme, 0x000000, 94, 0xFF3EA5, 1, BorderStyle::SolidBorder, BoxRadius::SlightCorners);
        setStrip(theme, HeaderStyle::GlossHeader, 0x5E1139, 0xFFC2E2);
        setFonts(theme, Font::RoundedFont, Font::InterfaceFont, Font::RoundedFont);
        setInks(theme, 0xF4F4F4, 0xFF6FB8, 0xFF6FB8);
        setName(theme, NameSize::LargeName, 0xFF5CB3, 0xFFFFFF, NameEffect::GlitterName, Flourish::XxxFlourish);
        theme.tableStyle = TableStyle::LineTable;
        break;
    case Preset::NeonZebraPreset:
        setBackground(theme, BackgroundKind::PatternBackground, 0x000000, 0x000000);
        setMotif(theme, Motif::Zebra, 0xFFFFFF, 24, MotifScale::LargeMotif);
        setBox(theme, 0x07070A, 92, 0x00E5FF, 2, BorderStyle::SolidBorder, BoxRadius::SlightCorners);
        setStrip(theme, HeaderStyle::GlossHeader, 0x2A0058, 0xB6FF00);
        setFonts(theme, Font::MarkerFont, Font::InterfaceFont, Font::MarkerFont);
        setInks(theme, 0xF2F2F2, 0x00E5FF, 0xFF5FC9);
        setName(theme, NameSize::LargeName, 0xB6FF00, 0xFF4FC3, NameEffect::ShadowName, Flourish::StarFlourish);
        theme.tableStyle = TableStyle::LineTable;
        break;
    case Preset::MidnightEmoPreset:
        setBackground(theme, BackgroundKind::PatternBackground, 0x0E0E10, 0x0E0E10);
        setMotif(theme, Motif::BrokenHearts, 0xB3122E, 62, MotifScale::MediumMotif);
        setBox(theme, 0x141416, 94, 0xB3122E, 1, BorderStyle::DashedBorder, BoxRadius::SquareCorners);
        setStrip(theme, HeaderStyle::GradientHeader, 0x3A0E16, 0xFF7A8A);
        setFonts(theme, Font::TypewriterFont, Font::TypewriterFont, Font::GothicFont);
        setInks(theme, 0xD9D9DC, 0xFF5F72, 0xFF8796);
        setName(theme, NameSize::LargeName, 0xECECEF, 0xC0142F, NameEffect::GlowName, Flourish::NoFlourish);
        theme.tableStyle = TableStyle::LineTable;
        break;
    case Preset::GlitterGirlPreset:
        setBackground(theme, BackgroundKind::PatternBackground, 0xFFC3E1, 0xFFE6F3);
        setMotif(theme, Motif::Sparkles, 0xFF7DBE, 70, MotifScale::MediumMotif);
        setBox(theme, 0xFFFFFF, 94, 0xFF69B4, 2, BorderStyle::DottedBorder, BoxRadius::RoundCorners);
        setStrip(theme, HeaderStyle::GlossHeader, 0xB5165F, 0xFFFFFF);
        setFonts(theme, Font::RoundedFont, Font::RoundedFont, Font::ScriptFont);
        setInks(theme, 0x4A1033, 0xB5165F, 0xB5165F);
        setName(theme, NameSize::LargeName, 0xFF2E97, 0xFFE3F1, NameEffect::GlitterName, Flourish::HeartFlourish);
        theme.tableStyle = TableStyle::CellTable;
        theme.ambient = Ambient::FallingHearts;
        break;
    case Preset::SafetyPinPreset:
        setBackground(theme, BackgroundKind::PatternBackground, 0x161616, 0x161616);
        setMotif(theme, Motif::Checkerboard, 0xF2F2F2, 14, MotifScale::MediumMotif);
        setBox(theme, 0x0F0F0F, 94, 0xE0212E, 3, BorderStyle::DoubleBorder, BoxRadius::SquareCorners);
        setStrip(theme, HeaderStyle::FlatHeader, 0xB3121E, 0xFFFFFF);
        setFonts(theme, Font::MarkerFont, Font::TypewriterFont, Font::MarkerFont);
        setInks(theme, 0xEDEDED, 0xFFD400, 0xFFD400);
        setName(theme, NameSize::ExtraLargeName, 0xFFFFFF, 0xE0212E, NameEffect::ShadowName, Flourish::NoFlourish);
        theme.tableStyle = TableStyle::LineTable;
        break;
    case Preset::HeadlinerPreset:
        setBackground(theme, BackgroundKind::PatternBackground, 0x0B0E12, 0x151B22);
        setMotif(theme, Motif::Halftone, 0x3A4856, 55, MotifScale::MediumMotif);
        setBox(theme, 0x0E1116, 94, 0x394654, 1, BorderStyle::SolidBorder, BoxRadius::SlightCorners);
        setStrip(theme, HeaderStyle::GradientHeader, 0x232D38, 0xF5C518);
        setFonts(theme, Font::SerifFont, Font::InterfaceFont, Font::SerifFont);
        setInks(theme, 0xE4E7EB, 0xF5C518, 0x5CC8FF);
        setName(theme, NameSize::ExtraLargeName, 0xF5C518, 0x000000, NameEffect::ShadowName, Flourish::NoFlourish);
        theme.tableStyle = TableStyle::LineTable;
        break;
    case Preset::LinenPreset:
        setBackground(theme, BackgroundKind::PatternBackground, 0xF3EFE7, 0xECE5D8);
        setMotif(theme, Motif::LinenWeave, 0xD6CAB4, 55, MotifScale::MediumMotif);
        setBox(theme, 0xFFFFFF, 97, 0xD8CFBF, 1, BorderStyle::SolidBorder, BoxRadius::SlightCorners);
        // "none; — / #3A342B": no strip; its fill is the box fill.
        setStrip(theme, HeaderStyle::NoHeader, 0xFFFFFF, 0x3A342B);
        setFonts(theme, Font::SerifFont, Font::InterfaceFont, Font::SerifFont);
        setInks(theme, 0x2E2A24, 0x85552A, 0x85552A);
        setName(theme, NameSize::LargeName, 0x2E2A24, 0x2E2A24, NameEffect::PlainName, Flourish::NoFlourish);
        theme.tableStyle = TableStyle::LineTable;
        break;
    case Preset::ChromeY2KPreset:
        setBackground(theme, BackgroundKind::PatternBackground, 0x050716, 0x1A1F5C);
        setMotif(theme, Motif::CyberGrid, 0x00E5FF, 40, MotifScale::MediumMotif);
        setBox(theme, 0x0A0F2E, 88, 0x00D8F0, 1, BorderStyle::SolidBorder, BoxRadius::SoftCorners);
        theme.boxGlow = true;
        setStrip(theme, HeaderStyle::GlossHeader, 0xC3CFDF, 0x0A0F2E);
        setFonts(theme, Font::FutureFont, Font::InterfaceFont, Font::FutureFont);
        setInks(theme, 0xE3ECFF, 0x7FE9FF, 0xB8FF3C);
        setName(theme, NameSize::ExtraLargeName, 0x39D5FF, 0x6F7F99, NameEffect::ChromeName, Flourish::NoFlourish);
        theme.tableStyle = TableStyle::LineTable;
        theme.ambient = Ambient::FallingStars;
        break;
    }
    return theme;
}

Page applyPreset(Page page, Preset preset)
{
    if (!isShippedPreset(preset))
        return page;
    page.theme = presetTheme(preset);
    page.preset = preset;
    page.layout = preset == Preset::SafetyPinPreset ? Layout::FlippedLayout : Layout::ClassicLayout;
    if (preset == Preset::HeadlinerPreset) {
        // The band-page look: the song leads the wide column (after the fixed
        // banner). Its visibility stays whatever the owner chose.
        QVector<ModulePlacement> modules = normalizedModules(page.modules);
        const auto song = std::find_if(modules.begin(), modules.end(), [](const ModulePlacement &placement) {
            return placement.module == Module::SongModule;
        });
        ModulePlacement placement = *song;
        modules.erase(song);
        placement.column = Column::WideColumn;
        const auto firstWide = std::find_if(modules.begin(), modules.end(), [](const ModulePlacement &entry) {
            return entry.column == Column::WideColumn;
        });
        modules.insert(firstWide, placement);
        page.modules = modules;
    }
    return page;
}

bool styleDiffersFromPreset(const Page &page)
{
    if (!isShippedPreset(page.preset))
        return true;
    return normalizedTheme(page.theme, normalizedBackground(page.background).isSet())
        != presetTheme(page.preset);
}

QVector<Motif> motifFamily(Preset preset)
{
    switch (preset) {
    case Preset::AeroSkyPreset:
        return {Motif::Bubbles, Motif::PolkaDots, Motif::Pinstripes};
    case Preset::Classic06Preset:
        return {Motif::Pinstripes, Motif::Checkerboard, Motif::PolkaDots};
    case Preset::SceneQueenPreset:
        return {Motif::Skulls, Motif::Stars, Motif::Hearts};
    case Preset::NeonZebraPreset:
        return {Motif::Zebra, Motif::Leopard, Motif::Checkerboard};
    case Preset::MidnightEmoPreset:
        return {Motif::BrokenHearts, Motif::Skulls, Motif::Pinstripes};
    case Preset::GlitterGirlPreset:
        return {Motif::Sparkles, Motif::Hearts, Motif::Flowers};
    case Preset::SafetyPinPreset:
        return {Motif::Checkerboard, Motif::Plaid, Motif::Stars};
    case Preset::HeadlinerPreset:
        return {Motif::Halftone, Motif::MusicNotes, Motif::Stars};
    case Preset::LinenPreset:
        return {Motif::LinenWeave, Motif::Flowers, Motif::Pinstripes};
    case Preset::ChromeY2KPreset:
        return {Motif::CyberGrid, Motif::Stars, Motif::Sparkles};
    case Preset::CustomPreset:
        break;
    }
    return {};
}

QVector<ModulePlacement> defaultModules()
{
    return {
        {Module::HandleModule, Column::NarrowColumn, true},
        {Module::SongModule, Column::NarrowColumn, true},
        {Module::InterestsModule, Column::NarrowColumn, true},
        {Module::DetailsModule, Column::NarrowColumn, true},
        {Module::BlurbsModule, Column::WideColumn, true},
        {Module::TopFriendsModule, Column::WideColumn, true},
    };
}

Page defaultPage()
{
    return normalized(Page{});
}

Page normalized(Page page)
{
    page.revision = std::clamp<qint64>(page.revision, 0, maxRevision);
    page.publishedAtMs = std::clamp<qint64>(page.publishedAtMs, 0, maxRevision);
    if (!isShippedPreset(page.preset))
        page.preset = Preset::CustomPreset;
    page.background = normalizedBackground(page.background);
    page.song = normalizedSong(page.song);
    page.theme = normalizedTheme(page.theme, page.background.isSet());
    page.layout = validOr(page.layout, Layout::SingleLayout, Layout::ClassicLayout);
    page.modules = normalizedModules(page.modules);
    page.content = normalizedContent(page.content);
    page.topFriends = normalizedTopFriends(page.topFriends);
    return page;
}

bool samePublishedContent(const Page &a, const Page &b)
{
    Page left = normalized(a);
    Page right = normalized(b);
    left.revision = right.revision = 0;
    left.publishedAtMs = right.publishedAtMs = 0;
    return left == right;
}

qint64 nextRevision(qint64 previous, qint64 nowMs)
{
    const qint64 floor = previous < 0 ? 1 : (previous >= maxRevision ? maxRevision : previous + 1);
    return std::min(std::max(floor, nowMs), maxRevision);
}

int radiusPixels(BoxRadius radius)
{
    switch (radius) {
    case BoxRadius::SquareCorners:
        return 0;
    case BoxRadius::SlightCorners:
        return 3;
    case BoxRadius::SoftCorners:
        return 6;
    case BoxRadius::RoundCorners:
        return 10;
    }
    return 6;
}

bool isBodySafe(Font font)
{
    return font == Font::InterfaceFont || font == Font::RoundedFont || font == Font::TypewriterFont
        || font == Font::SerifFont;
}

bool isHeadingCapable(Font font)
{
    return static_cast<int>(font) <= static_cast<int>(Font::MarkerFont) && font != Font::PixelFont;
}

QString presetName(Preset preset)
{
    switch (preset) {
    case Preset::AeroSkyPreset:
        return QStringLiteral("Aero Sky");
    case Preset::Classic06Preset:
        return QStringLiteral("Classic '06");
    case Preset::SceneQueenPreset:
        return QStringLiteral("Scene Queen");
    case Preset::NeonZebraPreset:
        return QStringLiteral("Neon Zebra");
    case Preset::MidnightEmoPreset:
        return QStringLiteral("Midnight Emo");
    case Preset::GlitterGirlPreset:
        return QStringLiteral("Glitter Girl");
    case Preset::SafetyPinPreset:
        return QStringLiteral("Safety Pin");
    case Preset::HeadlinerPreset:
        return QStringLiteral("Headliner");
    case Preset::LinenPreset:
        return QStringLiteral("Linen");
    case Preset::ChromeY2KPreset:
        return QStringLiteral("Chrome Y2K");
    case Preset::CustomPreset:
        break;
    }
    return QStringLiteral("Custom");
}

QString presetSlug(Preset preset)
{
    switch (preset) {
    case Preset::AeroSkyPreset:
        return QStringLiteral("aero-sky");
    case Preset::Classic06Preset:
        return QStringLiteral("classic-06");
    case Preset::SceneQueenPreset:
        return QStringLiteral("scene-queen");
    case Preset::NeonZebraPreset:
        return QStringLiteral("neon-zebra");
    case Preset::MidnightEmoPreset:
        return QStringLiteral("midnight-emo");
    case Preset::GlitterGirlPreset:
        return QStringLiteral("glitter-girl");
    case Preset::SafetyPinPreset:
        return QStringLiteral("safety-pin");
    case Preset::HeadlinerPreset:
        return QStringLiteral("headliner");
    case Preset::LinenPreset:
        return QStringLiteral("linen");
    case Preset::ChromeY2KPreset:
        return QStringLiteral("chrome-y2k");
    case Preset::CustomPreset:
        break;
    }
    return QStringLiteral("custom");
}

QString motifName(Motif motif)
{
    switch (motif) {
    case Motif::Stars:
        return QStringLiteral("Stars");
    case Motif::Hearts:
        return QStringLiteral("Hearts");
    case Motif::Skulls:
        return QStringLiteral("Skulls & stars");
    case Motif::Checkerboard:
        return QStringLiteral("Checkerboard");
    case Motif::Zebra:
        return QStringLiteral("Zebra");
    case Motif::Leopard:
        return QStringLiteral("Leopard");
    case Motif::PolkaDots:
        return QStringLiteral("Polka dots");
    case Motif::Pinstripes:
        return QStringLiteral("Pinstripes");
    case Motif::Plaid:
        return QStringLiteral("Plaid");
    case Motif::Sparkles:
        return QStringLiteral("Sparkles");
    case Motif::MusicNotes:
        return QStringLiteral("Music notes");
    case Motif::Flowers:
        return QStringLiteral("Flowers");
    case Motif::Halftone:
        return QStringLiteral("Halftone");
    case Motif::CyberGrid:
        return QStringLiteral("Cyber grid");
    case Motif::Bubbles:
        return QStringLiteral("Bubbles");
    case Motif::BrokenHearts:
        return QStringLiteral("Broken hearts");
    case Motif::LinenWeave:
        return QStringLiteral("Linen");
    }
    return {};
}

QString fontName(Font font)
{
    switch (font) {
    case Font::InterfaceFont:
        return QStringLiteral("Standard");
    case Font::RoundedFont:
        return QStringLiteral("Rounded");
    case Font::ScriptFont:
        return QStringLiteral("Script");
    case Font::TypewriterFont:
        return QStringLiteral("Typewriter");
    case Font::PixelFont:
        return QStringLiteral("Pixel");
    case Font::GothicFont:
        return QStringLiteral("Gothic");
    case Font::FutureFont:
        return QStringLiteral("Future");
    case Font::SerifFont:
        return QStringLiteral("Serif");
    case Font::MarkerFont:
        return QStringLiteral("Marker");
    }
    return {};
}

QString fontCategory(Font font)
{
    switch (font) {
    case Font::InterfaceFont:
        return QStringLiteral("sans");
    case Font::RoundedFont:
        return QStringLiteral("rounded");
    case Font::ScriptFont:
        return QStringLiteral("script");
    case Font::TypewriterFont:
        return QStringLiteral("monospace");
    case Font::PixelFont:
        return QStringLiteral("pixel");
    case Font::GothicFont:
        return QStringLiteral("blackletter");
    case Font::FutureFont:
        return QStringLiteral("futuristic");
    case Font::SerifFont:
        return QStringLiteral("serif");
    case Font::MarkerFont:
        return QStringLiteral("handwritten");
    }
    return {};
}

QString zodiacName(Zodiac zodiac)
{
    switch (zodiac) {
    case Zodiac::NoZodiac:
        break;
    case Zodiac::Aries:
        return QStringLiteral("Aries");
    case Zodiac::Taurus:
        return QStringLiteral("Taurus");
    case Zodiac::Gemini:
        return QStringLiteral("Gemini");
    case Zodiac::Cancer:
        return QStringLiteral("Cancer");
    case Zodiac::Leo:
        return QStringLiteral("Leo");
    case Zodiac::Virgo:
        return QStringLiteral("Virgo");
    case Zodiac::Libra:
        return QStringLiteral("Libra");
    case Zodiac::Scorpio:
        return QStringLiteral("Scorpio");
    case Zodiac::Sagittarius:
        return QStringLiteral("Sagittarius");
    case Zodiac::Capricorn:
        return QStringLiteral("Capricorn");
    case Zodiac::Aquarius:
        return QStringLiteral("Aquarius");
    case Zodiac::Pisces:
        return QStringLiteral("Pisces");
    }
    return {};
}

QString moodName(Mood mood)
{
    const MoodInfo *info = moodInfo(mood);
    return info ? QString::fromUtf16(info->label) : QString();
}

MoodFace moodFace(Mood mood)
{
    const MoodInfo *info = moodInfo(mood);
    return info ? info->face : MoodFace::SmileFace;
}

QString hereForText(quint8 bits)
{
    static constexpr std::array<std::pair<HereFor, const char *>, 6> labels{{
        {HereForFriends, "Friends"},
        {HereForNetworking, "Networking"},
        {HereForChatting, "Chatting"},
        {HereForGaming, "Gaming"},
        {HereForMusic, "Music"},
        {HereForCollaborating, "Collaborating"},
    }};
    QStringList chosen;
    for (const auto &[bit, label] : labels) {
        if (bits & bit)
            chosen.push_back(QString::fromLatin1(label));
    }
    return chosen.join(QStringLiteral(", "));
}

QString moduleName(Module module)
{
    switch (module) {
    case Module::HandleModule:
        return QStringLiteral("OpenChat handle");
    case Module::SongModule:
        return QStringLiteral("Profile song");
    case Module::InterestsModule:
        return QStringLiteral("Interests");
    case Module::DetailsModule:
        return QStringLiteral("Details");
    case Module::BlurbsModule:
        return QStringLiteral("Blurbs");
    case Module::TopFriendsModule:
        return QStringLiteral("Top Friends");
    }
    return {};
}

QString flourishPrefix(Flourish flourish)
{
    switch (flourish) {
    case Flourish::NoFlourish:
        break;
    case Flourish::StarFlourish:
        return QStringLiteral("★ ");
    case Flourish::XxxFlourish:
        return QStringLiteral("xXx ");
    case Flourish::HeartFlourish:
        return QStringLiteral("♥ ");
    case Flourish::TildeFlourish:
        return QStringLiteral("~* ");
    case Flourish::NoteFlourish:
        return QStringLiteral("♫ ");
    case Flourish::FlowerFlourish:
        return QStringLiteral("✿ ");
    }
    return {};
}

QString flourishSuffix(Flourish flourish)
{
    switch (flourish) {
    case Flourish::NoFlourish:
        break;
    case Flourish::StarFlourish:
        return QStringLiteral(" ★");
    case Flourish::XxxFlourish:
        return QStringLiteral(" xXx");
    case Flourish::HeartFlourish:
        return QStringLiteral(" ♥");
    case Flourish::TildeFlourish:
        return QStringLiteral(" *~");
    case Flourish::NoteFlourish:
        return QStringLiteral(" ♫");
    case Flourish::FlowerFlourish:
        return QStringLiteral(" ✿");
    }
    return {};
}

QString sanitizeLine(const QString &text, qsizetype maxLength)
{
    return sanitize(text, maxLength, false, true);
}

QString sanitizeParagraphs(const QString &text, qsizetype maxLength)
{
    return sanitize(text, maxLength, true, true);
}

QString sanitizeLive(const QString &text, qsizetype maxLength, bool multiLine)
{
    return sanitize(text, maxLength, multiLine, false);
}

} // namespace OpenChat::Profile
