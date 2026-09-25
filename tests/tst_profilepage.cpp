#include "domain/ProfilePage.h"
#include "domain/ProfilePageCodec.h"
#include "domain/ProfileUpdate.h"
#include "domain/SongContainer.h"

#include <QCborArray>
#include <QCborMap>
#include <QCborValue>
#include <QMetaEnum>
#include <QSet>
#include <QtEndian>
#include <QtTest>

#include <functional>

using namespace OpenChat;
using namespace Qt::StringLiterals;
using Profile::Column;
using Profile::Font;
using Profile::Module;
using Profile::ModulePlacement;
using Profile::Page;
using Profile::Preset;
using Profile::Theme;

namespace {

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

QByteArray filled(qsizetype size, char byte)
{
    return QByteArray(size, byte);
}

QByteArray hashOf(char byte)
{
    return filled(32, byte);
}

QByteArray accountOf(char byte)
{
    return filled(16, byte);
}

// Every knob away from its default, each colour distinct, so a field written
// under another field's key cannot round-trip by accident.
Theme nonDefaultTheme()
{
    Theme theme;
    theme.backgroundKind = Profile::BackgroundKind::ImageBackground;
    theme.backgroundColor1 = 0x010203;
    theme.backgroundColor2 = 0x040506;
    theme.motif = Profile::Motif::LinenWeave;
    theme.motifInk = 0x070809;
    theme.motifOpacity = 80;
    theme.motifScale = Profile::MotifScale::LargeMotif;
    theme.imageMode = Profile::ImageMode::CenterImage;
    theme.imageFixed = false;
    theme.boxFill = 0x0A0B0C;
    theme.boxOpacity = 60;
    theme.borderColor = 0x0D0E0F;
    theme.borderWidth = 4;
    theme.borderStyle = Profile::BorderStyle::DoubleBorder;
    theme.boxRadius = Profile::BoxRadius::RoundCorners;
    theme.boxGlow = true;
    theme.tableStyle = Profile::TableStyle::LineTable;
    theme.headerStyle = Profile::HeaderStyle::NoHeader;
    theme.headerFill = 0x101112;
    theme.headerText = 0x131415;
    theme.altHeader = true;
    theme.altHeaderFill = 0x161718;
    theme.altHeaderText = 0x191A1B;
    theme.altBorderColor = 0x1C1D1E;
    theme.headingFont = Font::GothicFont;
    theme.bodyFont = Font::SerifFont;
    theme.textSize = Profile::TextSize::LargeText;
    theme.bodyColor = 0x1F2021;
    theme.labelColor = 0x222324;
    theme.linkColor = 0x252627;
    theme.nameFont = Font::PixelFont;
    theme.nameColor = 0x28292A;
    theme.nameColor2 = 0x2B2C2D;
    theme.nameEffect = Profile::NameEffect::ChromeName;
    theme.nameSize = Profile::NameSize::ExtraLargeName;
    theme.nameFlourish = Profile::Flourish::FlowerFlourish;
    theme.ambient = Profile::Ambient::FloatingSparkles;
    theme.adaptive = false;
    return theme;
}

// A page with every field set and already in normal form, so a round trip
// must reproduce it exactly.
Page richPage()
{
    Page page;
    page.revision = 1'727'000'000'123;
    page.publishedAtMs = 1'727'000'000'456;
    page.preset = Preset::GlitterGirlPreset;
    page.theme = nonDefaultTheme();
    page.layout = Profile::Layout::FlippedLayout;
    page.modules = {
        {Module::BlurbsModule, Column::NarrowColumn, true},
        {Module::SongModule, Column::NarrowColumn, false},
        {Module::HandleModule, Column::NarrowColumn, true},
        {Module::TopFriendsModule, Column::WideColumn, true},
        {Module::InterestsModule, Column::WideColumn, false},
        {Module::DetailsModule, Column::WideColumn, true},
    };
    Profile::Content &content = page.content;
    content.displayName = QStringLiteral("Dana Whitfield");
    content.headline = QStringLiteral("Zines, drums and bad puns");
    content.infoLines = {QStringLiteral("Brooklyn"), QStringLiteral("Drummer"), QStringLiteral("she/her")};
    content.mood = Profile::Mood::MoodRockin;
    content.interests = {QStringLiteral("Zines"), QStringLiteral("Sleater-Kinney"), QStringLiteral("Heathers"),
                         QStringLiteral("Twin Peaks"), QStringLiteral("Just Kids"), QStringLiteral("Kathleen Hanna")};
    content.details.hereFor = Profile::HereForFriends | Profile::HereForMusic;
    content.details.hometown = QStringLiteral("Olympia");
    content.details.zodiac = Profile::Zodiac::Leo;
    content.details.occupation = QStringLiteral("Printer");
    content.details.education = QStringLiteral("Evergreen");
    content.details.languages = QStringLiteral("English, Spanish");
    content.aboutMe = QStringLiteral("Hi.\n\nI make zines.\nSometimes music.");
    content.meet = QStringLiteral("Drummers with vans.");
    content.songTitle = QStringLiteral("Rebel Girl");
    content.songArtist = QStringLiteral("Bikini Kill");
    page.topFriends = {{accountOf(1), QStringLiteral("Mia")}, {accountOf(2), QStringLiteral("Tom")},
                       {accountOf(3), QString()}};
    page.background = {hashOf('\xB1'), 200'000, 1600, 1000, 0};
    page.song = {hashOf('\x50'), 180'000, 0, 0, 44'800};
    return page;
}

QByteArray tagged(const QCborMap &map)
{
    return QByteArray(1, '\xFF') + QCborValue(map).toCbor();
}

QByteArray taggedRaw(const char *hex)
{
    return QByteArray(1, '\xFF') + QByteArray::fromHex(hex);
}

QCborMap bodyOf(const QByteArray &payload)
{
    return QCborValue::fromCbor(payload.mid(1)).toMap();
}

// Sets (or adds) the value at a path of map keys, rebuilding the maps above it.
QCborMap setIn(QCborMap map, QList<qint64> path, const QCborValue &value)
{
    const qint64 key = path.takeFirst();
    if (path.isEmpty())
        map[key] = value;
    else
        map[key] = setIn(map.value(key).toMap(), path, value);
    return map;
}

QCborMap removeIn(QCborMap map, QList<qint64> path)
{
    const qint64 key = path.takeFirst();
    if (path.isEmpty())
        map.remove(key);
    else
        map[key] = removeIn(map.value(key).toMap(), path);
    return map;
}

QCborMap coreMap()
{
    return bodyOf(encodePageCore(richPage()));
}

// A JPEG as far as the marker walk is concerned: SOI, APP0, DQT, SOF2, DHT,
// then `scans` × (SOS + entropy data with stuffed bytes and a restart
// marker), EOI. Pixels are never decoded on arrival, so none are needed.
// With `targetBytes`, the last scan's data is padded to that exact size.
QByteArray jpegLike(int scans, qsizetype targetBytes = 0)
{
    const auto segment = [](quint8 marker, int payloadBytes) {
        QByteArray bytes;
        bytes.append(char(0xFF));
        bytes.append(char(marker));
        bytes.append(char(((payloadBytes + 2) >> 8) & 0xFF));
        bytes.append(char((payloadBytes + 2) & 0xFF));
        bytes.append(filled(payloadBytes, '\x11'));
        return bytes;
    };
    QByteArray jpeg("\xFF\xD8", 2);
    jpeg += segment(0xE0, 14);
    jpeg += segment(0xDB, 65);
    jpeg += segment(0xC2, 15);
    jpeg += segment(0xC4, 28);
    for (int scan = 0; scan < scans; ++scan) {
        jpeg += segment(0xDA, 10);
        jpeg += QByteArray::fromHex("1234ff0056ffd07890ff00ab");
    }
    if (targetBytes > 0) {
        const qsizetype padding = targetBytes - jpeg.size() - 2;
        Q_ASSERT(padding >= 0);
        jpeg += filled(padding, '\x5A');
    }
    jpeg += QByteArray("\xFF\xD9", 2);
    return jpeg;
}

// A song container written byte by byte from the layout table, independently
// of encodeSongContainer, so the decoder is tested against the spec.
struct RawSong {
    QByteArray magic = "OCSG";
    quint8 version = 1;
    quint8 channels = 1;
    quint16 reserved = 0;
    quint32 sampleRate = 48'000;
    quint16 frameSamples = 2880;
    quint16 preSkip = 312;
    quint32 totalSamples = 10 * 2880 - 312; // exactly ten packets
    qint16 gainQ8 = 0;
    std::optional<quint32> packetCount;     // default: packets.size()
    QVector<QByteArray> packets = QVector<QByteArray>(10, filled(100, '\x42'));
    QByteArray trailing;
};

template<typename T>
void appendLe(QByteArray &out, T value)
{
    char bytes[sizeof(T)];
    qToLittleEndian(value, bytes);
    out.append(bytes, sizeof(T));
}

QByteArray rawSongBytes(const RawSong &song)
{
    QByteArray out = song.magic;
    appendLe<quint8>(out, song.version);
    appendLe<quint8>(out, song.channels);
    appendLe<quint16>(out, song.reserved);
    appendLe<quint32>(out, song.sampleRate);
    appendLe<quint16>(out, song.frameSamples);
    appendLe<quint16>(out, song.preSkip);
    appendLe<quint32>(out, song.totalSamples);
    appendLe<qint16>(out, song.gainQ8);
    appendLe<quint32>(out, song.packetCount.value_or(quint32(song.packets.size())));
    for (const QByteArray &packet : song.packets) {
        appendLe<quint16>(out, quint16(packet.size()));
        out += packet;
    }
    return out + song.trailing;
}

// 180 packets of 1272/1273 bytes: a container of exactly maxSongBytes.
RawSong largestSong()
{
    RawSong song;
    song.totalSamples = 180 * 2880 - 312;
    song.packets.clear();
    for (int i = 0; i < 180; ++i)
        song.packets.push_back(filled(i < 150 ? 1272 : 1273, '\x42'));
    return song;
}

PageMediaMessage mediaOf(Profile::MediaKind kind, const QByteArray &data)
{
    return {kind, pageMediaHash(data), data};
}

QCborMap mediaMap(Profile::MediaKind kind, const QByteArray &data)
{
    return bodyOf(encodePageMedia(mediaOf(kind, data)));
}

// ---------------------------------------------------------------------------
// The SPEC §10 preset table, written the way the design table reads, and
// turned into Theme values by ARCH §2.3's mapping rules. Kept independent of
// ProfilePage.cpp's transcription so a typo in either one fails the test.
// ---------------------------------------------------------------------------

struct DesignRow {
    const char *background; // "pattern #A #B" (gradient) | "solid #X" | "#X" (one colour under a motif)
    const char *motif;      // "name #INK percent S|M|L" | "none"
    const char *box;        // "#FILL percent #BORDER width style radius [neon]"
    const char *strip;      // "style #FILL #TEXT" | "none - #TEXT"
    const char *alt;        // "" | "#FILL #TEXT #BORDER"
    const char *fonts;      // "heading body name"
    const char *inks;       // "#BODY #LABEL #LINK"
    const char *name;       // "size #COLOUR #COLOUR2|- effect [flourish]"
    const char *extras;     // "Cells|Lines [falling-hearts|falling-stars]"
};

quint32 colourOf(const QString &token)
{
    Q_ASSERT(token.startsWith(u'#') && token.size() == 7);
    return token.mid(1).toUInt(nullptr, 16);
}

template<typename Enum>
Enum lookup(const QHash<QString, Enum> &table, const QString &token)
{
    Q_ASSERT_X(table.contains(token), "lookup", qPrintable(token));
    return table.value(token);
}

Theme themeFromDesign(const DesignRow &row, bool adaptive)
{
    static const QHash<QString, Profile::Motif> motifs{
        {"bubbles", Profile::Motif::Bubbles},           {"skulls", Profile::Motif::Skulls},
        {"zebra", Profile::Motif::Zebra},               {"broken-hearts", Profile::Motif::BrokenHearts},
        {"sparkles", Profile::Motif::Sparkles},         {"checkerboard", Profile::Motif::Checkerboard},
        {"halftone", Profile::Motif::Halftone},         {"linen-weave", Profile::Motif::LinenWeave},
        {"cyber-grid", Profile::Motif::CyberGrid}};
    static const QHash<QString, Profile::MotifScale> scales{{"S", Profile::MotifScale::SmallMotif},
                                                             {"M", Profile::MotifScale::MediumMotif},
                                                             {"L", Profile::MotifScale::LargeMotif}};
    static const QHash<QString, Profile::BorderStyle> borders{{"solid", Profile::BorderStyle::SolidBorder},
                                                               {"dashed", Profile::BorderStyle::DashedBorder},
                                                               {"dotted", Profile::BorderStyle::DottedBorder},
                                                               {"double", Profile::BorderStyle::DoubleBorder}};
    static const QHash<QString, Profile::BoxRadius> radii{{"0", Profile::BoxRadius::SquareCorners},
                                                           {"3", Profile::BoxRadius::SlightCorners},
                                                           {"6", Profile::BoxRadius::SoftCorners},
                                                           {"10", Profile::BoxRadius::RoundCorners}};
    static const QHash<QString, Profile::HeaderStyle> strips{{"flat", Profile::HeaderStyle::FlatHeader},
                                                              {"gradient", Profile::HeaderStyle::GradientHeader},
                                                              {"gloss", Profile::HeaderStyle::GlossHeader},
                                                              {"none", Profile::HeaderStyle::NoHeader}};
    static const QHash<QString, Font> fonts{{"Standard", Font::InterfaceFont}, {"Rounded", Font::RoundedFont},
                                            {"Script", Font::ScriptFont},     {"Typewriter", Font::TypewriterFont},
                                            {"Pixel", Font::PixelFont},       {"Gothic", Font::GothicFont},
                                            {"Future", Font::FutureFont},     {"Serif", Font::SerifFont},
                                            {"Marker", Font::MarkerFont}};
    static const QHash<QString, Profile::NameSize> sizes{{"M", Profile::NameSize::MediumName},
                                                          {"L", Profile::NameSize::LargeName},
                                                          {"XL", Profile::NameSize::ExtraLargeName}};
    static const QHash<QString, Profile::NameEffect> effects{
        {"none", Profile::NameEffect::PlainName},       {"glow", Profile::NameEffect::GlowName},
        {"outline", Profile::NameEffect::OutlineName},  {"gradient", Profile::NameEffect::GradientName},
        {"glitter", Profile::NameEffect::GlitterName},  {"chrome", Profile::NameEffect::ChromeName},
        {"shadow", Profile::NameEffect::ShadowName}};
    static const QHash<QString, Profile::Flourish> flourishes{
        {"none", Profile::Flourish::NoFlourish}, {"★", Profile::Flourish::StarFlourish},
        {"xXx", Profile::Flourish::XxxFlourish}, {"♥", Profile::Flourish::HeartFlourish}};

    const auto words = [](const char *text) { return QString::fromUtf8(text).split(u' ', Qt::SkipEmptyParts); };
    Theme theme; // fields the table does not mention keep their defaults

    const QStringList background = words(row.background);
    if (background.at(0) == u"solid") {
        theme.backgroundKind = Profile::BackgroundKind::SolidBackground;
        theme.backgroundColor1 = theme.backgroundColor2 = colourOf(background.at(1));
    } else if (background.at(0) == u"pattern") {
        theme.backgroundKind = Profile::BackgroundKind::PatternBackground;
        theme.backgroundColor1 = colourOf(background.at(1));
        theme.backgroundColor2 = colourOf(background.at(2));
    } else {
        theme.backgroundKind = Profile::BackgroundKind::PatternBackground;
        theme.backgroundColor1 = theme.backgroundColor2 = colourOf(background.at(0));
    }

    const QStringList motif = words(row.motif);
    if (motif.at(0) != u"none") {
        theme.motif = lookup(motifs, motif.at(0));
        theme.motifInk = colourOf(motif.at(1));
        theme.motifOpacity = quint8(motif.at(2).toUInt());
        theme.motifScale = lookup(scales, motif.at(3));
    }

    const QStringList box = words(row.box);
    theme.boxFill = colourOf(box.at(0));
    theme.boxOpacity = quint8(box.at(1).toUInt());
    theme.borderColor = colourOf(box.at(2));
    theme.borderWidth = quint8(box.at(3).toUInt());
    theme.borderStyle = lookup(borders, box.at(4));
    theme.boxRadius = lookup(radii, box.at(5));
    theme.boxGlow = box.size() > 6 && box.at(6) == u"neon";

    const QStringList strip = words(row.strip);
    theme.headerStyle = lookup(strips, strip.at(0));
    theme.headerFill = strip.at(1) == u"-" ? theme.boxFill : colourOf(strip.at(1));
    theme.headerText = colourOf(strip.at(2));
    const QStringList alt = words(row.alt);
    theme.altHeader = !alt.isEmpty();
    theme.altHeaderFill = alt.isEmpty() ? theme.headerFill : colourOf(alt.at(0));
    theme.altHeaderText = alt.isEmpty() ? theme.headerText : colourOf(alt.at(1));
    theme.altBorderColor = alt.isEmpty() ? theme.borderColor : colourOf(alt.at(2));

    const QStringList faces = words(row.fonts);
    theme.headingFont = lookup(fonts, faces.at(0));
    theme.bodyFont = lookup(fonts, faces.at(1));
    theme.nameFont = lookup(fonts, faces.at(2));

    const QStringList inks = words(row.inks);
    theme.bodyColor = colourOf(inks.at(0));
    theme.labelColor = colourOf(inks.at(1));
    theme.linkColor = colourOf(inks.at(2));

    const QStringList name = words(row.name);
    theme.nameSize = lookup(sizes, name.at(0));
    theme.nameColor = colourOf(name.at(1));
    theme.nameColor2 = name.at(2) == u"-" ? theme.nameColor : colourOf(name.at(2));
    theme.nameEffect = lookup(effects, name.at(3));
    theme.nameFlourish = name.size() > 4 ? lookup(flourishes, name.at(4)) : Profile::Flourish::NoFlourish;

    const QStringList extras = words(row.extras);
    theme.tableStyle = extras.at(0) == u"Cells" ? Profile::TableStyle::CellTable : Profile::TableStyle::LineTable;
    theme.ambient = Profile::Ambient::NoAmbient;
    if (extras.size() > 1)
        theme.ambient = extras.at(1) == u"falling-hearts" ? Profile::Ambient::FallingHearts
                                                           : Profile::Ambient::FallingStars;
    theme.adaptive = adaptive;
    return theme;
}

const DesignRow aeroSkyLightRow{"pattern #C7E1F5 #F1F8FD", "bubbles #94C6EC 45 M",
                                "#FFFFFF 88 #8DBBE0 1 solid 6", "gloss #9FCDEF #133A61", "",
                                "Standard Standard Standard", "#2B3B53 #1F4E79 #1F6FA3",
                                "M #1C3D63 #FFFFFF glow none", "Cells"};
const DesignRow aeroSkyDarkRow{"pattern #16293A #0F1B26", "bubbles #3F6F94 45 M",
                               "#1B2A37 90 #567F9D 1 solid 6", "gloss #355871 #E3F1FB", "",
                               "Standard Standard Standard", "#E0EAF3 #9CC9EB #80BDE6",
                               "M #F2F8FC #4B93CC glow", "Cells"};

// Compares field by field so a failure names the knob.
void compareThemes(const Theme &actual, const Theme &expected)
{
    QCOMPARE(actual.backgroundKind, expected.backgroundKind);
    QCOMPARE(actual.backgroundColor1, expected.backgroundColor1);
    QCOMPARE(actual.backgroundColor2, expected.backgroundColor2);
    QCOMPARE(actual.motif, expected.motif);
    QCOMPARE(actual.motifInk, expected.motifInk);
    QCOMPARE(actual.motifOpacity, expected.motifOpacity);
    QCOMPARE(actual.motifScale, expected.motifScale);
    QCOMPARE(actual.imageMode, expected.imageMode);
    QCOMPARE(actual.imageFixed, expected.imageFixed);
    QCOMPARE(actual.boxFill, expected.boxFill);
    QCOMPARE(actual.boxOpacity, expected.boxOpacity);
    QCOMPARE(actual.borderColor, expected.borderColor);
    QCOMPARE(actual.borderWidth, expected.borderWidth);
    QCOMPARE(actual.borderStyle, expected.borderStyle);
    QCOMPARE(actual.boxRadius, expected.boxRadius);
    QCOMPARE(actual.boxGlow, expected.boxGlow);
    QCOMPARE(actual.tableStyle, expected.tableStyle);
    QCOMPARE(actual.headerStyle, expected.headerStyle);
    QCOMPARE(actual.headerFill, expected.headerFill);
    QCOMPARE(actual.headerText, expected.headerText);
    QCOMPARE(actual.altHeader, expected.altHeader);
    QCOMPARE(actual.altHeaderFill, expected.altHeaderFill);
    QCOMPARE(actual.altHeaderText, expected.altHeaderText);
    QCOMPARE(actual.altBorderColor, expected.altBorderColor);
    QCOMPARE(actual.headingFont, expected.headingFont);
    QCOMPARE(actual.bodyFont, expected.bodyFont);
    QCOMPARE(actual.textSize, expected.textSize);
    QCOMPARE(actual.bodyColor, expected.bodyColor);
    QCOMPARE(actual.labelColor, expected.labelColor);
    QCOMPARE(actual.linkColor, expected.linkColor);
    QCOMPARE(actual.nameFont, expected.nameFont);
    QCOMPARE(actual.nameColor, expected.nameColor);
    QCOMPARE(actual.nameColor2, expected.nameColor2);
    QCOMPARE(actual.nameEffect, expected.nameEffect);
    QCOMPARE(actual.nameSize, expected.nameSize);
    QCOMPARE(actual.nameFlourish, expected.nameFlourish);
    QCOMPARE(actual.ambient, expected.ambient);
    QCOMPARE(actual.adaptive, expected.adaptive);
    QVERIFY(actual == expected); // catches a field this list forgot
}

QSet<Module> hiddenModules(const QVector<ModulePlacement> &modules)
{
    QSet<Module> hidden;
    for (const ModulePlacement &placement : modules) {
        if (!placement.visible)
            hidden.insert(placement.module);
    }
    return hidden;
}

QVector<Module> columnOf(const QVector<ModulePlacement> &modules, Column column)
{
    QVector<Module> result;
    for (const ModulePlacement &placement : modules) {
        if (placement.column == column)
            result.push_back(placement.module);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Text helpers
// ---------------------------------------------------------------------------

// Whether `text` is valid UTF-16: every surrogate is half of a pair.
bool isWellFormed(const QString &text)
{
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text.at(i).isHighSurrogate()) {
            if (i + 1 == text.size() || !text.at(i + 1).isLowSurrogate())
                return false;
            ++i;
        } else if (text.at(i).isLowSurrogate()) {
            return false;
        }
    }
    return true;
}

// Whether the unit at `index` belongs to the character before it: the second
// half of a surrogate pair, or a combining mark.
bool continuesACharacter(const QString &text, qsizetype index)
{
    const QChar unit = text.at(index);
    if (unit.isLowSurrogate())
        return true;
    char32_t codePoint = unit.unicode();
    if (unit.isHighSurrogate() && index + 1 < text.size())
        codePoint = QChar::surrogateToUcs4(unit, text.at(index + 1));
    const QChar::Category category = QChar::category(codePoint);
    return category == QChar::Mark_NonSpacing || category == QChar::Mark_SpacingCombining
        || category == QChar::Mark_Enclosing;
}

// Every text field of a page, with the sanitiser ARCH §1.7 gives it:
// paragraphs keep line breaks, single-line fields turn them into spaces. The
// accessor adds the list entry it needs (an info line, a Top Friend) so one
// text can be put into any field of an empty page.
struct TextField {
    const char *name;
    bool paragraphs;
    std::function<QString &(Page &)> text;
};

QVector<TextField> textFields()
{
    using P = Page;
    // The lines before `index` hold a filler, so the text keeps its position.
    const auto infoLine = [](int index) {
        return [index](P &p) -> QString & {
            while (p.content.infoLines.size() <= index)
                p.content.infoLines.push_back(u"x"_s);
            return p.content.infoLines[index];
        };
    };
    return {
        {"displayName", false, [](P &p) -> QString & { return p.content.displayName; }},
        {"headline", false, [](P &p) -> QString & { return p.content.headline; }},
        {"infoLines[0]", false, infoLine(0)},
        {"infoLines[1]", false, infoLine(1)},
        {"infoLines[2]", false, infoLine(2)},
        {"interests.general", true, [](P &p) -> QString & { return p.content.interests.general; }},
        {"interests.music", true, [](P &p) -> QString & { return p.content.interests.music; }},
        {"interests.movies", true, [](P &p) -> QString & { return p.content.interests.movies; }},
        {"interests.television", true, [](P &p) -> QString & { return p.content.interests.television; }},
        {"interests.books", true, [](P &p) -> QString & { return p.content.interests.books; }},
        {"interests.heroes", true, [](P &p) -> QString & { return p.content.interests.heroes; }},
        {"details.hometown", false, [](P &p) -> QString & { return p.content.details.hometown; }},
        {"details.occupation", false, [](P &p) -> QString & { return p.content.details.occupation; }},
        {"details.education", false, [](P &p) -> QString & { return p.content.details.education; }},
        {"details.languages", false, [](P &p) -> QString & { return p.content.details.languages; }},
        {"aboutMe", true, [](P &p) -> QString & { return p.content.aboutMe; }},
        {"meet", true, [](P &p) -> QString & { return p.content.meet; }},
        {"songTitle", false, [](P &p) -> QString & { return p.content.songTitle; }},
        {"songArtist", false, [](P &p) -> QString & { return p.content.songArtist; }},
        {"topFriends.name", false,
         [](P &p) -> QString & {
             if (p.topFriends.isEmpty())
                 p.topFriends.push_back({accountOf(1), QString()});
             return p.topFriends.first().name;
         }},
    };
}

} // namespace

class ProfilePageTest final : public QObject
{
    Q_OBJECT

private slots:
    // --- Model ---------------------------------------------------------------

    void defaultPageIsAdaptiveAeroSkyWithDefaultModules()
    {
        const Page page = Profile::defaultPage();
        QCOMPARE(page.revision, 0);
        QCOMPARE(page.preset, Preset::AeroSkyPreset);
        QCOMPARE(page.layout, Profile::Layout::ClassicLayout);
        QVERIFY(page.theme.adaptive);
        compareThemes(page.theme, themeFromDesign(aeroSkyLightRow, true));
        if (QTest::currentTestFailed())
            return;
        // The knobs the design table leaves out keep ARCH §1.5's defaults.
        QCOMPARE(page.theme.imageMode, Profile::ImageMode::FillImage);
        QVERIFY(page.theme.imageFixed);
        QCOMPARE(page.theme.textSize, Profile::TextSize::NormalText);
        QVERIFY(Profile::defaultTheme() == Theme{});
        QVERIFY(Profile::presetTheme(Preset::AeroSkyPreset) == Theme{});

        const QVector<ModulePlacement> expected{
            {Module::HandleModule, Column::NarrowColumn, true},
            {Module::SongModule, Column::NarrowColumn, true},
            {Module::InterestsModule, Column::NarrowColumn, true},
            {Module::DetailsModule, Column::NarrowColumn, true},
            {Module::BlurbsModule, Column::WideColumn, true},
            {Module::TopFriendsModule, Column::WideColumn, true},
        };
        QVERIFY(page.modules == expected);
        QVERIFY(Profile::defaultModules() == expected);
        QVERIFY(page.content == Profile::Content{});
        QVERIFY(page.topFriends.isEmpty());
        QVERIFY(!page.background.isSet());
        QVERIFY(!page.song.isSet());

        const auto decoded = decodePageCore(encodePageCore(Page{}));
        QVERIFY(decoded);
        QVERIFY(*decoded == page);
    }

    void presetsMatchTheDesignTable_data()
    {
        QTest::addColumn<int>("preset");
        QTest::addColumn<int>("row");
        const QList<std::pair<Preset, const char *>> presets{
            {Preset::AeroSkyPreset, "Aero Sky"},         {Preset::Classic06Preset, "Classic '06"},
            {Preset::SceneQueenPreset, "Scene Queen"},   {Preset::NeonZebraPreset, "Neon Zebra"},
            {Preset::MidnightEmoPreset, "Midnight Emo"}, {Preset::GlitterGirlPreset, "Glitter Girl"},
            {Preset::SafetyPinPreset, "Safety Pin"},     {Preset::HeadlinerPreset, "Headliner"},
            {Preset::LinenPreset, "Linen"},              {Preset::ChromeY2KPreset, "Chrome Y2K"}};
        for (int i = 0; i < presets.size(); ++i)
            QTest::newRow(presets.at(i).second) << int(presets.at(i).first) << i;
    }

    void presetsMatchTheDesignTable()
    {
        QFETCH(int, preset);
        QFETCH(int, row);
        // SPEC §10, one line per preset, in the table's own order.
        static const DesignRow rows[] = {
            aeroSkyLightRow,
            {"solid #E5E5E5", "none", "#FFFFFF 100 #6699CC 1 solid 0", "flat #4574A8 #FFFFFF",
             "#FFCC99 #9A3B00 #F2B27A", "Standard Standard Standard", "#000000 #1F4E7A #003399",
             "M #000000 - none", "Cells"},
            {"#0A0A0C", "skulls #FF4FA8 80 M", "#000000 94 #FF3EA5 1 solid 3", "gloss #5E1139 #FFC2E2", "",
             "Rounded Standard Rounded", "#F4F4F4 #FF6FB8 #FF6FB8", "L #FF5CB3 #FFFFFF glitter xXx", "Lines"},
            {"#000000", "zebra #FFFFFF 24 L", "#07070A 92 #00E5FF 2 solid 3", "gloss #2A0058 #B6FF00", "",
             "Marker Standard Marker", "#F2F2F2 #00E5FF #FF5FC9", "L #B6FF00 #FF4FC3 shadow ★", "Lines"},
            {"#0E0E10", "broken-hearts #B3122E 62 M", "#141416 94 #B3122E 1 dashed 0", "gradient #3A0E16 #FF7A8A",
             "", "Typewriter Typewriter Gothic", "#D9D9DC #FF5F72 #FF8796", "L #ECECEF #C0142F glow", "Lines"},
            {"pattern #FFC3E1 #FFE6F3", "sparkles #FF7DBE 70 M", "#FFFFFF 94 #FF69B4 2 dotted 10",
             "gloss #B5165F #FFFFFF", "", "Rounded Rounded Script", "#4A1033 #B5165F #B5165F",
             "L #FF2E97 #FFE3F1 glitter ♥", "Cells falling-hearts"},
            {"#161616", "checkerboard #F2F2F2 14 M", "#0F0F0F 94 #E0212E 3 double 0", "flat #B3121E #FFFFFF", "",
             "Marker Typewriter Marker", "#EDEDED #FFD400 #FFD400", "XL #FFFFFF #E0212E shadow", "Lines"},
            {"pattern #0B0E12 #151B22", "halftone #3A4856 55 M", "#0E1116 94 #394654 1 solid 3",
             "gradient #232D38 #F5C518", "", "Serif Standard Serif", "#E4E7EB #F5C518 #5CC8FF",
             "XL #F5C518 #000000 shadow", "Lines"},
            {"pattern #F3EFE7 #ECE5D8", "linen-weave #D6CAB4 55 M", "#FFFFFF 97 #D8CFBF 1 solid 3",
             "none - #3A342B", "", "Serif Standard Serif", "#2E2A24 #85552A #85552A", "L #2E2A24 - none",
             "Lines"},
            {"pattern #050716 #1A1F5C", "cyber-grid #00E5FF 40 M", "#0A0F2E 88 #00D8F0 1 solid 6 neon",
             "gloss #C3CFDF #0A0F2E", "", "Future Standard Future", "#E3ECFF #7FE9FF #B8FF3C",
             "XL #39D5FF #6F7F99 chrome", "Lines falling-stars"},
        };
        const auto chosen = static_cast<Preset>(preset);
        const bool adaptive = chosen == Preset::AeroSkyPreset; // the only preset that follows the viewer
        const Theme theme = Profile::presetTheme(chosen);
        compareThemes(theme, themeFromDesign(rows[row], adaptive));
        if (QTest::currentTestFailed())
            return;

        // Presets are valid as they stand: normalising changes nothing.
        Page page;
        page.theme = theme;
        QVERIFY(Profile::normalized(page).theme == theme);
        QCOMPARE(Profile::presetName(chosen), QString::fromLatin1(QTest::currentDataTag()));
    }

    void aeroSkyDarkMatchesTheDesignTable()
    {
        const Theme dark = Profile::aeroSkyTheme(true);
        compareThemes(dark, themeFromDesign(aeroSkyDarkRow, false));
        if (QTest::currentTestFailed())
            return;
        QCOMPARE(dark.boxOpacity, 90);
        QVERIFY(!dark.adaptive);

        // The light palette as concrete values: Aero Sky light, not adaptive.
        compareThemes(Profile::aeroSkyTheme(false), themeFromDesign(aeroSkyLightRow, false));
    }

    void applyPresetKeepsContentAndSetsLayoutEffects_data()
    {
        QTest::addColumn<int>("preset");
        QTest::addColumn<int>("layout");
        QTest::addColumn<bool>("songFirst");
        const auto classic = int(Profile::Layout::ClassicLayout);
        QTest::newRow("aero sky") << int(Preset::AeroSkyPreset) << classic << false;
        QTest::newRow("classic 06") << int(Preset::Classic06Preset) << classic << false;
        QTest::newRow("scene queen") << int(Preset::SceneQueenPreset) << classic << false;
        QTest::newRow("neon zebra") << int(Preset::NeonZebraPreset) << classic << false;
        QTest::newRow("midnight emo") << int(Preset::MidnightEmoPreset) << classic << false;
        QTest::newRow("glitter girl") << int(Preset::GlitterGirlPreset) << classic << false;
        QTest::newRow("safety pin") << int(Preset::SafetyPinPreset) << int(Profile::Layout::FlippedLayout) << false;
        QTest::newRow("headliner") << int(Preset::HeadlinerPreset) << classic << true;
        QTest::newRow("linen") << int(Preset::LinenPreset) << classic << false;
        QTest::newRow("chrome y2k") << int(Preset::ChromeY2KPreset) << classic << false;
    }

    void applyPresetKeepsContentAndSetsLayoutEffects()
    {
        QFETCH(int, preset);
        QFETCH(int, layout);
        QFETCH(bool, songFirst);
        // One column, with the song hidden in the narrow column: the preset
        // must reset the layout without touching the owner's visibility.
        Page page = richPage();
        page.layout = Profile::Layout::SingleLayout;
        const Page before = page;

        const Page after = Profile::applyPreset(page, static_cast<Preset>(preset));
        QVERIFY(after.theme == Profile::presetTheme(static_cast<Preset>(preset)));
        QCOMPARE(after.preset, static_cast<Preset>(preset));
        QCOMPARE(after.layout, static_cast<Profile::Layout>(layout));
        QVERIFY(after.content == before.content);
        QVERIFY(after.topFriends == before.topFriends);
        QVERIFY(after.background == before.background);
        QVERIFY(after.song == before.song);
        QCOMPARE(after.revision, before.revision);
        QCOMPARE(hiddenModules(after.modules), hiddenModules(before.modules));
        QCOMPARE(after.modules.size(), before.modules.size());

        if (songFirst) {
            const QVector<Module> wide = columnOf(after.modules, Column::WideColumn);
            QCOMPARE(wide.value(0), Module::SongModule);
            QCOMPARE(wide.mid(1), columnOf(before.modules, Column::WideColumn));
            QVector<Module> narrowBefore = columnOf(before.modules, Column::NarrowColumn);
            narrowBefore.removeAll(Module::SongModule);
            QCOMPARE(columnOf(after.modules, Column::NarrowColumn), narrowBefore);
        } else {
            QVERIFY(after.modules == before.modules);
        }
    }

    void applyingTheCustomPresetChangesNothing()
    {
        const Page page = richPage();
        QVERIFY(Profile::applyPreset(page, Preset::CustomPreset) == page);
        QVERIFY(Profile::applyPreset(page, static_cast<Preset>(42)) == page);
    }

    void styleEditedSincePresetDetectsKnobChanges()
    {
        const QList<std::pair<const char *, std::function<void(Theme &)>>> knobs{
            {"background kind", [](Theme &t) { t.backgroundKind = Profile::BackgroundKind::GradientBackground; }},
            {"colour 1", [](Theme &t) { t.backgroundColor1 ^= 0x000001; }},
            {"motif", [](Theme &t) { t.motif = Profile::Motif::Plaid; }},
            {"motif opacity", [](Theme &t) { t.motifOpacity = 11; }},
            {"box opacity", [](Theme &t) { t.boxOpacity = 61; }},
            {"border width", [](Theme &t) { t.borderWidth = 0; }},
            {"radius", [](Theme &t) { t.boxRadius = Profile::BoxRadius::RoundCorners; }},
            {"neon edge", [](Theme &t) { t.boxGlow = !t.boxGlow; }},
            {"table style", [](Theme &t) {
                 t.tableStyle = t.tableStyle == Profile::TableStyle::CellTable ? Profile::TableStyle::LineTable
                                                                                 : Profile::TableStyle::CellTable;
             }},
            {"strip", [](Theme &t) {
                 t.headerStyle = t.headerStyle == Profile::HeaderStyle::FlatHeader ? Profile::HeaderStyle::GlossHeader
                                                                                   : Profile::HeaderStyle::FlatHeader;
             }},
            {"alt strip", [](Theme &t) { t.altHeader = !t.altHeader; }},
            {"alt border", [](Theme &t) { t.altBorderColor ^= 0x010000; }},
            {"body font", [](Theme &t) { t.bodyFont = Font::SerifFont; }},
            {"text size", [](Theme &t) { t.textSize = Profile::TextSize::SmallText; }},
            {"link colour", [](Theme &t) { t.linkColor ^= 0x000100; }},
            {"name size", [](Theme &t) { t.nameSize = Profile::NameSize::LargeName; }},
            {"flourish", [](Theme &t) { t.nameFlourish = Profile::Flourish::NoteFlourish; }},
            {"name colour 2", [](Theme &t) { t.nameColor2 ^= 0x000001; }},
            {"ambient", [](Theme &t) { t.ambient = Profile::Ambient::FallingSnow; }},
            {"adaptive", [](Theme &t) { t.adaptive = !t.adaptive; }},
        };
        for (const Preset preset : {Preset::AeroSkyPreset, Preset::Classic06Preset, Preset::ChromeY2KPreset}) {
            const Page picked = Profile::applyPreset(richPage(), preset);
            QVERIFY(!Profile::styleDiffersFromPreset(picked));
            for (const auto &[knob, change] : knobs) {
                Page edited = picked;
                change(edited.theme);
                QVERIFY2(Profile::styleDiffersFromPreset(edited), knob);
            }

            // Words, layout, arrangement and media are not style.
            Page other = picked;
            other.content.headline = QStringLiteral("Something new");
            other.layout = Profile::Layout::SingleLayout;
            other.modules.swapItemsAt(0, 1);
            other.song = {};
            QVERIFY(!Profile::styleDiffersFromPreset(other));
        }

        // A picture background differs from every preset once a picture is set.
        Page withPicture = Profile::applyPreset(richPage(), Preset::LinenPreset);
        withPicture.theme.backgroundKind = Profile::BackgroundKind::ImageBackground;
        QVERIFY(Profile::styleDiffersFromPreset(withPicture));

        // Aero Sky edited into concrete colours is no longer the adaptive preset.
        Page concrete = Profile::applyPreset(richPage(), Preset::AeroSkyPreset);
        concrete.theme = Profile::aeroSkyTheme(false);
        QVERIFY(Profile::styleDiffersFromPreset(concrete));

        Page custom = richPage();
        custom.preset = Preset::CustomPreset;
        QVERIFY(Profile::styleDiffersFromPreset(custom));
    }

    void normalizeClampsNumericKnobs_data()
    {
        QTest::addColumn<QString>("field");
        QTest::addColumn<qint64>("input");
        QTest::addColumn<qint64>("expected");
        QTest::newRow("motif opacity 0") << "motifOpacity" << qint64(0) << qint64(10);
        QTest::newRow("motif opacity 9") << "motifOpacity" << qint64(9) << qint64(10);
        QTest::newRow("motif opacity 10") << "motifOpacity" << qint64(10) << qint64(10);
        QTest::newRow("motif opacity 80") << "motifOpacity" << qint64(80) << qint64(80);
        QTest::newRow("motif opacity 81") << "motifOpacity" << qint64(81) << qint64(80);
        QTest::newRow("motif opacity 255") << "motifOpacity" << qint64(255) << qint64(80);
        QTest::newRow("box opacity 0") << "boxOpacity" << qint64(0) << qint64(60);
        QTest::newRow("box opacity 59") << "boxOpacity" << qint64(59) << qint64(60);
        QTest::newRow("box opacity 60") << "boxOpacity" << qint64(60) << qint64(60);
        QTest::newRow("box opacity 100") << "boxOpacity" << qint64(100) << qint64(100);
        QTest::newRow("box opacity 101") << "boxOpacity" << qint64(101) << qint64(100);
        QTest::newRow("border width 0") << "borderWidth" << qint64(0) << qint64(0);
        QTest::newRow("border width 4") << "borderWidth" << qint64(4) << qint64(4);
        QTest::newRow("border width 5") << "borderWidth" << qint64(5) << qint64(4);
        QTest::newRow("colour 1 high bits") << "backgroundColor1" << qint64(0xFF123456) << qint64(0x123456);
        QTest::newRow("name colour 2 high bits") << "nameColor2" << qint64(0x01000000) << qint64(0);
        QTest::newRow("alt border high bits") << "altBorderColor" << qint64(0x80ABCDEF) << qint64(0xABCDEF);
        QTest::newRow("revision negative") << "revision" << qint64(-5) << qint64(0);
        QTest::newRow("revision too large") << "revision" << Profile::maxRevision + 1 << Profile::maxRevision;
        QTest::newRow("published negative") << "publishedAtMs" << qint64(-1) << qint64(0);
    }

    void normalizeClampsNumericKnobs()
    {
        QFETCH(QString, field);
        QFETCH(qint64, input);
        QFETCH(qint64, expected);
        // Each knob as a setter and a getter, so one row writes and reads the same field.
        struct Knob {
            std::function<void(Page &, qint64)> set;
            std::function<qint64(const Page &)> get;
        };
        const auto small = [](quint8 Theme::*member) {
            return Knob{[member](Page &p, qint64 v) { p.theme.*member = quint8(v); },
                        [member](const Page &p) { return qint64(p.theme.*member); }};
        };
        const auto colour = [](quint32 Theme::*member) {
            return Knob{[member](Page &p, qint64 v) { p.theme.*member = quint32(v); },
                        [member](const Page &p) { return qint64(p.theme.*member); }};
        };
        const auto wide = [](qint64 Page::*member) {
            return Knob{[member](Page &p, qint64 v) { p.*member = v; },
                        [member](const Page &p) { return p.*member; }};
        };
        const QHash<QString, Knob> knobs{
            {u"motifOpacity"_s, small(&Theme::motifOpacity)},
            {u"boxOpacity"_s, small(&Theme::boxOpacity)},
            {u"borderWidth"_s, small(&Theme::borderWidth)},
            {u"backgroundColor1"_s, colour(&Theme::backgroundColor1)},
            {u"nameColor2"_s, colour(&Theme::nameColor2)},
            {u"altBorderColor"_s, colour(&Theme::altBorderColor)},
            {u"revision"_s, wide(&Page::revision)},
            {u"publishedAtMs"_s, wide(&Page::publishedAtMs)},
        };
        QVERIFY(knobs.contains(field));
        const Knob knob = knobs.value(field);

        Page page = richPage();
        knob.set(page, input);
        QCOMPARE(knob.get(page), input); // the struct really holds the raw value
        const Page normalized = Profile::normalized(page);
        QCOMPARE(knob.get(normalized), expected);

        // A normalised page survives the wire unchanged.
        const auto decoded = decodePageCore(encodePageCore(normalized));
        QVERIFY(decoded);
        QVERIFY(*decoded == normalized);
    }

    void normalizeResetsUnknownEnumsToDefaults()
    {
        const Theme defaults;
        Page page = richPage();
        Theme &theme = page.theme;
        theme.backgroundKind = static_cast<Profile::BackgroundKind>(4);
        theme.motif = static_cast<Profile::Motif>(17);
        theme.motifScale = static_cast<Profile::MotifScale>(3);
        theme.imageMode = static_cast<Profile::ImageMode>(4);
        theme.borderStyle = static_cast<Profile::BorderStyle>(4);
        theme.boxRadius = static_cast<Profile::BoxRadius>(4);
        theme.tableStyle = static_cast<Profile::TableStyle>(2);
        theme.headerStyle = static_cast<Profile::HeaderStyle>(4);
        theme.headingFont = static_cast<Font>(9);
        theme.bodyFont = static_cast<Font>(200);
        theme.textSize = static_cast<Profile::TextSize>(3);
        theme.nameFont = static_cast<Font>(9);
        theme.nameEffect = static_cast<Profile::NameEffect>(7);
        theme.nameSize = static_cast<Profile::NameSize>(3);
        theme.nameFlourish = static_cast<Profile::Flourish>(7);
        theme.ambient = static_cast<Profile::Ambient>(5);
        page.layout = static_cast<Profile::Layout>(3);
        page.preset = static_cast<Preset>(10);
        page.content.mood = static_cast<Profile::Mood>(49);
        page.content.details.zodiac = static_cast<Profile::Zodiac>(13);
        page.content.details.hereFor = 0xC5;

        const Page result = Profile::normalized(page);
        const Theme &t = result.theme;
        QCOMPARE(t.backgroundKind, defaults.backgroundKind);
        QCOMPARE(t.motif, defaults.motif);
        QCOMPARE(t.motifScale, defaults.motifScale);
        QCOMPARE(t.imageMode, defaults.imageMode);
        QCOMPARE(t.borderStyle, defaults.borderStyle);
        QCOMPARE(t.boxRadius, defaults.boxRadius);
        QCOMPARE(t.tableStyle, defaults.tableStyle);
        QCOMPARE(t.headerStyle, defaults.headerStyle);
        QCOMPARE(t.headingFont, defaults.headingFont);
        QCOMPARE(t.bodyFont, defaults.bodyFont);
        QCOMPARE(t.textSize, defaults.textSize);
        QCOMPARE(t.nameFont, defaults.nameFont);
        QCOMPARE(t.nameEffect, defaults.nameEffect);
        QCOMPARE(t.nameSize, defaults.nameSize);
        QCOMPARE(t.nameFlourish, defaults.nameFlourish);
        QCOMPARE(t.ambient, defaults.ambient);
        QCOMPARE(result.layout, Profile::Layout::ClassicLayout);
        QCOMPARE(result.preset, Preset::CustomPreset);
        QCOMPARE(result.content.mood, Profile::Mood::NoMood);
        QCOMPARE(result.content.details.zodiac, Profile::Zodiac::NoZodiac);
        QCOMPARE(result.content.details.hereFor, quint8(0x05));

        // The last value of each range is still valid.
        Page edges = richPage();
        edges.theme.motif = Profile::Motif::LinenWeave;
        edges.content.mood = Profile::Mood::MoodWorking;
        edges.content.details.zodiac = Profile::Zodiac::Pisces;
        edges.content.details.hereFor = 0x3F;
        const Page kept = Profile::normalized(edges);
        QCOMPARE(kept.theme.motif, Profile::Motif::LinenWeave);
        QCOMPARE(kept.content.mood, Profile::Mood::MoodWorking);
        QCOMPARE(kept.content.details.zodiac, Profile::Zodiac::Pisces);
        QCOMPARE(kept.content.details.hereFor, quint8(0x3F));

        // Faces: Pixel is name-only, and body text only takes the body-safe faces.
        const auto faces = [](Font heading, Font body, Font name) {
            Page p;
            p.theme.headingFont = heading;
            p.theme.bodyFont = body;
            p.theme.nameFont = name;
            return Profile::normalized(p).theme;
        };
        QCOMPARE(faces(Font::PixelFont, Font::InterfaceFont, Font::PixelFont).headingFont, Font::InterfaceFont);
        QCOMPARE(faces(Font::PixelFont, Font::InterfaceFont, Font::PixelFont).nameFont, Font::PixelFont);
        QCOMPARE(faces(Font::GothicFont, Font::InterfaceFont, Font::InterfaceFont).headingFont, Font::GothicFont);
        for (const Font body :
             {Font::ScriptFont, Font::PixelFont, Font::GothicFont, Font::FutureFont, Font::MarkerFont})
            QCOMPARE(faces(Font::InterfaceFont, body, Font::InterfaceFont).bodyFont, Font::InterfaceFont);
        for (const Font body : {Font::InterfaceFont, Font::RoundedFont, Font::TypewriterFont, Font::SerifFont})
            QCOMPARE(faces(Font::InterfaceFont, body, Font::InterfaceFont).bodyFont, body);

        // A picture background needs a picture; without one it becomes the solid base colour.
        Page noPicture = richPage();
        noPicture.background = {};
        QCOMPARE(Profile::normalized(noPicture).theme.backgroundKind, Profile::BackgroundKind::SolidBackground);
        QCOMPARE(Profile::normalized(richPage()).theme.backgroundKind, Profile::BackgroundKind::ImageBackground);

        // A narrow double border is kept as chosen (the renderer draws it solid).
        Page narrowDouble = richPage();
        narrowDouble.theme.borderStyle = Profile::BorderStyle::DoubleBorder;
        narrowDouble.theme.borderWidth = 1;
        QCOMPARE(Profile::normalized(narrowDouble).theme.borderStyle, Profile::BorderStyle::DoubleBorder);
    }

    void normalizeDropsMalformedMediaRefs()
    {
        const auto normalizedRefs = [](Profile::MediaRef background, Profile::MediaRef song) {
            Page page = richPage();
            page.background = background;
            page.song = song;
            return Profile::normalized(page);
        };
        const Profile::MediaRef goodBackground{hashOf('\x01'), 1000, 1920, 1920, 0};
        const Profile::MediaRef goodSong{hashOf('\x02'), quint32(maxSongBytes), 0, 0, 45'500};
        Page kept = normalizedRefs(goodBackground, goodSong);
        QVERIFY(kept.background == goodBackground);
        QVERIFY(kept.song == goodSong);

        QVERIFY(!normalizedRefs({hashOf('\x01').left(31), 1000, 10, 10, 0}, goodSong).background.isSet());
        QVERIFY(!normalizedRefs({hashOf('\x01'), 0, 10, 10, 0}, goodSong).background.isSet());
        QVERIFY(!normalizedRefs({hashOf('\x01'), quint32(maxBackgroundImageBytes) + 1, 10, 10, 0}, goodSong)
                     .background.isSet());
        QVERIFY(!normalizedRefs({hashOf('\x01'), 1000, 1921, 10, 0}, goodSong).background.isSet());
        QVERIFY(!normalizedRefs({hashOf('\x01'), 1000, 10, 1921, 0}, goodSong).background.isSet());
        QVERIFY(!normalizedRefs(goodBackground, {hashOf('\x02'), 1000, 0, 0, 45'501}).song.isSet());
        QVERIFY(!normalizedRefs(goodBackground, {hashOf('\x02'), quint32(maxSongBytes) + 1, 0, 0, 1000})
                     .song.isSet());
        // A dropped ref is fully cleared, and fields a slot does not use are zeroed.
        QVERIFY(normalizedRefs(goodBackground, {hashOf('\x02'), 0, 0, 0, 1000}).song == Profile::MediaRef{});
        const Page zeroed = normalizedRefs({hashOf('\x01'), 1000, 10, 10, 777}, {hashOf('\x02'), 1000, 5, 6, 900});
        QCOMPARE(zeroed.background.durationMs, 0u);
        QCOMPARE(zeroed.song.width, 0);
        QCOMPARE(zeroed.song.height, 0);
    }

    void normalizeSanitizesText_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<bool>("paragraphs");
        QTest::addColumn<int>("maxLength");
        QTest::addColumn<QString>("expected");
        const auto line = [](const char *tag, const QString &in, const QString &out, int max = 100) {
            QTest::newRow(tag) << in << false << max << out;
        };
        const auto para = [](const char *tag, const QString &in, const QString &out, int max = 100) {
            QTest::newRow(tag) << in << true << max << out;
        };
        line("C0 controls", u"a\u0001b\u0007c\u001Bd\u0000e"_s, u"abcde"_s);
        line("DEL", u"a\u007Fb"_s, u"ab"_s);
        line("C1 controls", u"a\u0085b\u009Bc\u0080d"_s, u"abcd"_s);
        line("tab is whitespace", u"a\tb"_s, u"a b"_s);
        line("bidi override", u"\u202Egnp.exe"_s, u"gnp.exe"_s);
        line("bidi marks and isolates", u"a\u200Eb\u200Fc\u061Cd\u2066e\u2067f\u2068g\u2069h\u202Ai\u202Dj"_s,
             u"abcdefghij"_s);
        line("line and paragraph separators", u"a\u2028b\u2029c"_s, u"abc"_s);
        line("default ignorables", u"a\u00ADb\u200Bc\uFEFFd\u2060e\u034Ff\u3164g\uFFA0h\u115Fi\u180Ej\u17B4k"_s,
             u"abcdefghijk"_s);
        line("tag characters", u"flag\U000E0067\U000E0062\U000E007F"_s, u"flag"_s);
        line("musical formatting", u"a\U0001D173b\U0001BCA0c"_s, u"abc"_s);
        line("lone surrogates", QStringLiteral("a") + QChar(0xD800) + u"b"_s + QChar(0xDC00), u"ab"_s);
        // The sequences below, spelled out. A joiner inside them is meaningful and kept.
        const QString woman = u"\U0001F469"_s, laptop = u"\U0001F4BB"_s;
        const QString zwj = u"\u200D"_s, zwnj = u"\u200C"_s, emojiStyle = u"\uFE0F"_s;
        const QString coder = woman + zwj + laptop;
        const QString rainbowFlag = u"\U0001F3F3"_s + emojiStyle + zwj + u"\U0001F308"_s;
        const QString heartOnFire = u"\u2764"_s + emojiStyle + zwj + u"\U0001F525"_s;
        const QString persianMi = u"\u0645\u06CC"_s;
        const QString persian = persianMi + zwnj + u"\u062E\u0648\u0627\u0647\u0645"_s; // "mi-khaham"
        const QString devanagari = u"\u0915\u094D"_s + zwnj + u"\u0937"_s;           // ka, virama, ZWNJ, ssa
        line("ZWJ inside an emoji sequence kept", coder + u" ok"_s, coder + u" ok"_s);
        line("ZWJ after a variation selector kept", rainbowFlag, rainbowFlag);
        line("ZWJ in a heart on fire kept", heartOnFire, heartOnFire);
        line("stray ZWJ between letters dropped", u"a"_s + zwj + u"b"_s, u"ab"_s);
        line("ZWJ at the end dropped", woman + zwj, woman);
        line("ZWJ at the start dropped", zwj + woman, woman);
        line("ZWJ after a space dropped", woman + u" "_s + zwj + laptop, woman + u" "_s + laptop);
        line("ZWJ before an ignorable dropped", woman + zwj + u"\u200B"_s, woman);
        line("doubled ZWJ keeps one", woman + zwj + zwj + laptop, coder);
        line("ZWNJ between letters kept", persian, persian);
        line("ZWNJ after a virama kept", devanagari, devanagari);
        line("ZWNJ between ASCII dropped", u"a"_s + zwnj + u"b"_s, u"ab"_s);
        line("ZWNJ at the end dropped", persianMi + zwnj, persianMi);
        line("variation selector after a base kept", u"\u263A\uFE0F!"_s, u"\u263A\uFE0F!"_s);
        line("variation selector at the start dropped", u"\uFE0Fa"_s, u"a"_s);
        line("variation selector after a space dropped", u"a \uFE0Fb"_s, u"a b"_s);
        line("doubled variation selector keeps one", u"\u263A\uFE0F\uFE0E"_s, u"\u263A\uFE0F"_s);
        line("five combining marks keep two", u"Ze\u0301\u0302\u0303\u0304\u0305d"_s, u"Ze\u0301\u0302d"_s);
        line("enclosing marks count too", u"a\u20DD\u20DE\u20DF"_s, u"a\u20DD\u20DE"_s);
        line("marks on each base counted apart", u"e\u0301\u0302\u0303o\u0301\u0302\u0303"_s,
             u"e\u0301\u0302o\u0301\u0302"_s);
        line("newline in a single-line field", u"a\nb"_s, u"a b"_s);
        line("CRLF in a single-line field", u"a\r\nb\rc"_s, u"a b c"_s);
        para("CRLF and CR in paragraphs", u"a\r\nb\rc"_s, u"a\nb\nc"_s);
        para("triple newline", u"a\n\n\nb"_s, u"a\n\nb"_s);
        para("blank lines of spaces", u"a\n \n\t\n  b"_s, u"a\n\n b"_s);
        para("trailing whitespace per line", u"a  \nb\t\nc"_s, u"a\nb\nc"_s);
        para("leading and trailing newlines", u"\n\n  a\n\n"_s, u"a"_s);
        para("paragraph spaces collapse", u"a   b\n c  d"_s, u"a b\n c d"_s);
        line("whitespace collapse", u"  a \t\u00A0 b  "_s, u"a b"_s);
        line("wide spaces collapse", u"a\u3000\u2003\u2002b"_s, u"a b"_s);
        line("surrogate at the bound", u"abc\U0001F600"_s, u"abc"_s, 4);
        line("cluster at the bound", u"abe\u0301"_s, u"ab"_s, 3);
        line("emoji sequence at the bound", u"a\U0001F469\u200D\U0001F4BB"_s, u"a"_s, 4);
        line("flag at the bound", u"ab\U0001F1FA\U0001F1F8"_s, u"ab"_s, 5);
        // After an Indic or Thai run Qt's boundary finder reports a boundary
        // inside the emoji's surrogate pair; the cut must not trust it.
        line("Hindi then emoji at the bound", u"नमस्ते \U0001F600"_s,
             u"नमस्ते"_s, 8);
        line("Thai then emoji at the bound", u"สวัสดี\U0001F600"_s,
             u"สวัสดี"_s, 7);
        para("Tamil then emoji at the bound", u"வணக்கம்\U0001F600"_s,
             u"வணக்கம்"_s, 8);
        line("cut then trim", u"ab cd"_s, u"ab"_s, 3);
        para("cut then trim newlines", u"ab\n\ncd"_s, u"ab"_s, 4);
        line("exactly at the bound", u"abcd"_s, u"abcd"_s, 4);
        line("zero bound", u"abcd"_s, QString(), 0);
    }

    void normalizeSanitizesText()
    {
        QFETCH(QString, input);
        QFETCH(bool, paragraphs);
        QFETCH(int, maxLength);
        QFETCH(QString, expected);
        const QString result =
            paragraphs ? Profile::sanitizeParagraphs(input, maxLength) : Profile::sanitizeLine(input, maxLength);
        QCOMPARE(result, expected);
        QVERIFY(result.size() <= maxLength);
        // Full sanitising is idempotent, so a decoded page re-encodes to the same bytes.
        QCOMPARE(paragraphs ? Profile::sanitizeParagraphs(result, maxLength) : Profile::sanitizeLine(result, maxLength),
                 result);

        // normalized() applies the same rules to the page's fields.
        Page page;
        if (paragraphs)
            page.content.aboutMe = input;
        else
            page.content.headline = input;
        const Page normalized = Profile::normalized(page);
        if (paragraphs)
            QCOMPARE(normalized.content.aboutMe, Profile::sanitizeParagraphs(input, Profile::TextBounds::aboutMe));
        else
            QCOMPARE(normalized.content.headline, Profile::sanitizeLine(input, Profile::TextBounds::headline));
    }

    void normalizeAppliesEveryFieldBound()
    {
        Page page;
        const QString longText(3000, u'x');
        Profile::Content &c = page.content;
        c.displayName = c.headline = c.aboutMe = c.meet = c.songTitle = c.songArtist = longText;
        c.interests = {longText, longText, longText, longText, longText, longText};
        c.details.hometown = c.details.occupation = c.details.education = c.details.languages = longText;
        c.infoLines = {longText, u"  "_s, longText, u"\u200B"_s, longText, longText};
        page.topFriends = {{accountOf(1), longText}};

        const Profile::Content n = Profile::normalized(page).content;
        using B = Profile::TextBounds;
        QCOMPARE(n.displayName.size(), B::displayName);
        QCOMPARE(n.headline.size(), B::headline);
        QCOMPARE(n.aboutMe.size(), B::aboutMe);
        QCOMPARE(n.meet.size(), B::meet);
        QCOMPARE(n.songTitle.size(), B::songTitle);
        QCOMPARE(n.songArtist.size(), B::songArtist);
        for (const QString *interest : {&n.interests.general, &n.interests.music, &n.interests.movies,
                                        &n.interests.television, &n.interests.books, &n.interests.heroes})
            QCOMPARE(interest->size(), B::interest);
        for (const QString *detail :
             {&n.details.hometown, &n.details.occupation, &n.details.education, &n.details.languages})
            QCOMPARE(detail->size(), B::detail);
        // Empty lines are dropped before the three are counted.
        QCOMPARE(n.infoLines.size(), B::infoLines);
        for (const QString &line : n.infoLines)
            QCOMPARE(line.size(), B::infoLine);
        QCOMPARE(Profile::normalized(page).topFriends.value(0).name.size(), B::friendName);
    }

    void liveSanitizingKeepsTypedSpacesAndBlankLines()
    {
        // Whitespace is never trimmed or collapsed while typing.
        QCOMPARE(Profile::sanitizeLive(u"a "_s, 48, false), u"a "_s);
        QCOMPARE(Profile::sanitizeLive(u"  lead  in"_s, 48, false), u"  lead  in"_s);
        QCOMPARE(Profile::sanitizeLive(u"x\n\n"_s, 1000, true), u"x\n\n"_s);
        QCOMPARE(Profile::sanitizeLive(u"x\n\n\n\n"_s, 1000, true), u"x\n\n\n\n"_s);
        QCOMPARE(Profile::sanitizeLive(u"a\u00A0b"_s, 48, false), u"a\u00A0b"_s);
        // Line breaks are still normalised; single-line fields turn them into spaces.
        QCOMPARE(Profile::sanitizeLive(u"a\r\nb\rc"_s, 1000, true), u"a\nb\nc"_s);
        QCOMPARE(Profile::sanitizeLive(u"a\nb"_s, 48, false), u"a b"_s);
        QCOMPARE(Profile::sanitizeLive(u"a\tb"_s, 48, false), u"a b"_s);
        // Invisible and direction-changing characters never get in.
        QCOMPARE(Profile::sanitizeLive(u"a\u202Eb\u200Bc\u0007d\U000E0041e"_s, 48, false), u"abcde"_s);
        QCOMPARE(Profile::sanitizeLive(u"a\u200Db"_s, 48, false), u"ab"_s);
        // A joiner being typed at the end waits for its right-hand neighbour.
        QCOMPARE(Profile::sanitizeLive(u"\u0645\u06CC\u200C"_s, 48, false), u"\u0645\u06CC\u200C"_s);
        QCOMPARE(Profile::sanitizeLive(u"\U0001F469\u200D"_s, 48, false), u"\U0001F469\u200D"_s);
        // The bound holds, without splitting a character.
        QCOMPARE(Profile::sanitizeLive(QString(50, u'a'), 48, false).size(), 48);
        QCOMPARE(Profile::sanitizeLive(QString(47, u'a') + u"\U0001F600"_s, 48, false), QString(47, u'a'));
        // Typing gets no second filtering pass, so the cut alone must keep
        // the emoji whole after a script Qt's boundary finder misjudges.
        const QString namaste = u"नमस्ते"_s;
        QCOMPARE(Profile::sanitizeLive(namaste + u" \U0001F600"_s, 8, false), namaste + u" "_s);
        QCOMPARE(Profile::sanitizeLive(namaste + u" \U0001F600"_s, 8, true), namaste + u" "_s);
        QCOMPARE(Profile::sanitizeLive(namaste + u"\U0001F600"_s, 7, false), namaste);

        // Saving what was typed gives what pasting it would have given.
        for (const QString &typed : {u"  Dana  W. "_s, u"a\n\n\n\nb  "_s, u"x\u202E y\u200B"_s}) {
            QCOMPARE(Profile::sanitizeParagraphs(Profile::sanitizeLive(typed, 1000, true), 1000),
                     Profile::sanitizeParagraphs(typed, 1000));
            QCOMPARE(Profile::sanitizeLine(Profile::sanitizeLive(typed, 48, false), 48),
                     Profile::sanitizeLine(typed, 48));
        }
    }

    void truncationNeverSplitsACharacter_data()
    {
        QTest::addColumn<QString>("text");
        // Each script with marks, then an emoji (a surrogate pair) and a
        // flag, so every bound lands once inside a cluster or a pair.
        const QString emoji = u" \U0001F600\U0001F1FA\U0001F1F8 end"_s;
        QTest::newRow("Hindi") << u"नमस्ते"_s + emoji;
        QTest::newRow("Hindi, no space") << u"नमस्ते\U0001F600\U0001F600"_s;
        QTest::newRow("Bengali") << u"নমস্কার"_s + emoji;
        QTest::newRow("Tamil") << u"வணக்கம்"_s + emoji;
        QTest::newRow("Thai") << u"สวัสดี"_s + emoji;
        QTest::newRow("Arabic") << u"مرحبا"_s + emoji;
        QTest::newRow("Korean") << u"안녕"_s + emoji;
        QTest::newRow("Latin with marks") << u"Zé̂ö"_s + emoji;
        QTest::newRow("supplementary mark") << u"a\U00011013\U00011038b\U0001F600"_s; // Brahmi ka + aa sign
    }

    void truncationNeverSplitsACharacter()
    {
        QFETCH(QString, text);
        for (const bool multiLine : {false, true}) {
            const QString whole = Profile::sanitizeLive(text, 1000, multiLine);
            for (int bound = 1; bound <= whole.size(); ++bound) {
                const QString live = Profile::sanitizeLive(text, bound, multiLine);
                const QString context = u"bound %1, multiLine %2"_s.arg(bound).arg(multiLine);
                QVERIFY2(live.size() <= bound, qPrintable(context));
                QVERIFY2(isWellFormed(live), qPrintable(context));
                // A prefix of the text, cut between two characters.
                QVERIFY2(whole.startsWith(live), qPrintable(context));
                QVERIFY2(live.size() == whole.size() || !continuesACharacter(whole, live.size()),
                         qPrintable(context));
                // Typing it again changes nothing.
                QCOMPARE(Profile::sanitizeLive(live, bound, multiLine), live);

                const QString full =
                    multiLine ? Profile::sanitizeParagraphs(text, bound) : Profile::sanitizeLine(text, bound);
                QVERIFY2(full.size() <= bound && isWellFormed(full), qPrintable(context));
                QCOMPARE(multiLine ? Profile::sanitizeParagraphs(full, bound) : Profile::sanitizeLine(full, bound),
                         full);
            }
        }
    }

    void everyTextFieldUsesItsSanitizer_data()
    {
        QTest::addColumn<int>("field");
        const QVector<TextField> fields = textFields();
        for (int i = 0; i < fields.size(); ++i)
            QTest::newRow(fields.at(i).name) << i;
    }

    void everyTextFieldUsesItsSanitizer()
    {
        QFETCH(int, field);
        const TextField text = textFields().at(field);
        // Paragraph fields keep line breaks (at most one blank line in a
        // row). Single-line fields turn them into spaces: a contact's name
        // or friend label must never draw a second line of its own.
        const QString input = u"a\nb\n\n\nc"_s;
        const QString expected = text.paragraphs ? u"a\nb\n\nc"_s : u"a b c"_s;
        Page page;
        text.text(page) = input;

        Page normalized = Profile::normalized(page);
        QCOMPARE(text.text(normalized), expected);
        // What a contact receives reads the same.
        const auto decoded = decodePageCore(encodePageCore(page));
        QVERIFY(decoded);
        Page received = *decoded;
        QCOMPARE(text.text(received), expected);
    }

    void samePublishedContentComparesNormalizedForms()
    {
        const Page published = richPage();
        Page draft = published;
        QVERIFY(Profile::samePublishedContent(draft, published));

        // Typing leftovers that full sanitising removes are not changes.
        draft.content.displayName += u"  "_s;
        draft.content.headline = u"  "_s + draft.content.headline;
        draft.content.aboutMe += u"\n\n\n"_s;
        draft.revision = 0;
        draft.publishedAtMs = 99;
        // The same arrangement written in another order.
        std::rotate(draft.modules.begin(), draft.modules.begin() + 3, draft.modules.end());
        QVERIFY(Profile::samePublishedContent(draft, published));
        QVERIFY(Profile::samePublishedContent(published, draft));

        Page changed = draft;
        changed.content.headline = u"Something else"_s;
        QVERIFY(!Profile::samePublishedContent(changed, published));
        changed = draft;
        changed.theme.linkColor ^= 1;
        QVERIFY(!Profile::samePublishedContent(changed, published));
        changed = draft;
        changed.modules[0].visible = !changed.modules[0].visible;
        QVERIFY(!Profile::samePublishedContent(changed, published));
        changed = draft;
        changed.song = {};
        QVERIFY(!Profile::samePublishedContent(changed, published));
        changed = draft;
        changed.topFriends.removeLast();
        QVERIFY(!Profile::samePublishedContent(changed, published));
        changed = draft;
        changed.preset = Preset::LinenPreset;
        QVERIFY(!Profile::samePublishedContent(changed, published));
    }

    void normalizeRepairsModuleArrangement()
    {
        Page page;
        page.modules = {
            {Module::TopFriendsModule, Column::WideColumn, false},
            {Module::HandleModule, Column::NarrowColumn, true},
            {Module::HandleModule, Column::WideColumn, false},           // repeated: the first wins
            {static_cast<Module>(9), Column::NarrowColumn, true},        // unknown
            {static_cast<Module>(0), Column::NarrowColumn, true},        // unknown
            {Module::SongModule, static_cast<Column>(7), false},         // bad column: its default
            {Module::BlurbsModule, Column::NarrowColumn, true},          // moved by the owner
        };
        const QVector<ModulePlacement> expected{
            {Module::HandleModule, Column::NarrowColumn, true},
            {Module::SongModule, Column::NarrowColumn, false},
            {Module::BlurbsModule, Column::NarrowColumn, true},
            {Module::InterestsModule, Column::NarrowColumn, true}, // missing: appended, visible
            {Module::DetailsModule, Column::NarrowColumn, true},
            {Module::TopFriendsModule, Column::WideColumn, false},
        };
        const Page result = Profile::normalized(page);
        QVERIFY(result.modules == expected);
        QVERIFY(Profile::normalized(result).modules == expected);

        Page wideDefault;
        wideDefault.modules = {{Module::TopFriendsModule, static_cast<Column>(2), true}};
        QCOMPARE(columnOf(Profile::normalized(wideDefault).modules, Column::WideColumn),
                 (QVector<Module>{Module::TopFriendsModule, Module::BlurbsModule}));

        QVERIFY(Profile::normalized(Page{}).modules == Profile::defaultModules());
    }

    void normalizeCapsAndDeduplicatesTopFriends()
    {
        Page page;
        page.topFriends = {
            {accountOf(1), u"One"_s},
            {accountOf(2), u"Two"_s},
            {accountOf(1), u"One again"_s},       // repeated: the first wins
            {accountOf(3).left(15), u"Short"_s}, // not an account id
            {filled(16, '\0'), u"Null"_s},        // not an account id
            {accountOf(4), u"  Four\u202E  "_s},
            {accountOf(5), u"Five"_s},
            {accountOf(6), u"Six"_s},
            {accountOf(7), u"Seven"_s},
            {accountOf(8), u"Eight"_s},
            {accountOf(9), u"Nine"_s},
        };
        const QVector<Profile::TopFriend> result = Profile::normalized(page).topFriends;
        QCOMPARE(result.size(), Profile::maxTopFriends);
        const QStringList names{u"One"_s, u"Two"_s, u"Four"_s, u"Five"_s, u"Six"_s, u"Seven"_s, u"Eight"_s, u"Nine"_s};
        for (int i = 0; i < result.size(); ++i)
            QCOMPARE(result.at(i).name, names.at(i));
        QCOMPARE(result.at(0).accountId, accountOf(1));
        QCOMPARE(result.at(2).accountId, accountOf(4));
    }

    void nextRevisionIsMonotonic()
    {
        QCOMPARE(Profile::nextRevision(0, 1'000), 1'000);
        QCOMPARE(Profile::nextRevision(1'000, 1'000), 1'001);
        QCOMPARE(Profile::nextRevision(5'000, 1'000), 5'001); // the clock went back
        QCOMPARE(Profile::nextRevision(-7, 0), 1);
        QCOMPARE(Profile::nextRevision(0, -50), 1);
        QCOMPARE(Profile::nextRevision(Profile::maxRevision, 5), Profile::maxRevision);
        QCOMPARE(Profile::nextRevision(3, Profile::maxRevision + 10), Profile::maxRevision);

        // A reinstall: nothing stored locally, yet the new revision beats what contacts hold.
        const qint64 now = 1'727'000'000'000;
        QVERIFY(Profile::nextRevision(0, now) > now - 1);

        qint64 revision = 0;
        const qint64 clocks[] = {100, 50, 50, 400, 10, 401, 401, 0, 1'000};
        for (const qint64 clock : clocks) {
            const qint64 next = Profile::nextRevision(revision, clock);
            QVERIFY(next > revision);
            QVERIFY(next >= clock);
            revision = next;
        }
    }

    // --- Codec ---------------------------------------------------------------

    void coreRoundTripsEveryField()
    {
        const Page page = richPage();
        QVERIFY(Profile::normalized(page) == page); // the fixture is already in normal form
        const QByteArray payload = encodePageCore(page);
        QCOMPARE(classifyProfilePayload(payload), ProfilePayloadKind::PageCore);
        const auto decoded = decodePageCore(payload);
        QVERIFY(decoded);
        compareThemes(decoded->theme, page.theme);
        if (QTest::currentTestFailed())
            return;
        QVERIFY(decoded->content == page.content);
        QVERIFY(decoded->modules == page.modules);
        QVERIFY(decoded->topFriends == page.topFriends);
        QVERIFY(decoded->background == page.background);
        QVERIFY(decoded->song == page.song);
        QVERIFY(*decoded == page);

        // The keys on the wire are ARCH §1.4–§1.9's, each holding its field.
        const QCborMap body = bodyOf(payload);
        QCOMPARE(body.value(0), QCborValue(1));
        QCOMPARE(body.value(1), QCborValue(1));
        QCOMPARE(body.value(2), QCborValue(page.revision));
        QCOMPARE(body.value(3), QCborValue(page.publishedAtMs));
        QCOMPARE(body.value(9), QCborValue(int(Preset::GlitterGirlPreset)));

        const QCborMap expectedTheme{
            {1, 3},         {2, 0x010203},  {3, 0x040506},  {4, 16},        {5, 0x070809},  {6, 80},
            {7, 2},         {8, 3},         {9, false},     {10, 0x0A0B0C}, {11, 60},       {12, 0x0D0E0F},
            {13, 4},        {14, 3},        {15, 3},        {16, 3},        {17, 0x101112}, {18, 0x131415},
            {19, true},     {20, 0x161718}, {21, 0x191A1B}, {22, 5},        {23, 7},        {24, 2},
            {25, 0x1F2021}, {26, 0x222324}, {27, 0x252627}, {28, 4},        {29, 0x28292A}, {30, 0x2B2C2D},
            {31, 5},        {32, 4},        {33, false},    {34, 2},        {35, 6},        {36, 1},
            {37, true},     {38, 0x1C1D1E}};
        QCOMPARE(body.value(4).toMap(), expectedTheme);

        const QCborMap expectedLayout{
            {1, 1},
            {2, QCborArray{QCborMap{{1, 5}, {2, 0}, {3, true}}, QCborMap{{1, 2}, {2, 0}, {3, false}},
                           QCborMap{{1, 1}, {2, 0}, {3, true}}, QCborMap{{1, 6}, {2, 1}, {3, true}},
                           QCborMap{{1, 3}, {2, 1}, {3, false}}, QCborMap{{1, 4}, {2, 1}, {3, true}}}}};
        QCOMPARE(body.value(5).toMap(), expectedLayout);

        const Profile::Content &c = page.content;
        const QCborMap expectedContent{
            {1, c.displayName},
            {2, c.headline},
            {3, QCborArray{c.infoLines.at(0), c.infoLines.at(1), c.infoLines.at(2)}},
            {4, 41}, // rockin'
            {5, QCborMap{{1, c.interests.general}, {2, c.interests.music}, {3, c.interests.movies},
                         {4, c.interests.television}, {5, c.interests.books}, {6, c.interests.heroes}}},
            {6, QCborMap{{1, 0x11}, {2, c.details.hometown}, {3, 5}, {4, c.details.occupation},
                         {5, c.details.education}, {6, c.details.languages}}},
            {7, c.aboutMe},
            {8, c.meet},
            {9, c.songTitle},
            {10, c.songArtist}};
        QCOMPARE(body.value(6).toMap(), expectedContent);

        const QCborArray expectedFriends{QCborMap{{1, accountOf(1)}, {2, u"Mia"_s}},
                                         QCborMap{{1, accountOf(2)}, {2, u"Tom"_s}},
                                         QCborMap{{1, accountOf(3)}, {2, QString()}}};
        QCOMPARE(body.value(7).toArray(), expectedFriends);

        const QCborMap expectedMedia{
            {1, QCborMap{{1, hashOf('\xB1')}, {2, 200'000}, {3, 1600}, {4, 1000}}},
            {2, QCborMap{{1, hashOf('\x50')}, {2, 180'000}, {3, 44'800}}}};
        QCOMPARE(body.value(8).toMap(), expectedMedia);
    }

    void coreEncodingIsDeterministic()
    {
        const QByteArray first = encodePageCore(richPage());
        QCOMPARE(encodePageCore(richPage()), first);
        QCOMPARE(static_cast<quint8>(first.at(0)), 0xFF);

        // encode(decode(x)) is a fixed point.
        const auto decoded = decodePageCore(first);
        QVERIFY(decoded);
        QCOMPARE(encodePageCore(*decoded), first);

        // Two pages that normalise alike encode to the same bytes.
        Page messy = richPage();
        messy.content.headline = u"  "_s + messy.content.headline + u"\u200B "_s;
        // The wide column listed first: the same arrangement, in another order.
        std::rotate(messy.modules.begin(), messy.modules.begin() + 3, messy.modules.end());
        messy.theme.nameColor |= 0xAB000000;
        QCOMPARE(encodePageCore(messy), first);

        // Canonical CBOR: keys ascend in every map, and re-serialising the
        // parsed value reproduces the bytes (shortest integer forms).
        std::function<void(const QCborValue &)> checkOrder = [&](const QCborValue &value) {
            if (value.isMap()) {
                qint64 previous = -1;
                const QCborMap map = value.toMap();
                for (auto it = map.cbegin(); it != map.cend(); ++it) {
                    QVERIFY(it.key().isInteger());
                    QVERIFY2(it.key().toInteger() > previous, "keys must ascend");
                    previous = it.key().toInteger();
                    checkOrder(it.value());
                }
            } else if (value.isArray()) {
                for (const QCborValue &entry : value.toArray())
                    checkOrder(entry);
            }
        };
        const QCborValue parsed = QCborValue::fromCbor(first.mid(1));
        checkOrder(parsed);
        QCOMPARE(parsed.toCbor(), first.mid(1));
    }

    void topFriendHandleKeyIsIgnored()
    {
        QCborMap core = coreMap();
        QCborArray friends = core.value(7).toArray();
        QCborMap spoof = friends.at(0).toMap();
        spoof.insert(3, u"@totally.real"_s);
        friends[0] = spoof;
        QCborMap weird = friends.at(1).toMap();
        weird.insert(3, 12345); // even a key 3 of another type is not looked at
        friends[1] = weird;
        core[7] = friends;

        const auto decoded = decodePageCore(tagged(core));
        QVERIFY(decoded);
        QVERIFY(decoded->topFriends == richPage().topFriends);
        const QCborArray reencoded = bodyOf(encodePageCore(*decoded)).value(7).toArray();
        for (const QCborValue &entry : reencoded)
            QVERIFY(!entry.toMap().contains(3));
    }

    void mediaRoundTripsAndVerifiesHash()
    {
        const PageMediaMessage background = mediaOf(Profile::MediaKind::BackgroundImageMedia, jpegLike(3));
        const QByteArray payload = encodePageMedia(background);
        QCOMPARE(classifyProfilePayload(payload), ProfilePayloadKind::PageMedia);
        QVERIFY(decodePageMedia(payload) == background);

        const PageMediaMessage song = mediaOf(Profile::MediaKind::SongMedia, rawSongBytes(RawSong{}));
        QVERIFY(decodePageMedia(encodePageMedia(song)) == song);

        // Wire keys: {0: 1, 1: 2, 2: kind, 3: sha256, 4: data}.
        const QCborMap body = bodyOf(payload);
        QCOMPARE(body.value(2), QCborValue(1));
        QCOMPARE(body.value(3).toByteArray(), background.sha256);
        QCOMPARE(body.value(4).toByteArray(), background.data);

        // One changed byte of data no longer matches the hash.
        QByteArray tampered = payload;
        tampered[tampered.size() - 3] = char(tampered.at(tampered.size() - 3) ^ 0x01); // entropy data
        QVERIFY(!decodePageMedia(tampered));
        // A hash that is not the data's.
        QVERIFY(!decodePageMedia(tagged(setIn(body, {3}, pageMediaHash("other")))));
        // Each kind's structural check applies to its own kind only.
        QVERIFY(!decodePageMedia(tagged(setIn(bodyOf(encodePageMedia(song)), {2}, 1))));
        QVERIFY(!decodePageMedia(tagged(setIn(body, {2}, 2))));
    }

    void requestRoundTripsWithAndWithoutRevision()
    {
        const PageRequestMessage empty;
        const QByteArray emptyPayload = encodePageRequest(empty);
        QCOMPARE(classifyProfilePayload(emptyPayload), ProfilePayloadKind::PageRequest);
        QVERIFY(decodePageRequest(emptyPayload) == empty);
        QVERIFY(!bodyOf(emptyPayload).contains(2)); // "I have no page of yours"

        const PageRequestMessage one{qint64(1'727'000'000'123), {hashOf('\x01')}};
        QVERIFY(decodePageRequest(encodePageRequest(one)) == one);
        QCOMPARE(bodyOf(encodePageRequest(one)).value(2), QCborValue(qint64(1'727'000'000'123)));

        const PageRequestMessage two{qint64(0), {hashOf('\x01'), hashOf('\x02')}};
        QVERIFY(decodePageRequest(encodePageRequest(two)) == two);

        // The encoder keeps at most six well-formed, distinct hashes.
        const PageRequestMessage messy{Profile::maxRevision,
                                       {hashOf('\x01'), hashOf('\x01'), hashOf('\x02').left(31), hashOf('\x03'),
                                        hashOf('\x04')}};
        const QByteArray messyPayload = encodePageRequest(messy);
        QVERIFY(messyPayload.size() <= maxPageRequestBytes);
        const auto decoded = decodePageRequest(messyPayload);
        QVERIFY(decoded);
        QCOMPARE(decoded->haveRevision, std::optional<qint64>(Profile::maxRevision));
        QCOMPARE(decoded->wantMedia, (QVector<QByteArray>{hashOf('\x01'), hashOf('\x03'), hashOf('\x04')}));
    }

    void requestDecoderKeepsDistinctHashes()
    {
        // Written by hand, not by the encoder (which already trims): a
        // hostile request naming one blob over and over must not make the
        // owner answer with it more than once, nor ask for more than six.
        const QByteArray h1 = hashOf('\x01'), h2 = hashOf('\x02'), h3 = hashOf('\x03');
        const QCborMap base = bodyOf(encodePageRequest({qint64(5), {}}));
        const auto requestWanting = [&](const QCborArray &wanted) { return tagged(setIn(base, {3}, wanted)); };

        const QByteArray repeated = requestWanting({h1, h1, h2, h3, h1});
        QVERIFY(repeated.size() <= maxPageRequestBytes);
        auto decoded = decodePageRequest(repeated);
        QVERIFY(decoded);
        QCOMPARE(decoded->haveRevision, std::optional<qint64>(5));
        QCOMPARE(decoded->wantMedia, (QVector<QByteArray>{h1, h2, h3}));

        // As many copies of one hash as fit under the size cap still ask for it once.
        QCborArray copies;
        while (requestWanting(copies + h1).size() <= maxPageRequestBytes)
            copies.append(h1);
        QVERIFY(copies.size() >= 6);
        decoded = decodePageRequest(requestWanting(copies));
        QVERIFY(decoded);
        QCOMPARE(decoded->wantMedia, QVector<QByteArray>{h1});

        // Every entry is checked, even past the two kept: a malformed third
        // one still rejects the message.
        QVERIFY(!decodePageRequest(requestWanting({h1, h2, h3.left(31)})));
        QVERIFY(!decodePageRequest(requestWanting({h1, h1, h1, u"hash"_s})));
    }

    void decodeRejectsMalformed_data()
    {
        QTest::addColumn<QByteArray>("payload");
        QTest::addColumn<int>("kind");
        const auto legacy = int(ProfilePayloadKind::Legacy);
        const auto unknown = int(ProfilePayloadKind::UnknownPage);
        const auto core = int(ProfilePayloadKind::PageCore);
        const auto media = int(ProfilePayloadKind::PageMedia);
        const auto request = int(ProfilePayloadKind::PageRequest);
        const QCborMap valid = coreMap();
        const QCborMap background = mediaMap(Profile::MediaKind::BackgroundImageMedia, jpegLike(2));
        const QCborMap song = mediaMap(Profile::MediaKind::SongMedia, rawSongBytes(RawSong{}));
        const QCborMap wanted = bodyOf(encodePageRequest({qint64(5), {hashOf('\x01')}}));
        const auto withMediaData = [&](Profile::MediaKind kind, const QByteArray &data) {
            return tagged(mediaMap(kind, data));
        };

        QTest::newRow("empty") << QByteArray() << legacy;
        QTest::newRow("untagged") << QCborValue(valid).toCbor() << legacy;
        QTest::newRow("tag only") << QByteArray(1, '\xFF') << unknown;
        QTest::newRow("tag+array") << QByteArray(1, '\xFF') + QCborValue(QCborArray{1, 1}).toCbor() << unknown;
        QTest::newRow("tag+garbage") << taggedRaw("ff0013") << unknown;
        QTest::newRow("tag+truncated map") << encodePageCore(richPage()).chopped(3) << unknown;
        QTest::newRow("trailing bytes") << encodePageCore(richPage()) + QByteArray(1, '\0') << unknown;
        QTest::newRow("duplicate key") << taggedRaw("a3000101010101") << unknown;
        QTest::newRow("version 2") << tagged(setIn(valid, {0}, 2)) << unknown;
        QTest::newRow("version missing") << tagged(removeIn(valid, {0})) << unknown;
        QTest::newRow("version as text") << tagged(setIn(valid, {0}, u"1"_s)) << unknown;
        QTest::newRow("missing type") << tagged(removeIn(valid, {1})) << unknown;
        QTest::newRow("unknown type") << tagged(setIn(valid, {1}, 4)) << unknown;
        QTest::newRow("unknown media kind") << tagged(setIn(background, {2}, 9)) << unknown;
        QTest::newRow("oversize media message")
            << tagged(setIn(mediaMap(Profile::MediaKind::BackgroundImageMedia,
                                     jpegLike(1, maxBackgroundImageBytes)),
                            {60}, filled(1024, 'x')))
            << unknown;

        QTest::newRow("wrong CBOR type: revision as text") << tagged(setIn(valid, {2}, u"7"_s)) << core;
        QTest::newRow("wrong CBOR type: negative revision") << tagged(setIn(valid, {2}, -7)) << core;
        QTest::newRow("wrong CBOR type: uint above int64") << taggedRaw("a300010101021bffffffffffffffff") << core;
        QTest::newRow("wrong CBOR type: float revision") << tagged(setIn(valid, {2}, 7.0)) << core;
        QTest::newRow("wrong CBOR type: theme as array") << tagged(setIn(valid, {4}, QCborArray{1})) << core;
        QTest::newRow("wrong CBOR type: colour as text") << tagged(setIn(valid, {4, 2}, u"blue"_s)) << core;
        QTest::newRow("wrong CBOR type: bool as integer") << tagged(setIn(valid, {4, 9}, 1)) << core;
        QTest::newRow("wrong CBOR type: negative opacity") << tagged(setIn(valid, {4, 6}, -1)) << core;
        QTest::newRow("wrong CBOR type: layout as text") << tagged(setIn(valid, {5, 1}, u"classic"_s)) << core;
        QTest::newRow("wrong CBOR type: modules as map") << tagged(setIn(valid, {5, 2}, QCborMap())) << core;
        QTest::newRow("wrong CBOR type: module entry") << tagged(setIn(valid, {5, 2}, QCborArray{7})) << core;
        QTest::newRow("wrong CBOR type: module visible")
            << tagged(setIn(valid, {5, 2}, QCborArray{QCborMap{{1, 1}, {2, 0}, {3, 1}}})) << core;
        QTest::newRow("wrong CBOR type: content as array") << tagged(setIn(valid, {6}, QCborArray())) << core;
        QTest::newRow("wrong CBOR type: headline as bytes") << tagged(setIn(valid, {6, 2}, QByteArray("x"))) << core;
        QTest::newRow("wrong CBOR type: info line") << tagged(setIn(valid, {6, 3}, QCborArray{u"a"_s, 5})) << core;
        QTest::newRow("wrong CBOR type: mood as text") << tagged(setIn(valid, {6, 4}, u"happy"_s)) << core;
        QTest::newRow("wrong CBOR type: interest") << tagged(setIn(valid, {6, 5, 2}, 3)) << core;
        QTest::newRow("wrong CBOR type: here for") << tagged(setIn(valid, {6, 6, 1}, u"friends"_s)) << core;
        QTest::newRow("wrong CBOR type: friends as map") << tagged(setIn(valid, {7}, QCborMap())) << core;
        QTest::newRow("wrong CBOR type: friend entry") << tagged(setIn(valid, {7}, QCborArray{accountOf(1)})) << core;
        QTest::newRow("wrong CBOR type: friend name")
            << tagged(setIn(valid, {7}, QCborArray{QCborMap{{1, accountOf(1)}, {2, 5}}})) << core;
        QTest::newRow("wrong CBOR type: preset as text") << tagged(setIn(valid, {9}, u"linen"_s)) << core;
        QTest::newRow("friend without account id")
            << tagged(setIn(valid, {7}, QCborArray{QCborMap{{2, u"Mia"_s}}})) << core;
        QTest::newRow("15-byte account id")
            << tagged(setIn(valid, {7}, QCborArray{QCborMap{{1, accountOf(1).left(15)}, {2, u"Mia"_s}}})) << core;
        QTest::newRow("17-byte account id")
            << tagged(setIn(valid, {7}, QCborArray{QCborMap{{1, accountOf(1) + 'x'}}})) << core;
        QTest::newRow("31-byte background hash") << tagged(setIn(valid, {8, 1, 1}, hashOf('\x01').left(31))) << core;
        QTest::newRow("33-byte song hash") << tagged(setIn(valid, {8, 2, 1}, hashOf('\x01') + 'x')) << core;
        QTest::newRow("ref without hash") << tagged(removeIn(valid, {8, 2, 1})) << core;
        QTest::newRow("background ref not a map") << tagged(setIn(valid, {8, 1}, hashOf('\x01'))) << core;
        QTest::newRow("ref bytes as text") << tagged(setIn(valid, {8, 1, 2}, u"1"_s)) << core;
        QTest::newRow("oversize core") << tagged(setIn(valid, {60}, filled(maxPageCoreBytes, 'x'))) << core;

        QTest::newRow("31-byte media hash") << tagged(setIn(background, {3}, hashOf('\x01').left(31))) << media;
        QTest::newRow("media without data") << tagged(removeIn(background, {4})) << media;
        QTest::newRow("media without kind") << tagged(removeIn(background, {2})) << media;
        QTest::newRow("media data as text") << tagged(setIn(background, {4}, u"jpeg"_s)) << media;
        QTest::newRow("hash mismatch") << tagged(setIn(background, {3}, hashOf('\x01'))) << media;
        QTest::newRow("oversize background data")
            << withMediaData(Profile::MediaKind::BackgroundImageMedia, jpegLike(1, maxBackgroundImageBytes + 1))
            << media;
        QTest::newRow("missing JPEG magic")
            << withMediaData(Profile::MediaKind::BackgroundImageMedia, jpegLike(1).mid(2)) << media;
        QTest::newRow("JPEG with 33 scans")
            << withMediaData(Profile::MediaKind::BackgroundImageMedia, jpegLike(33)) << media;
        QTest::newRow("JPEG with no scan") << withMediaData(Profile::MediaKind::BackgroundImageMedia, jpegLike(0))
                                           << media;
        QTest::newRow("JPEG cut short")
            << withMediaData(Profile::MediaKind::BackgroundImageMedia, jpegLike(2).chopped(2)) << media;
        QTest::newRow("bad song container")
            << withMediaData(Profile::MediaKind::SongMedia, rawSongBytes(RawSong{}).chopped(1)) << media;
        QTest::newRow("JPEG sent as a song") << tagged(setIn(background, {2}, 2)) << media;
        QTest::newRow("song sent as a JPEG") << tagged(setIn(song, {2}, 1)) << media;
        RawSong oversizeSong = largestSong();
        oversizeSong.packets.last().append('\x42');
        QTest::newRow("oversize song data")
            << withMediaData(Profile::MediaKind::SongMedia, rawSongBytes(oversizeSong)) << media;

        QTest::newRow("31-byte wanted hash") << tagged(setIn(wanted, {3}, QCborArray{hashOf('\x01').left(31)}))
                                             << request;
        QTest::newRow("wanted hash as text") << tagged(setIn(wanted, {3}, QCborArray{u"hash"_s})) << request;
        QTest::newRow("wanted media not an array") << tagged(setIn(wanted, {3}, hashOf('\x01'))) << request;
        QTest::newRow("have revision as text") << tagged(setIn(wanted, {2}, u"5"_s)) << request;
        QTest::newRow("oversize request") << tagged(setIn(wanted, {60}, filled(maxPageRequestBytes, 'x')))
                                          << request;
    }

    void decodeRejectsMalformed()
    {
        QFETCH(QByteArray, payload);
        QFETCH(int, kind);
        // Each row changes one thing in a message that decodes (the baselines below).
        QVERIFY(decodePageCore(encodePageCore(richPage())));
        QVERIFY(decodePageMedia(tagged(mediaMap(Profile::MediaKind::BackgroundImageMedia, jpegLike(2)))));
        QVERIFY(decodePageMedia(tagged(mediaMap(Profile::MediaKind::SongMedia, rawSongBytes(RawSong{})))));
        QVERIFY(decodePageRequest(encodePageRequest({qint64(5), {hashOf('\x01')}})));

        QCOMPARE(classifyProfilePayload(payload), static_cast<ProfilePayloadKind>(kind));
        QVERIFY(!decodePageCore(payload));
        QVERIFY(!decodePageMedia(payload));
        QVERIFY(!decodePageRequest(payload));
    }

    void decodeIgnoresUnknownKeys()
    {
        const Page expected = richPage();
        QCborMap core = coreMap();
        core.insert(60, u"future top-level field"_s);
        core.insert(1000, 5);
        core.insert(-3, true);
        core.insert(u"text key"_s, QCborArray{1, 2});
        core = setIn(core, {4, 39}, u"a future theme knob"_s);
        core = setIn(core, {4, 63}, QCborMap{{1, 2}});
        core = setIn(core, {5, 9}, 1);
        core = setIn(core, {6, 11}, u"pronouns"_s);
        core = setIn(core, {6, 5, 7}, u"Podcasts"_s);
        core = setIn(core, {6, 6, 7}, 3);
        core = setIn(core, {8, 3}, QCborMap{{1, hashOf('\x07')}});
        core = setIn(core, {8, 1, 5}, u"a future ref field"_s);
        core = setIn(core, {8, 2, 4}, u"key 4 is not a song field"_s);
        QCborArray modules = core.value(5).toMap().value(2).toArray();
        QCborMap firstModule = modules.at(0).toMap();
        firstModule.insert(9, u"future"_s);
        modules[0] = firstModule;
        core = setIn(core, {5, 2}, modules);
        QCborArray friends = core.value(7).toArray();
        QCborMap firstFriend = friends.at(0).toMap();
        firstFriend.insert(9, QByteArray("future"));
        friends[0] = firstFriend;
        core[7] = friends;

        const auto decoded = decodePageCore(tagged(core));
        QVERIFY(decoded);
        compareThemes(decoded->theme, expected.theme);
        if (QTest::currentTestFailed())
            return;
        QVERIFY(*decoded == expected);

        QCborMap mediaBody = mediaMap(Profile::MediaKind::BackgroundImageMedia, jpegLike(1));
        mediaBody.insert(9, u"future"_s);
        QVERIFY(decodePageMedia(tagged(mediaBody)));
        QCborMap requestBody = bodyOf(encodePageRequest({qint64(3), {}}));
        requestBody.insert(9, u"future"_s);
        QVERIFY(decodePageRequest(tagged(requestBody)) == PageRequestMessage({qint64(3), {}}));

        // Absent optional maps read as their defaults.
        QCborMap bare;
        bare.insert(0, 1);
        bare.insert(1, 1);
        const auto bareDecoded = decodePageCore(tagged(bare));
        QVERIFY(bareDecoded);
        QVERIFY(*bareDecoded == Profile::defaultPage());
    }

    void decodeClampsFutureValues()
    {
        const auto decodeWith = [](const QCborMap &core) {
            const auto page = decodePageCore(tagged(core));
            if (!page)
                QTest::qFail("a future value was rejected instead of clamped", __FILE__, __LINE__);
            return page.value_or(Page{});
        };
        const QCborMap valid = coreMap();

        const Page numbers = decodeWith(setIn(setIn(setIn(valid, {4, 6}, 200), {4, 11}, 20), {4, 13}, 9));
        QCOMPARE(numbers.theme.motifOpacity, 80);
        QCOMPARE(numbers.theme.boxOpacity, 60);
        QCOMPARE(numbers.theme.borderWidth, 4);
        QCOMPARE(decodeWith(setIn(valid, {4, 6}, 1)).theme.motifOpacity, 10);
        QCOMPARE(decodeWith(setIn(valid, {4, 11}, 5000)).theme.boxOpacity, 100);

        const Theme defaults;
        QCOMPARE(decodeWith(setIn(valid, {4, 4}, 40)).theme.motif, defaults.motif);
        QCOMPARE(decodeWith(setIn(valid, {4, 1}, 9)).theme.backgroundKind, defaults.backgroundKind);
        QCOMPARE(decodeWith(setIn(valid, {4, 22}, 20)).theme.headingFont, Font::InterfaceFont);
        QCOMPARE(decodeWith(setIn(valid, {4, 22}, 4)).theme.headingFont, Font::InterfaceFont); // Pixel heading
        QCOMPARE(decodeWith(setIn(valid, {4, 23}, 8)).theme.bodyFont, Font::InterfaceFont);    // Marker body
        QCOMPARE(decodeWith(setIn(valid, {4, 34}, 7)).theme.nameSize, defaults.nameSize);
        QCOMPARE(decodeWith(setIn(valid, {4, 35}, 99)).theme.nameFlourish, defaults.nameFlourish);
        QCOMPARE(decodeWith(setIn(valid, {4, 36}, 3)).theme.tableStyle, defaults.tableStyle);
        QCOMPARE(decodeWith(setIn(valid, {4, 32}, 9)).theme.ambient, defaults.ambient);
        QCOMPARE(decodeWith(setIn(valid, {4, 8}, 8)).theme.imageMode, defaults.imageMode);
        // A value past 255 must not wrap onto a valid one (259 & 0xFF would be Gradient).
        QCOMPARE(decodeWith(setIn(valid, {4, 31}, 259)).theme.nameEffect, defaults.nameEffect);
        QCOMPARE(decodeWith(setIn(valid, {9}, 258)).preset, Preset::CustomPreset);
        QCOMPARE(decodeWith(setIn(valid, {9}, 77)).preset, Preset::CustomPreset);
        QCOMPARE(decodeWith(setIn(valid, {9}, 255)).preset, Preset::CustomPreset);
        QCOMPARE(decodeWith(setIn(valid, {5, 1}, 5)).layout, Profile::Layout::ClassicLayout);

        // Colours keep their low 24 bits; here-for keeps its six bits.
        QCOMPARE(decodeWith(setIn(valid, {4, 2}, qint64(0x7FFFFFFF))).theme.backgroundColor1, 0xFFFFFFu);
        QCOMPARE(decodeWith(setIn(valid, {4, 38}, qint64(0x1234567890))).theme.altBorderColor, 0x567890u);
        QCOMPARE(decodeWith(setIn(valid, {6, 6, 1}, 0x1C1)).content.details.hereFor, quint8(0x01));
        QCOMPARE(decodeWith(setIn(valid, {6, 4}, 99)).content.mood, Profile::Mood::NoMood);
        QCOMPARE(decodeWith(setIn(valid, {6, 6, 3}, 20)).content.details.zodiac, Profile::Zodiac::NoZodiac);

        // A sender from before key 38 drew wide boxes with the main border.
        const Page noAltBorder = decodeWith(removeIn(valid, {4, 38}));
        QCOMPARE(noAltBorder.theme.altBorderColor, noAltBorder.theme.borderColor);

        QCOMPARE(decodeWith(setIn(valid, {2}, qint64(1) << 60)).revision, Profile::maxRevision);
        QCOMPARE(decodeWith(setIn(valid, {3}, qint64(1) << 60)).publishedAtMs, Profile::maxRevision);

        // Lists keep their first entries.
        QCOMPARE(decodeWith(setIn(valid, {6, 3}, QCborArray{u"1"_s, u"2"_s, u"3"_s, u"4"_s, u"5"_s}))
                     .content.infoLines,
                 (QStringList{u"1"_s, u"2"_s, u"3"_s}));
        QCborArray twelve;
        for (char i = 1; i <= 12; ++i)
            twelve.append(QCborMap{{1, accountOf(i)}, {2, QString::number(i)}});
        const Page capped = decodeWith(setIn(valid, {7}, twelve));
        QCOMPARE(capped.topFriends.size(), Profile::maxTopFriends);
        QCOMPARE(capped.topFriends.last().accountId, accountOf(8));
        // Only the first sixteen module entries are read.
        QCborArray modules;
        for (int i = 0; i < 16; ++i)
            modules.append(QCborMap{{1, 99}, {2, 0}});
        modules.append(QCborMap{{1, int(Module::TopFriendsModule)}, {2, 0}, {3, false}});
        const Page arranged = decodeWith(setIn(valid, {5, 2}, modules));
        QVERIFY(arranged.modules == Profile::defaultModules());

        // Text over its bound is sanitised, then cut.
        const Page longText = decodeWith(setIn(valid, {6, 2}, QString(500, u'h')));
        QCOMPARE(longText.content.headline, QString(Profile::TextBounds::headline, u'h'));
        QCOMPARE(decodeWith(setIn(valid, {6, 1}, u" \u202EDana\u200B "_s)).content.displayName, u"Dana"_s);

        // Refs out of range drop only themselves; an image background without its ref falls back to solid.
        const Page wide = decodeWith(setIn(valid, {8, 1, 3}, 4000));
        QVERIFY(!wide.background.isSet());
        QVERIFY(wide.song.isSet());
        QCOMPARE(wide.theme.backgroundKind, Profile::BackgroundKind::SolidBackground);
        QVERIFY(!decodeWith(setIn(valid, {8, 2, 3}, 60'000)).song.isSet());
        QVERIFY(!decodeWith(setIn(valid, {8, 2, 2}, 0)).song.isSet());
        QVERIFY(!decodeWith(setIn(valid, {8, 1, 2}, qint64(1) << 40)).background.isSet());
    }

    void classifySeparatesLegacyFromPages()
    {
        ProfileUpdateMessage legacy;
        legacy.presence = 1;
        legacy.statusText = u"Around"_s;
        QCOMPARE(classifyProfilePayload(encodeProfileUpdate(legacy)), ProfilePayloadKind::Legacy);
        QCOMPARE(classifyProfilePayload(encodeProfileUpdate({})), ProfilePayloadKind::Legacy);
        QCOMPARE(classifyProfilePayload(QByteArray()), ProfilePayloadKind::Legacy);

        QCOMPARE(classifyProfilePayload(encodePageCore(richPage())), ProfilePayloadKind::PageCore);
        QCOMPARE(classifyProfilePayload(encodePageCore(Page{})), ProfilePayloadKind::PageCore);
        QCOMPARE(classifyProfilePayload(
                     encodePageMedia(mediaOf(Profile::MediaKind::BackgroundImageMedia, jpegLike(1)))),
                 ProfilePayloadKind::PageMedia);
        QCOMPARE(classifyProfilePayload(
                     encodePageMedia(mediaOf(Profile::MediaKind::SongMedia, rawSongBytes(RawSong{})))),
                 ProfilePayloadKind::PageMedia);
        QCOMPARE(classifyProfilePayload(encodePageRequest({})), ProfilePayloadKind::PageRequest);

        // Tagged but from a future version, of a future type, or unreadable: ignored silently.
        const QCborMap core = coreMap();
        QCOMPARE(classifyProfilePayload(tagged(setIn(core, {0}, 2))), ProfilePayloadKind::UnknownPage);
        QCOMPARE(classifyProfilePayload(tagged(setIn(core, {1}, 9))), ProfilePayloadKind::UnknownPage);
        QCOMPARE(classifyProfilePayload(QByteArray(1, '\xFF')), ProfilePayloadKind::UnknownPage);
        QCOMPARE(classifyProfilePayload(taggedRaw("0102")), ProfilePayloadKind::UnknownPage);

        // The legacy decoder still reads what classify calls Legacy.
        QVERIFY(decodeProfileUpdate(encodeProfileUpdate(legacy)) == legacy);
    }

    void legacyDecoderIgnoresEveryPageMessage()
    {
        const QList<QByteArray> pageMessages{
            encodePageCore(Page{}),
            encodePageCore(richPage()),
            encodePageMedia(mediaOf(Profile::MediaKind::BackgroundImageMedia, jpegLike(1))),
            encodePageMedia(mediaOf(Profile::MediaKind::BackgroundImageMedia, jpegLike(1, maxBackgroundImageBytes))),
            encodePageMedia(mediaOf(Profile::MediaKind::SongMedia, rawSongBytes(RawSong{}))),
            encodePageMedia(mediaOf(Profile::MediaKind::SongMedia, rawSongBytes(largestSong()))),
            encodePageRequest({}),
            encodePageRequest({qint64(9), {hashOf('\x01'), hashOf('\x02')}}),
        };
        for (const QByteArray &payload : pageMessages) {
            // 0.2.8's decoder: nullopt, so ChatController drops the message.
            QVERIFY(!decodeProfileUpdate(payload));
            // And the reason is the tag itself: the CBOR "break" byte cannot
            // start an item, so parsing fails before any shape check.
            QCborParserError error;
            QCborValue::fromCbor(payload, &error);
            QVERIFY(error.error != QCborError::NoError);
        }
    }

    void largestCoreFitsTheBudget()
    {
        // Every text at its bound in a character that takes three UTF-8
        // bytes per UTF-16 unit (the worst case), eight friends, both refs.
        using B = Profile::TextBounds;
        const auto wide = [](int length) { return QString(length, QChar(0x4E2D)); };
        Page page = richPage();
        page.revision = page.publishedAtMs = Profile::maxRevision;
        Profile::Content &c = page.content;
        c.displayName = wide(B::displayName);
        c.headline = wide(B::headline);
        c.infoLines = {wide(B::infoLine), wide(B::infoLine), wide(B::infoLine)};
        c.interests = {wide(B::interest), wide(B::interest), wide(B::interest),
                       wide(B::interest), wide(B::interest), wide(B::interest)};
        c.details.hometown = c.details.occupation = c.details.education = c.details.languages = wide(B::detail);
        c.aboutMe = wide(B::aboutMe);
        c.meet = wide(B::meet);
        c.songTitle = wide(B::songTitle);
        c.songArtist = wide(B::songArtist);
        page.topFriends.clear();
        for (char i = 1; i <= Profile::maxTopFriends; ++i)
            page.topFriends.push_back({accountOf(i), wide(B::friendName)});
        page.background = {hashOf('\x01'), quint32(maxBackgroundImageBytes), 1920, 1920, 0};
        page.song = {hashOf('\x02'), quint32(maxSongBytes), 0, 0, Profile::maxSongRefDurationMs};
        for (quint32 *colour : {&page.theme.backgroundColor1, &page.theme.nameColor, &page.theme.altBorderColor})
            *colour = 0xFFFFFF;

        const QByteArray payload = encodePageCore(page);
        const int textUnits = B::displayName + B::headline + 3 * B::infoLine + 6 * B::interest + 4 * B::detail
            + B::aboutMe + B::meet + B::songTitle + B::songArtist + Profile::maxTopFriends * B::friendName;
        QVERIFY2(payload.size() > 3 * textUnits, "the fixture must really fill every field");
        // A page without panels always reaches a 0.2.9 client, whose cap is the old one.
        QVERIFY2(payload.size() <= legacyMaxPageCoreBytes, qPrintable(QString::number(payload.size())));
        const auto decoded = decodePageCore(payload);
        QVERIFY(decoded);
        QVERIFY(*decoded == Profile::normalized(page));
    }

    void largestMediaMessagesFitUnderTheMlsCap()
    {
        QCOMPARE(mlsPlaintextCapBytes, 262'144);
        QCOMPARE(maxPageSendBytes, 245'760);
        QCOMPARE(maxBackgroundImageBytes, 229'376);
        QCOMPARE(maxSongBytes, 229'376);
        QCOMPARE(maxPageMediaMessageBytes, 229'888);
        QCOMPARE(maxPageCoreBytes, 98'304);
        QCOMPARE(legacyMaxPageCoreBytes, 24'576);
        QCOMPARE(maxRequestedMedia, 6);
        QCOMPARE(maxPageRequestBytes, 256);
        // The other wire bounds clients must agree on, and the scan limit
        // that keeps a progressive "scan bomb" out. Tests elsewhere build
        // their edge cases from these constants, so they are pinned here.
        QCOMPARE(maxJpegScans, 32);
        QCOMPARE(SongContainer::maxPreSkip, 3840);
        QCOMPARE(SongContainer::maxTotalSamples, qint64(2'184'000));
        QCOMPARE(Profile::maxRevision, (qint64(1) << 53) - 1);
        QCOMPARE(Profile::maxRevision, qint64(9'007'199'254'740'991));

        // A JPEG at the scan limit is accepted (33 is refused in decodeRejectsMalformed).
        const PageMediaMessage mostScans = mediaOf(Profile::MediaKind::BackgroundImageMedia, jpegLike(32));
        QVERIFY(decodePageMedia(encodePageMedia(mostScans)) == mostScans);

        const QByteArray jpeg = jpegLike(12, maxBackgroundImageBytes);
        const QByteArray song = rawSongBytes(largestSong());
        QCOMPARE(jpeg.size(), maxBackgroundImageBytes);
        QCOMPARE(song.size(), maxSongBytes);

        for (const PageMediaMessage &message :
             {mediaOf(Profile::MediaKind::BackgroundImageMedia, jpeg), mediaOf(Profile::MediaKind::SongMedia, song)}) {
            const QByteArray payload = encodePageMedia(message);
            QVERIFY2(payload.size() <= maxPageMediaMessageBytes, qPrintable(QString::number(payload.size())));
            QVERIFY(payload.size() <= maxPageSendBytes);
            QVERIFY(payload.size() < mlsPlaintextCapBytes);
            QVERIFY(decodePageMedia(payload) == message);
        }
    }

    void jpegScanCountWalksMarkers()
    {
        QCOMPARE(jpegScanCount(jpegLike(1)), 1);
        QCOMPARE(jpegScanCount(jpegLike(10)), 10);
        QCOMPARE(jpegScanCount(jpegLike(33)), 33);
        QCOMPARE(jpegScanCount(jpegLike(0)), 0);
        QCOMPARE(jpegScanCount(jpegLike(2, 5000)), 2);

        // Bytes inside entropy data that look like SOS do not count: FF DA only
        // counts as a marker, and FF 00 DA is stuffed data.
        QByteArray stuffed = jpegLike(1);
        stuffed.insert(stuffed.size() - 2, QByteArray::fromHex("ff00dada"));
        QCOMPARE(jpegScanCount(stuffed), 1);

        // Fill bytes before a marker are allowed.
        QByteArray fill = jpegLike(1);
        fill.insert(2, QByteArray::fromHex("ffff"));
        QCOMPARE(jpegScanCount(fill), 1);

        // Data after the end of the image is not walked.
        QCOMPARE(jpegScanCount(jpegLike(1) + QByteArray("trailing\xFF\xDA", 10)), 1);

        QCOMPARE(jpegScanCount(QByteArray()), -1);
        QCOMPARE(jpegScanCount(jpegLike(1).mid(2)), -1);         // no SOI
        QCOMPARE(jpegScanCount(jpegLike(1).chopped(2)), -1);     // no EOI
        QCOMPARE(jpegScanCount(jpegLike(1).chopped(1)), -1);     // EOI cut in half
        QByteArray badLength = jpegLike(1);
        badLength[4] = '\x7F'; // APP0 claims 32 KB
        QCOMPARE(jpegScanCount(badLength), -1);
        QByteArray shortLength = jpegLike(1);
        shortLength[4] = '\0';
        shortLength[5] = '\x01'; // a length below its own two bytes
        QCOMPARE(jpegScanCount(shortLength), -1);
        QByteArray stray = jpegLike(1);
        stray.insert(2, '\x00'); // not a marker where one belongs
        QCOMPARE(jpegScanCount(stray), -1);
        QByteArray nestedStart = jpegLike(1);
        nestedStart.insert(2, QByteArray::fromHex("ffd8"));
        QCOMPARE(jpegScanCount(nestedStart), -1);
    }

    // --- Song container ------------------------------------------------------

    void containerRoundTrips()
    {
        SongContainer stereo;
        stereo.channels = 2;
        stereo.frameSamples = 960;
        stereo.preSkip = 312;
        stereo.totalSamples = 20 * 960 - 312 - 100; // the last packet is part-filled
        stereo.gainQ8 = -512;
        for (int i = 0; i < 20; ++i)
            stereo.packets.push_back(filled(1 + (i * 97) % SongContainer::maxPacketBytes, char(i)));
        const QByteArray bytes = encodeSongContainer(stereo);
        QVERIFY(looksLikeSongContainer(bytes));
        const auto decoded = decodeSongContainer(bytes);
        QVERIFY(decoded);
        QVERIFY(*decoded == stereo);
        QCOMPARE(decoded->durationMs(), (20 * 960 - 412) * 1000 / 48'000);

        // The layout is the spec's, byte for byte.
        QCOMPARE(bytes.left(4), QByteArray("OCSG"));
        QCOMPARE(quint8(bytes.at(4)), 1);
        QCOMPARE(quint8(bytes.at(5)), 2);
        QCOMPARE(qFromLittleEndian<quint16>(bytes.constData() + 6), 0);
        QCOMPARE(qFromLittleEndian<quint32>(bytes.constData() + 8), 48'000u);
        QCOMPARE(qFromLittleEndian<quint16>(bytes.constData() + 12), 960);
        QCOMPARE(qFromLittleEndian<quint16>(bytes.constData() + 14), 312);
        QCOMPARE(qFromLittleEndian<quint32>(bytes.constData() + 16), quint32(stereo.totalSamples));
        QCOMPARE(qFromLittleEndian<qint16>(bytes.constData() + 20), -512);
        QCOMPARE(qFromLittleEndian<quint32>(bytes.constData() + 22), 20u);
        QCOMPARE(qFromLittleEndian<quint16>(bytes.constData() + 26), quint16(stereo.packets.at(0).size()));
        QCOMPARE(bytes.mid(28, stereo.packets.at(0).size()), stereo.packets.at(0));

        // The longest song, with the one spare packet the count allows.
        SongContainer longest;
        longest.frameSamples = 2880;
        longest.preSkip = 3840;           // the spec's bounds written out, so a
        longest.totalSamples = 2'184'000; // constant that drifted still fails
        const int needed = int((longest.totalSamples + longest.preSkip + 2879) / 2880);
        longest.packets = QVector<QByteArray>(needed + 1, filled(40, '\x33'));
        QVERIFY(decodeSongContainer(encodeSongContainer(longest)) == longest);

        // The decoder reads what the spec's own layout says, not just what the encoder writes.
        const auto raw = decodeSongContainer(rawSongBytes(RawSong{}));
        QVERIFY(raw);
        QCOMPARE(raw->channels, 1);
        QCOMPARE(raw->frameSamples, 2880);
        QCOMPARE(raw->preSkip, 312);
        QCOMPARE(raw->packets.size(), 10);

        // The encoder refuses a song that breaks a rule rather than writing one no one can read.
        SongContainer broken = stereo;
        broken.channels = 3;
        QVERIFY(encodeSongContainer(broken).isEmpty());
        broken = stereo;
        broken.packets.removeLast();
        QVERIFY(encodeSongContainer(broken).isEmpty());
        broken = stereo;
        broken.packets[3] = QByteArray();
        QVERIFY(encodeSongContainer(broken).isEmpty());
        broken = stereo;
        broken.frameSamples = 1000;
        QVERIFY(encodeSongContainer(broken).isEmpty());

        QVERIFY(!looksLikeSongContainer(QByteArray("OCS")));
        QVERIFY(!looksLikeSongContainer(QByteArray("RIFF....")));
    }

    void containerRejectsMalformed_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        const auto row = [](const char *tag, const std::function<void(RawSong &)> &change) {
            RawSong song;
            change(song);
            QTest::newRow(tag) << rawSongBytes(song);
        };
        row("magic", [](RawSong &s) { s.magic = "OCSX"; });
        row("version", [](RawSong &s) { s.version = 2; });
        row("channels 0", [](RawSong &s) { s.channels = 0; });
        row("channels 3", [](RawSong &s) { s.channels = 3; });
        row("reserved", [](RawSong &s) { s.reserved = 1; });
        row("44.1 kHz", [](RawSong &s) { s.sampleRate = 44'100; });
        // Each of these keeps the ten packets consistent with its own frame
        // size and pre-skip, so only the rule it names is broken.
        const auto frameRow = [&](const char *tag, quint16 frameSamples) {
            row(tag, [frameSamples](RawSong &s) {
                s.frameSamples = frameSamples;
                s.totalSamples = 10u * frameSamples - s.preSkip;
            });
        };
        frameRow("frameSamples 480", 480);
        frameRow("frameSamples 1000", 1000);
        frameRow("frameSamples 2881", 2881);
        frameRow("frameSamples 65535", 65535);
        // Zero would divide the packet count's arithmetic: it must be
        // refused before anything computes with it.
        row("frameSamples 0", [](RawSong &s) { s.frameSamples = 0; });
        row("preSkip 3841", [](RawSong &s) {
            s.preSkip = 3841;
            s.totalSamples = 10 * 2880 - 3841;
        });
        row("preSkip 65535", [](RawSong &s) {
            s.preSkip = 65535;
            s.totalSamples = 30 * 2880 - 65535;
            s.packets = QVector<QByteArray>(30, filled(1, '\x42'));
        });
        row("totalSamples 0", [](RawSong &s) {
            s.totalSamples = 0;
            s.packets.resize(1);
        });
        row("totalSamples over cap", [](RawSong &s) {
            s.totalSamples = 2'184'001;
            s.packets = QVector<QByteArray>(int((s.totalSamples + s.preSkip + 2879) / 2880), filled(1, '\x42'));
        });
        row("packetCount inconsistent: short", [](RawSong &s) { s.packets.removeLast(); });
        row("packetCount inconsistent: two spare", [](RawSong &s) {
            s.packets.push_back(filled(10, '\x42'));
            s.packets.push_back(filled(10, '\x42'));
        });
        row("packetCount larger than the packets", [](RawSong &s) { s.packetCount = 11; });
        row("length 0", [](RawSong &s) { s.packets[4] = QByteArray(); });
        row("length 1276", [](RawSong &s) { s.packets[4] = filled(1276, '\x42'); });
        row("trailing bytes", [](RawSong &s) { s.trailing = QByteArray(1, '\0'); });
        row("over 224 KiB", [](RawSong &s) {
            s = largestSong();
            s.packets.last().append('\x42'); // one byte over, every other rule kept
        });
        QTest::newRow("truncated packet") << rawSongBytes(RawSong{}).chopped(1);
        QTest::newRow("truncated length") << rawSongBytes(RawSong{}).chopped(101);
        QTest::newRow("truncated header") << rawSongBytes(RawSong{}).left(25);
        QTest::newRow("empty") << QByteArray();
    }

    void containerRejectsMalformed()
    {
        QFETCH(QByteArray, bytes);
        QVERIFY(decodeSongContainer(rawSongBytes(RawSong{}))); // each row breaks one rule of this
        QVERIFY(decodeSongContainer(rawSongBytes(largestSong())));
        QVERIFY(!decodeSongContainer(bytes));
    }

    // The accepting side of each bound the rejections above test, so a rule
    // drawn one step too tight fails as surely as one drawn too loose.
    void containerAcceptsEveryBound_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        const auto row = [](const QByteArray &tag, const std::function<void(RawSong &)> &change) {
            RawSong song;
            change(song);
            QTest::newRow(tag.constData()) << rawSongBytes(song);
        };
        for (const quint16 frameSamples : {quint16(960), quint16(1920), quint16(2880)}) {
            row("frameSamples " + QByteArray::number(frameSamples), [frameSamples](RawSong &s) {
                s.frameSamples = frameSamples;
                s.totalSamples = 10u * frameSamples - s.preSkip;
            });
        }
        row("preSkip 0", [](RawSong &s) {
            s.preSkip = 0;
            s.totalSamples = 10 * 2880;
        });
        row("preSkip 3840", [](RawSong &s) {
            s.preSkip = 3840;
            s.totalSamples = 10 * 2880 - 3840;
        });
        row("totalSamples 1", [](RawSong &s) {
            s.totalSamples = 1;
            s.packets.resize(1);
        });
        row("totalSamples at cap", [](RawSong &s) {
            s.totalSamples = 2'184'000;
            s.packets = QVector<QByteArray>(int((s.totalSamples + s.preSkip + 2879) / 2880), filled(1, '\x42'));
        });
        row("one spare packet", [](RawSong &s) { s.packets.push_back(filled(10, '\x42')); });
        row("stereo", [](RawSong &s) { s.channels = 2; });
        row("packet of 1275 bytes", [](RawSong &s) { s.packets[4] = filled(1275, '\x42'); });
        QTest::newRow("exactly 224 KiB") << rawSongBytes(largestSong());
    }

    void containerAcceptsEveryBound()
    {
        QFETCH(QByteArray, bytes);
        const auto decoded = decodeSongContainer(bytes);
        QVERIFY(decoded);
        // It reads what the bytes say, and writes the same bytes back.
        QCOMPARE(qFromLittleEndian<quint16>(bytes.constData() + 12), quint16(decoded->frameSamples));
        QCOMPARE(qFromLittleEndian<quint16>(bytes.constData() + 14), quint16(decoded->preSkip));
        QCOMPARE(qFromLittleEndian<quint32>(bytes.constData() + 16), quint32(decoded->totalSamples));
        QCOMPARE(encodeSongContainer(*decoded), bytes);
    }

    void containerClampsGainToAttenuation()
    {
        const auto gainOf = [](qint16 gainQ8) {
            RawSong song;
            song.gainQ8 = gainQ8;
            const auto decoded = decodeSongContainer(rawSongBytes(song));
            return decoded ? decoded->gainQ8 : 12345;
        };
        QCOMPARE(gainOf(0), 0);
        QCOMPARE(gainOf(-1), -1);
        QCOMPARE(gainOf(-3 * 256), -3 * 256);
        QCOMPARE(gainOf(-24 * 256), -24 * 256);
        QCOMPARE(gainOf(-24 * 256 - 1), -24 * 256);
        QCOMPARE(gainOf(-32768), -24 * 256);
        QCOMPARE(gainOf(1), 0);
        QCOMPARE(gainOf(500), 0);
        QCOMPARE(gainOf(32767), 0);

        // The encoder never writes a boost either.
        SongContainer loud;
        loud.totalSamples = 2880 - 312;
        loud.preSkip = 312;
        loud.packets = {filled(20, '\x01')};
        loud.gainQ8 = 6 * 256;
        const QByteArray bytes = encodeSongContainer(loud);
        QCOMPARE(qFromLittleEndian<qint16>(bytes.constData() + 20), 0);
        QCOMPARE(decodeSongContainer(bytes)->gainQ8, 0);
    }

    // --- Catalogues ----------------------------------------------------------

    void namesAndFamiliesMatchTheDesign()
    {
        // ARCH §2.1's mood table: 48 labels in id order, each with its face.
        const QStringList labels{
            u"amused"_s,   u"artistic"_s,   u"blah"_s,      u"bored"_s,      u"bouncy"_s,     u"bubbly"_s,
            u"busy"_s,     u"calm"_s,       u"cheerful"_s,  u"chill"_s,      u"confused"_s,   u"content"_s,
            u"cranky"_s,   u"creative"_s,   u"curious"_s,   u"determined"_s, u"dreamy"_s,     u"energetic"_s,
            u"excited"_s,  u"flirty"_s,     u"geeky"_s,     u"giddy"_s,      u"grateful"_s,   u"groggy"_s,
            u"happy"_s,    u"hopeful"_s,    u"hungry"_s,    u"hyper"_s,      u"inspired"_s,   u"lazy"_s,
            u"loved"_s,    u"mellow"_s,     u"melancholy"_s, u"nerdy"_s,     u"nostalgic"_s,  u"optimistic"_s,
            u"peaceful"_s, u"pensive"_s,    u"rebellious"_s, u"relaxed"_s,   u"rockin'"_s,    u"sleepy"_s,
            u"silly"_s,    u"stressed"_s,   u"thankful"_s,  u"tired"_s,      u"weird"_s,      u"working"_s};
        const QString faces = u"GSFFGGFSGSFSRSSFZGGWSGSZGSFGSZSSRSSSSFWSGZWRSZWF"_s; // S G F R(frown) Z(sleepy) W
        QCOMPARE(labels.size(), 48);
        QCOMPARE(faces.size(), 48);
        const QHash<QChar, Profile::MoodFace> faceOf{
            {u'S', Profile::MoodFace::SmileFace}, {u'G', Profile::MoodFace::GrinFace},
            {u'F', Profile::MoodFace::FlatFace},  {u'R', Profile::MoodFace::FrownFace},
            {u'Z', Profile::MoodFace::SleepyFace}, {u'W', Profile::MoodFace::WinkFace}};
        for (int id = 1; id <= 48; ++id) {
            const auto mood = static_cast<Profile::Mood>(id);
            QCOMPARE(Profile::moodName(mood), labels.at(id - 1));
            QCOMPARE(Profile::moodFace(mood), faceOf.value(faces.at(id - 1)));
        }
        QCOMPARE(Profile::moodName(Profile::Mood::MoodRockin), u"rockin'"_s);
        QVERIFY(Profile::moodName(Profile::Mood::NoMood).isEmpty());
        QVERIFY(Profile::moodName(static_cast<Profile::Mood>(49)).isEmpty());

        QCOMPARE(Profile::hereForText(0), QString());
        QCOMPARE(Profile::hereForText(Profile::HereForFriends | Profile::HereForNetworking), u"Friends, Networking"_s);
        QCOMPARE(Profile::hereForText(0x3F), u"Friends, Networking, Chatting, Gaming, Music, Collaborating"_s);
        QCOMPARE(Profile::hereForText(Profile::HereForCollaborating | Profile::HereForChatting),
                 u"Chatting, Collaborating"_s);

        QCOMPARE(Profile::zodiacName(Profile::Zodiac::NoZodiac), QString());
        QCOMPARE(Profile::zodiacName(Profile::Zodiac::Aries), u"Aries"_s);
        QCOMPARE(Profile::zodiacName(Profile::Zodiac::Pisces), u"Pisces"_s);

        const QList<std::tuple<Profile::Flourish, QString, QString>> flourishes{
            {Profile::Flourish::NoFlourish, QString(), QString()},
            {Profile::Flourish::StarFlourish, u"★ "_s, u" ★"_s},
            {Profile::Flourish::XxxFlourish, u"xXx "_s, u" xXx"_s},
            {Profile::Flourish::HeartFlourish, u"♥ "_s, u" ♥"_s},
            {Profile::Flourish::TildeFlourish, u"~* "_s, u" *~"_s},
            {Profile::Flourish::NoteFlourish, u"♫ "_s, u" ♫"_s},
            {Profile::Flourish::FlowerFlourish, u"✿ "_s, u" ✿"_s}};
        for (const auto &[flourish, prefix, suffix] : flourishes) {
            QCOMPARE(Profile::flourishPrefix(flourish), prefix);
            QCOMPARE(Profile::flourishSuffix(flourish), suffix);
        }

        const QStringList slugs{u"aero-sky"_s,     u"classic-06"_s, u"scene-queen"_s, u"neon-zebra"_s,
                                u"midnight-emo"_s, u"glitter-girl"_s, u"safety-pin"_s, u"headliner"_s,
                                u"linen"_s,        u"chrome-y2k"_s};
        for (int i = 0; i < slugs.size(); ++i)
            QCOMPARE(Profile::presetSlug(static_cast<Preset>(i)), slugs.at(i));

        QCOMPARE(Profile::radiusPixels(Profile::BoxRadius::SquareCorners), 0);
        QCOMPARE(Profile::radiusPixels(Profile::BoxRadius::SlightCorners), 3);
        QCOMPARE(Profile::radiusPixels(Profile::BoxRadius::SoftCorners), 6);
        QCOMPARE(Profile::radiusPixels(Profile::BoxRadius::RoundCorners), 10);

        const QStringList fontNames{u"Standard"_s, u"Rounded"_s, u"Script"_s, u"Typewriter"_s, u"Pixel"_s,
                                    u"Gothic"_s,   u"Future"_s,  u"Serif"_s,  u"Marker"_s};
        for (int i = 0; i < fontNames.size(); ++i) {
            const auto font = static_cast<Font>(i);
            QCOMPARE(Profile::fontName(font), fontNames.at(i));
            QVERIFY(!Profile::fontCategory(font).isEmpty());
            const bool bodySafe = font == Font::InterfaceFont || font == Font::RoundedFont
                || font == Font::TypewriterFont || font == Font::SerifFont;
            QCOMPARE(Profile::isBodySafe(font), bodySafe);
            QCOMPARE(Profile::isHeadingCapable(font), font != Font::PixelFont);
        }
        QCOMPARE(Profile::fontCategory(Font::InterfaceFont), u"sans"_s);
        QCOMPARE(Profile::fontCategory(Font::RoundedFont), u"rounded"_s);
        QCOMPARE(Profile::fontCategory(Font::ScriptFont), u"script"_s);

        QCOMPARE(Profile::moduleName(Module::HandleModule), u"OpenChat handle"_s);
        QCOMPARE(Profile::moduleName(Module::SongModule), u"Profile song"_s);
        QCOMPARE(Profile::moduleName(Module::TopFriendsModule), u"Top Friends"_s);
        for (int motif = 0; motif <= int(Profile::Motif::LinenWeave); ++motif)
            QVERIFY(!Profile::motifName(static_cast<Profile::Motif>(motif)).isEmpty());
        QCOMPARE(Profile::motifName(Profile::Motif::PolkaDots), u"Polka dots"_s);

        using M = Profile::Motif;
        const QList<std::pair<Preset, QVector<M>>> families{
            {Preset::AeroSkyPreset, {M::Bubbles, M::PolkaDots, M::Pinstripes}},
            {Preset::Classic06Preset, {M::Pinstripes, M::Checkerboard, M::PolkaDots}},
            {Preset::SceneQueenPreset, {M::Skulls, M::Stars, M::Hearts}},
            {Preset::NeonZebraPreset, {M::Zebra, M::Leopard, M::Checkerboard}},
            {Preset::MidnightEmoPreset, {M::BrokenHearts, M::Skulls, M::Pinstripes}},
            {Preset::GlitterGirlPreset, {M::Sparkles, M::Hearts, M::Flowers}},
            {Preset::SafetyPinPreset, {M::Checkerboard, M::Plaid, M::Stars}},
            {Preset::HeadlinerPreset, {M::Halftone, M::MusicNotes, M::Stars}},
            {Preset::LinenPreset, {M::LinenWeave, M::Flowers, M::Pinstripes}},
            {Preset::ChromeY2KPreset, {M::CyberGrid, M::Stars, M::Sparkles}}};
        for (const auto &[preset, family] : families)
            QCOMPARE(Profile::motifFamily(preset), family);
        QVERIFY(Profile::motifFamily(Preset::CustomPreset).isEmpty());
    }

    void enumeratorNamesAreUniqueForQml()
    {
        // QML registers Profile's enumerators unscoped as well (Profile.Stars),
        // so one name in two enums would make one of them unreachable.
        const QMetaObject &meta = Profile::staticMetaObject;
        QCOMPARE(meta.enumeratorCount(), 38);
        QSet<QByteArray> names;
        for (int i = 0; i < meta.enumeratorCount(); ++i) {
            const QMetaEnum enumerator = meta.enumerator(i);
            for (int k = 0; k < enumerator.keyCount(); ++k) {
                const QByteArray key = enumerator.key(k);
                QVERIFY2(!names.contains(key), key.constData());
                names.insert(key);
            }
        }
        QCOMPARE(QMetaEnum::fromType<Profile::Motif>().keyToValue("LinenWeave"), 16);
        QCOMPARE(QMetaEnum::fromType<Profile::Mood>().keyCount(), 49);
        QCOMPARE(QMetaEnum::fromType<Profile::Mood>().keyToValue("MoodRockin"), 41);
        QCOMPARE(QMetaEnum::fromType<Profile::Preset>().keyToValue("CustomPreset"), 255);
        QCOMPARE(QMetaEnum::fromType<Profile::EditorTab>().keyToValue("LayoutTab"), 8);
    }
};

QTEST_GUILESS_MAIN(ProfilePageTest)

#include "tst_profilepage.moc"
