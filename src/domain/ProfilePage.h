#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <utility>

namespace OpenChat {

// A MySpace-style profile page: a typed theme (colours, motifs, fonts, name
// effects), the owner's words and a module arrangement. There is no markup,
// no URL and no script anywhere in it; the renderer turns these typed knobs
// into pixels and owns readability. Colours are 0xRRGGBB so this stays a Qt
// Core type the codec, the storage and the sync layer can share.
//
// Every enum's values travel on the wire and live in the database, so they
// are append-only from the first release. Enumerator names are unique across
// the whole namespace because QML also registers them unscoped (Profile.X).
namespace Profile {
Q_NAMESPACE

enum class BackgroundKind : quint8 { SolidBackground = 0, GradientBackground = 1, PatternBackground = 2,
                                     ImageBackground = 3 };
Q_ENUM_NS(BackgroundKind)

enum class Motif : quint8 { Stars = 0, Hearts = 1, Skulls = 2, Checkerboard = 3, Zebra = 4, Leopard = 5,
                            PolkaDots = 6, Pinstripes = 7, Plaid = 8, Sparkles = 9, MusicNotes = 10,
                            Flowers = 11, Halftone = 12, CyberGrid = 13, Bubbles = 14, BrokenHearts = 15,
                            LinenWeave = 16 };
Q_ENUM_NS(Motif)

// ×0.72 / ×1.0 / ×1.4 of the motif's base tile.
enum class MotifScale : quint8 { SmallMotif = 0, MediumMotif = 1, LargeMotif = 2 };
Q_ENUM_NS(MotifScale)

enum class ImageMode : quint8 { TileImage = 0, FillImage = 1, FitImage = 2, CenterImage = 3 };
Q_ENUM_NS(ImageMode)

// DoubleBorder narrower than 3 px is kept as chosen; the renderer draws it solid.
enum class BorderStyle : quint8 { SolidBorder = 0, DashedBorder = 1, DottedBorder = 2, DoubleBorder = 3 };
Q_ENUM_NS(BorderStyle)

// 0 / 3 / 6 / 10 px (radiusPixels).
enum class BoxRadius : quint8 { SquareCorners = 0, SlightCorners = 1, SoftCorners = 2, RoundCorners = 3 };
Q_ENUM_NS(BoxRadius)

enum class HeaderStyle : quint8 { FlatHeader = 0, GradientHeader = 1, GlossHeader = 2, NoHeader = 3 };
Q_ENUM_NS(HeaderStyle)

// Bundled content faces. Body text may only use the body-safe ones; Pixel is
// for the name alone (isBodySafe / isHeadingCapable).
enum class Font : quint8 { InterfaceFont = 0, RoundedFont = 1, ScriptFont = 2, TypewriterFont = 3,
                           PixelFont = 4, GothicFont = 5, FutureFont = 6, SerifFont = 7, MarkerFont = 8 };
Q_ENUM_NS(Font)

enum class TextSize : quint8 { SmallText = 0, NormalText = 1, LargeText = 2 };
Q_ENUM_NS(TextSize)

enum class NameEffect : quint8 { PlainName = 0, GlowName = 1, OutlineName = 2, GradientName = 3,
                                 GlitterName = 4, ChromeName = 5, ShadowName = 6 };
Q_ENUM_NS(NameEffect)

// Base pixel size of the name before the face's factor: 28 / 34 / 42.
enum class NameSize : quint8 { MediumName = 0, LargeName = 1, ExtraLargeName = 2 };
Q_ENUM_NS(NameSize)

// ★ Name ★, xXx Name xXx, ♥ Name ♥, ~* Name *~, ♫ Name ♫, ✿ Name ✿ (flourishPrefix/Suffix).
enum class Flourish : quint8 { NoFlourish = 0, StarFlourish = 1, XxxFlourish = 2, HeartFlourish = 3,
                               TildeFlourish = 4, NoteFlourish = 5, FlowerFlourish = 6 };
Q_ENUM_NS(Flourish)

enum class TableStyle : quint8 { CellTable = 0, LineTable = 1 };
Q_ENUM_NS(TableStyle)

enum class Ambient : quint8 { NoAmbient = 0, FallingStars = 1, FallingHearts = 2, FallingSnow = 3,
                              FloatingSparkles = 4 };
Q_ENUM_NS(Ambient)

enum class Layout : quint8 { ClassicLayout = 0, FlippedLayout = 1, SingleLayout = 2 };
Q_ENUM_NS(Layout)

// The movable modules. The identity card, the Contacting box and the "is in
// your contacts" banner are app-owned and fixed, so they are not listed.
// CustomPanelModule places one of the owner's own panels (ModulePlacement::
// panel names which); a client from before panels drops it as unknown.
enum class Module : quint8 { HandleModule = 1, SongModule = 2, InterestsModule = 3, DetailsModule = 4,
                             BlurbsModule = 5, TopFriendsModule = 6, CustomPanelModule = 7 };
Q_ENUM_NS(Module)

enum class Column : quint8 { NarrowColumn = 0, WideColumn = 1 };
Q_ENUM_NS(Column)

enum class Zodiac : quint8 { NoZodiac = 0, Aries, Taurus, Gemini, Cancer, Leo, Virgo, Libra, Scorpio,
                             Sagittarius, Capricorn, Aquarius, Pisces };
Q_ENUM_NS(Zodiac)

// A bit set; hereForText lists the chosen ones in this order.
enum HereFor : quint8 { HereForFriends = 0x01, HereForNetworking = 0x02, HereForChatting = 0x04,
                        HereForGaming = 0x08, HereForMusic = 0x10, HereForCollaborating = 0x20 };
Q_ENUM_NS(HereFor)

// The curated mood list; moodName gives the word shown after "Mood:" and
// moodFace the painted expression.
enum class Mood : quint8 {
    NoMood = 0,
    MoodAmused, MoodArtistic, MoodBlah, MoodBored, MoodBouncy, MoodBubbly, MoodBusy, MoodCalm,
    MoodCheerful, MoodChill, MoodConfused, MoodContent, MoodCranky, MoodCreative, MoodCurious,
    MoodDetermined, MoodDreamy, MoodEnergetic, MoodExcited, MoodFlirty, MoodGeeky, MoodGiddy,
    MoodGrateful, MoodGroggy, MoodHappy, MoodHopeful, MoodHungry, MoodHyper, MoodInspired, MoodLazy,
    MoodLoved, MoodMellow, MoodMelancholy, MoodNerdy, MoodNostalgic, MoodOptimistic, MoodPeaceful,
    MoodPensive, MoodRebellious, MoodRelaxed, MoodRockin, MoodSleepy, MoodSilly, MoodStressed,
    MoodThankful, MoodTired, MoodWeird, MoodWorking
};
Q_ENUM_NS(Mood)

enum class MoodFace : quint8 { SmileFace = 0, GrinFace = 1, FlatFace = 2, FrownFace = 3, SleepyFace = 4,
                               WinkFace = 5 };
Q_ENUM_NS(MoodFace)

// Informational on the wire (the editor's "default"/"edited" tags); the theme
// itself always travels in full, so a page never depends on the preset table.
enum class Preset : quint8 { AeroSkyPreset = 0, Classic06Preset = 1, SceneQueenPreset = 2,
                             NeonZebraPreset = 3, MidnightEmoPreset = 4, GlitterGirlPreset = 5,
                             SafetyPinPreset = 6, HeadlinerPreset = 7, LinenPreset = 8,
                             ChromeY2KPreset = 9, CustomPreset = 255 };
Q_ENUM_NS(Preset)

// PanelImageMedia: a JPEG a panel shows (a picture, a list item's cover or a
// video's poster). VideoSegmentMedia: one self-contained piece of a panel
// video (domain/ClipContainer.h).
enum class MediaKind : quint8 { BackgroundImageMedia = 1, SongMedia = 2, PanelImageMedia = 3,
                                VideoSegmentMedia = 4 };
Q_ENUM_NS(MediaKind)

// Viewer-side only; never on the wire.
enum class Relationship : quint8 { SelfPerson = 0, ContactPerson = 1, IncomingRequestPerson = 2,
                                   OutgoingRequestPerson = 3, StrangerPerson = 4 };
Q_ENUM_NS(Relationship)

// Where Back returns to, and why a stub exists.
enum class Origin : quint8 { FromChat = 0, FromCall = 1, FromRequests = 2, FromSearch = 3,
                             FromSettings = 4, FromFriendSpace = 5 };
Q_ENUM_NS(Origin)

enum class PageState : quint8 { StubPage = 0, DefaultPage = 1, CustomPage = 2 };
Q_ENUM_NS(PageState)

enum class InkRole : quint8 { BodyInk = 0, LabelInk = 1, LinkInk = 2, NameInk = 3, HeaderTextInk = 4,
                              AltHeaderTextInk = 5 };
Q_ENUM_NS(InkRole)

enum class EditorTab : quint8 { ThemesTab = 0, BackgroundTab = 1, BoxesTab = 2, TextTab = 3, NameTab = 4,
                                AboutTab = 5, FriendsTab = 6, SongTab = 7, LayoutTab = 8, PanelsTab = 9 };
Q_ENUM_NS(EditorTab)

// --- Custom panels (docs/profile-panels.md)

enum class BlockKind : quint8 { TextBlock = 1, ImageBlock = 2, VideoBlock = 3, ListBlock = 4, DividerBlock = 5 };
Q_ENUM_NS(BlockKind)

enum class TextStyle : quint8 { ParagraphText = 0, HeadingText = 1, QuoteText = 2, CalloutText = 3 };
Q_ENUM_NS(TextStyle)

enum class TextAlign : quint8 { StartAlign = 0, CenterAlign = 1, EndAlign = 2 };
Q_ENUM_NS(TextAlign)

// Grid: tiles in rows. Stack: one under another at full width. Strip: one
// row the viewer scrolls sideways.
enum class GalleryStyle : quint8 { GridGallery = 0, StackGallery = 1, StripGallery = 2 };
Q_ENUM_NS(GalleryStyle)

enum class ImageFrame : quint8 { PlainFrame = 0, RoundedFrame = 1, PolaroidFrame = 2, CircleFrame = 3 };
Q_ENUM_NS(ImageFrame)

enum class ListStyle : quint8 { BulletList = 0, NumberedList = 1, GameList = 2, HeartList = 3 };
Q_ENUM_NS(ListStyle)

// Shown on a game list's items only.
enum class GameStatus : quint8 { NoGameStatus = 0, PlayingNow = 1, AllTimeFavorite = 2, Completed = 3,
                                 WantToPlay = 4, PlayingWithFriends = 5 };
Q_ENUM_NS(GameStatus)

enum class DividerStyle : quint8 { LineDivider = 0, DotsDivider = 1, StarsDivider = 2, HeartsDivider = 3,
                                   SpaceDivider = 4 };
Q_ENUM_NS(DividerStyle)

// Drawn before a panel's title (ProfileGlyph kinds, panelIconGlyph).
enum class PanelIcon : quint8 { NoPanelIcon = 0, GamepadIcon, CameraIcon, FilmIcon, MusicIcon, HeartIcon,
                                StarIcon, BookIcon, ChatIcon, SparkleIcon, TrophyIcon, PaletteIcon };
Q_ENUM_NS(PanelIcon)

// What "Add new panel" starts from.
enum class PanelTemplate : quint8 { BlankPanel = 0, TextPanel = 1, PhotoPanel = 2, GamesPanel = 3,
                                    VideoPanel = 4, TopListPanel = 5 };
Q_ENUM_NS(PanelTemplate)

// The style knobs. Defaults are the adaptive Aero Sky light look, which is
// also what a page decodes to when its theme map is absent.
struct Theme final {
    BackgroundKind backgroundKind = BackgroundKind::PatternBackground;
    quint32 backgroundColor1 = 0xC7E1F5, backgroundColor2 = 0xF1F8FD;
    Motif motif = Motif::Bubbles;
    quint32 motifInk = 0x94C6EC;
    quint8 motifOpacity = 45; // percent, 10…80
    MotifScale motifScale = MotifScale::MediumMotif;
    ImageMode imageMode = ImageMode::FillImage;
    bool imageFixed = true;
    quint32 boxFill = 0xFFFFFF;
    quint8 boxOpacity = 88; // percent, 60…100
    quint32 borderColor = 0x8DBBE0;
    quint8 borderWidth = 1; // px, 0…4
    BorderStyle borderStyle = BorderStyle::SolidBorder;
    BoxRadius boxRadius = BoxRadius::SoftCorners;
    bool boxGlow = false;
    TableStyle tableStyle = TableStyle::CellTable;
    HeaderStyle headerStyle = HeaderStyle::GlossHeader;
    quint32 headerFill = 0x9FCDEF, headerText = 0x133A61;
    bool altHeader = false; // wide-column boxes use the alt strip and border colours
    quint32 altHeaderFill = 0x9FCDEF, altHeaderText = 0x133A61, altBorderColor = 0x8DBBE0;
    Font headingFont = Font::InterfaceFont, bodyFont = Font::InterfaceFont;
    TextSize textSize = TextSize::NormalText;
    quint32 bodyColor = 0x2B3B53, labelColor = 0x1F4E79, linkColor = 0x1F6FA3;
    Font nameFont = Font::InterfaceFont;
    quint32 nameColor = 0x1C3D63;
    quint32 nameColor2 = 0xFFFFFF; // glow / outline / gradient end / sparkle / steel tint / shadow
    NameEffect nameEffect = NameEffect::GlowName;
    NameSize nameSize = NameSize::MediumName;
    Flourish nameFlourish = Flourish::NoFlourish;
    Ambient ambient = Ambient::NoAmbient;
    // Renders the Aero Sky colours for the viewer's light/dark mode instead
    // of the colour fields above (ARCH §2.5).
    bool adaptive = true;

    friend bool operator==(const Theme &, const Theme &) = default;
};

struct ModulePlacement final {
    Module module = Module::HandleModule;
    Column column = Column::NarrowColumn;
    bool visible = true;
    quint8 panel = 0; // CustomPanelModule: the Panel::id it places; 0 for every other module

    friend bool operator==(const ModulePlacement &, const ModulePlacement &) = default;
};

struct Interests final {
    QString general, music, movies, television, books, heroes;

    friend bool operator==(const Interests &, const Interests &) = default;
};

struct Details final {
    quint8 hereFor = 0; // HereFor bits
    QString hometown;
    Zodiac zodiac = Zodiac::NoZodiac;
    QString occupation, education, languages;

    friend bool operator==(const Details &, const Details &) = default;
};

// Plain text only. An empty displayName means "show the app-known name"; the
// display name is page-only and never changes the account's session name.
struct Content final {
    QString displayName, headline;
    QStringList infoLines;
    Mood mood = Mood::NoMood;
    Interests interests;
    Details details;
    QString aboutMe, meet, songTitle, songArtist;

    friend bool operator==(const Content &, const Content &) = default;
};

// A Top Friend as the page owner lists them. The name is the owner's label:
// it is shown only on monogram tiles and as a stub's provisional name, never
// under a real picture, and no handle is ever carried.
struct TopFriend final {
    QByteArray accountId; // 16 bytes
    QString name;

    friend bool operator==(const TopFriend &, const TopFriend &) = default;
};

// A media blob named by its SHA-256. width/height apply to the background,
// durationMs to the song; normalized() zeroes the fields a slot does not use.
struct MediaRef final {
    QByteArray sha256;
    quint32 bytes = 0;
    quint16 width = 0, height = 0;
    quint32 durationMs = 0;

    [[nodiscard]] bool isSet() const { return sha256.size() == 32; }
    friend bool operator==(const MediaRef &, const MediaRef &) = default;
};

// A picture in a panel: a PanelImageMedia JPEG (width/height set).
struct PanelImage final {
    MediaRef ref;
    QString caption;

    friend bool operator==(const PanelImage &, const PanelImage &) = default;
};

struct ListItem final {
    QString title, detail;
    quint8 rating = 0; // stars, 0…5; 0 shows none
    GameStatus status = GameStatus::NoGameStatus;
    MediaRef cover; // optional PanelImageMedia (a game's box art)

    friend bool operator==(const ListItem &, const ListItem &) = default;
};

// A panel video: its VideoSegmentMedia pieces in playing order (each ref's
// durationMs, width and height set) and an optional poster frame.
struct VideoClip final {
    QVector<MediaRef> segments;
    MediaRef poster; // PanelImageMedia

    [[nodiscard]] bool isSet() const { return !segments.isEmpty(); }
    [[nodiscard]] quint32 durationMs() const;
    friend bool operator==(const VideoClip &, const VideoClip &) = default;
};

// One piece of a panel. normalized() keeps only the fields of its kind and
// resets the rest, so a block never carries hidden data.
struct Block final {
    quint16 id = 0; // unique within the page, 1…65535; normalized() repairs
    BlockKind kind = BlockKind::TextBlock;
    // TextBlock
    QString text;
    TextStyle textStyle = TextStyle::ParagraphText;
    TextAlign align = TextAlign::StartAlign;
    // ImageBlock
    QVector<PanelImage> images;
    GalleryStyle gallery = GalleryStyle::GridGallery;
    ImageFrame frame = ImageFrame::RoundedFrame;
    // VideoBlock
    VideoClip video;
    bool loop = false;
    QString caption;
    // ListBlock
    ListStyle listStyle = ListStyle::BulletList;
    QVector<ListItem> items;
    // DividerBlock
    DividerStyle divider = DividerStyle::LineDivider;

    friend bool operator==(const Block &, const Block &) = default;
};

// A panel's own colours. With ownColours off the panel wears the page's box
// theme like every other box and these are ignored (but kept, so switching
// back restores them).
struct PanelLook final {
    bool ownColours = false;
    quint32 headerFill = 0x9FCDEF, headerText = 0x133A61, boxFill = 0xFFFFFF, bodyInk = 0x2B3B53;
    bool showTitle = true;

    friend bool operator==(const PanelLook &, const PanelLook &) = default;
};

struct Panel final {
    quint8 id = 0; // unique within the page, 1…255; normalized() repairs
    QString title;
    PanelIcon icon = PanelIcon::NoPanelIcon;
    PanelLook look;
    QVector<Block> blocks;

    friend bool operator==(const Panel &, const Panel &) = default;
};

// A media blob a page names, with the kind it travels as.
struct NamedMedia final {
    MediaKind kind = MediaKind::BackgroundImageMedia;
    MediaRef ref;

    friend bool operator==(const NamedMedia &, const NamedMedia &) = default;
};

struct Page final {
    qint64 revision = 0; // 0 = the default page, never published
    qint64 publishedAtMs = 0;
    Preset preset = Preset::AeroSkyPreset;
    Theme theme;
    Layout layout = Layout::ClassicLayout;
    QVector<ModulePlacement> modules; // normalized() fills in the defaults
    Content content;
    QVector<TopFriend> topFriends;
    MediaRef background;
    MediaRef song;
    QVector<Panel> panels; // docs/profile-panels.md; placed by CustomPanelModule entries

    friend bool operator==(const Page &, const Page &) = default;
};

inline constexpr int maxTopFriends = 8;
// The largest revision a core carries: JavaScript-safe, and far beyond any
// millisecond clock nextRevision() will meet.
inline constexpr qint64 maxRevision = (qint64(1) << 53) - 1;
inline constexpr int maxMood = static_cast<int>(Mood::MoodWorking);
inline constexpr quint8 hereForMask = 0x3F;
inline constexpr int maxBackgroundDimension = 1920;
inline constexpr quint32 maxSongRefDurationMs = 45'500;

// Bounds in UTF-16 code units, applied after sanitising.
struct TextBounds {
    static constexpr int displayName = 48, headline = 80, infoLine = 40, infoLines = 3, interest = 300,
                         detail = 60, aboutMe = 2000, meet = 1000, songTitle = 60, songArtist = 60,
                         friendName = 48, panelTitle = 60, blockText = 2000, caption = 120,
                         itemTitle = 80, itemDetail = 80;
};

// Panel bounds (docs/profile-panels.md). maxPanels keeps the six built-in
// modules plus every panel within the sixteen layout entries a 0.2.9 client
// reads. The budgets apply across the whole page in document order.
struct PanelBounds {
    static constexpr int maxPanels = 10, maxBlocksPerPanel = 12, maxBlocks = 40, maxImagesPerBlock = 6,
                         maxItemsPerList = 20, maxItems = 120, maxSegments = 6, maxRating = 5,
                         textBudget = 16'000, maxMedia = 24, maxImageDimension = 1280,
                         maxVideoDimension = 640;
    static constexpr quint32 maxSegmentDurationMs = 8'000;
    static constexpr qint64 maxMediaBytes = 4LL * 1024 * 1024;
};

[[nodiscard]] Theme defaultTheme();                 // == Theme{}: Aero Sky light values, adaptive
[[nodiscard]] Theme aeroSkyTheme(bool dark);        // the concrete Aero Sky palette for one mode, not adaptive
[[nodiscard]] Theme presetTheme(Preset preset);     // SPEC §10; CustomPreset gives defaultTheme()
// Replaces every style knob with the preset's, sets page.preset, then the
// preset's layout effect: Safety Pin → Flipped, every other → Classic, and
// Headliner moves the song to the top of the wide column. Words, media, Top
// Friends and module visibility never change. CustomPreset is a no-op.
[[nodiscard]] Page applyPreset(Page page, Preset preset);
[[nodiscard]] bool styleDiffersFromPreset(const Page &page); // the editor's "edited" tag
[[nodiscard]] QVector<Motif> motifFamily(Preset preset);      // "Surprise me"
[[nodiscard]] QVector<ModulePlacement> defaultModules();
[[nodiscard]] Page defaultPage(); // normalized(Page{})
// Clamps, resets unknown enums, repairs the module arrangement, dedupes and
// caps Top Friends, drops malformed media refs and fully sanitises every text.
// Idempotent: normalized(normalized(p)) == normalized(p).
[[nodiscard]] Page normalized(Page page);
// Compares normalised forms and ignores revision and publishedAtMs, so a
// trailing space typed into the draft does not make it "dirty".
[[nodiscard]] bool samePublishedContent(const Page &a, const Page &b);
// max(previous + 1, nowMs): a reinstall with a fresh clock still produces a
// revision above anything a contact stored.
[[nodiscard]] qint64 nextRevision(qint64 previous, qint64 nowMs);
[[nodiscard]] int radiusPixels(BoxRadius radius);
[[nodiscard]] bool isBodySafe(Font font);
[[nodiscard]] bool isHeadingCapable(Font font);
[[nodiscard]] QString presetName(Preset preset);  // "Aero Sky", "Classic '06", …
[[nodiscard]] QString presetSlug(Preset preset);  // "aero-sky", "classic-06", …
[[nodiscard]] QString motifName(Motif motif);     // "Polka dots", …
[[nodiscard]] QString fontName(Font font);        // "Standard", "Rounded", …
[[nodiscard]] QString fontCategory(Font font);    // "sans", "rounded", "script", …
[[nodiscard]] QString zodiacName(Zodiac zodiac);  // "" for NoZodiac
[[nodiscard]] QString moodName(Mood mood);        // "" for NoMood
[[nodiscard]] MoodFace moodFace(Mood mood);
[[nodiscard]] QString hereForText(quint8 bits);   // "Friends, Networking"
[[nodiscard]] QString moduleName(Module module);  // "OpenChat handle", "Profile song", …
// Every media ref the page names: the background, the song, then each
// panel's in document order (pictures, covers, posters, segments). A blob
// named twice is listed once, as the kind it is first named as.
[[nodiscard]] QVector<NamedMedia> mediaRefs(const Page &page);
// The panel "Add new panel" makes from a template: its id and block ids free
// in `page`, the template's title, icon and starter blocks, no media.
[[nodiscard]] Panel panelFromTemplate(const Page &page, PanelTemplate panelTemplate);
// An empty block of `kind` with an id free in `page`.
[[nodiscard]] Block newBlock(const Page &page, BlockKind kind);
[[nodiscard]] quint8 freePanelId(const Page &page);   // 0 when all 255 are taken
[[nodiscard]] quint16 freeBlockId(const Page &page);  // 0 when all are taken
[[nodiscard]] qsizetype panelIndex(const Page &page, quint8 id); // -1 when absent
// {panel index, block index} of the block with this id; {-1, -1} when absent.
[[nodiscard]] std::pair<qsizetype, qsizetype> blockIndex(const Page &page, quint16 id);
[[nodiscard]] QString panelTemplateName(PanelTemplate panelTemplate); // "Blank", "Favorite games", …
[[nodiscard]] QString panelIconGlyph(PanelIcon icon);                 // a ProfileGlyph kind, "" for none
[[nodiscard]] QString gameStatusName(GameStatus status);              // "Playing now", …; "" for none
[[nodiscard]] QString blockKindName(BlockKind kind);                  // "Text", "Pictures", …
[[nodiscard]] QString flourishPrefix(Flourish flourish); // "★ "
[[nodiscard]] QString flourishSuffix(Flourish flourish); // " ★"

// Full sanitising (ARCH §2.4): line breaks, controls, bidi and default-
// ignorable characters, the combining-mark cap, whitespace collapse and trim,
// then truncation to maxLength UTF-16 units without splitting a character or
// a base+mark cluster. Idempotent.
[[nodiscard]] QString sanitizeLine(const QString &text, qsizetype maxLength);
// As sanitizeLine, but keeps line breaks: at most one blank line in a row,
// trailing whitespace trimmed per line.
[[nodiscard]] QString sanitizeParagraphs(const QString &text, qsizetype maxLength);
// Light sanitising for setters while the user types: line breaks, controls,
// bidi and default-ignorables, and truncation. It never trims or collapses
// whitespace, so typing "a " or two newlines is not undone mid-word.
[[nodiscard]] QString sanitizeLive(const QString &text, qsizetype maxLength, bool multiLine);

} // namespace Profile
} // namespace OpenChat
