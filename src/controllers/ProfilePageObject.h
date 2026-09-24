#pragma once

#include "domain/ProfilePage.h"

#include <QColor>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <functional>

namespace OpenChat {

// A page's resolved look for one viewer (ARCH §7.4): the owner's theme after
// Plain style and the adaptive Aero Sky substitution, run through the
// renderer's readability pass (ProfileReadability::resolve) and turned into
// fonts and pixel sizes. Everything QML paints a page with comes from here, so
// no QML file ever mixes, measures or corrects a colour itself. Read-only;
// recomputed only when the page's style or media, or the viewer, changes.
class ProfileRenderStyle final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool plain READ plain NOTIFY changed)
    // Backdrop
    Q_PROPERTY(int backgroundKind READ backgroundKind NOTIFY changed)
    Q_PROPERTY(QColor color1 READ color1 NOTIFY changed)
    Q_PROPERTY(QColor color2 READ color2 NOTIFY changed)
    Q_PROPERTY(int motif READ motif NOTIFY changed)
    Q_PROPERTY(QColor motifInk READ motifInk NOTIFY changed)
    Q_PROPERTY(qreal motifOpacity READ motifOpacity NOTIFY changed) // 0…1
    Q_PROPERTY(int motifScale READ motifScale NOTIFY changed)
    // "" unless the page shows its picture (not in Plain style) and it is registered.
    Q_PROPERTY(QString imageKey READ imageKey NOTIFY changed)
    Q_PROPERTY(int imageMode READ imageMode NOTIFY changed)
    Q_PROPERTY(bool imageFixed READ imageFixed NOTIFY changed)
    // Boxes
    Q_PROPERTY(QColor boxFill READ boxFill NOTIFY changed) // alpha = the resolved opacity
    Q_PROPERTY(bool boxDark READ boxDark NOTIFY changed)
    Q_PROPERTY(QColor borderColor READ borderColor NOTIFY changed)
    // The wide-column border: the alt border with altHeader on, else the border.
    Q_PROPERTY(QColor altBorderColor READ altBorderColor NOTIFY changed)
    Q_PROPERTY(int borderWidth READ borderWidth NOTIFY changed)
    Q_PROPERTY(int borderStyle READ borderStyle NOTIFY changed) // Double narrower than 3 px → Solid
    Q_PROPERTY(int radiusPx READ radiusPx NOTIFY changed)
    Q_PROPERTY(bool boxGlow READ boxGlow NOTIFY changed)
    Q_PROPERTY(int tableStyle READ tableStyle NOTIFY changed)
    // Header strips (SPEC §4.2); the alt family equals the main one with altHeader off.
    Q_PROPERTY(int headerStyle READ headerStyle NOTIFY changed)
    Q_PROPERTY(QVariantList stripStops READ stripStops NOTIFY changed)         // 4 colours
    Q_PROPERTY(QVariantList stripPositions READ stripPositions NOTIFY changed) // 4 reals
    Q_PROPERTY(QColor stripHighlight READ stripHighlight NOTIFY changed)
    Q_PROPERTY(QColor stripBottomLine READ stripBottomLine NOTIFY changed)
    Q_PROPERTY(bool stripDark READ stripDark NOTIFY changed)
    Q_PROPERTY(QVariantList altStripStops READ altStripStops NOTIFY changed)
    Q_PROPERTY(QVariantList altStripPositions READ altStripPositions NOTIFY changed)
    Q_PROPERTY(QColor altStripHighlight READ altStripHighlight NOTIFY changed)
    Q_PROPERTY(QColor altStripBottomLine READ altStripBottomLine NOTIFY changed)
    Q_PROPERTY(bool altStripDark READ altStripDark NOTIFY changed)
    Q_PROPERTY(bool altHeader READ altHeader NOTIFY changed)
    Q_PROPERTY(QColor headerText READ headerText NOTIFY changed)
    Q_PROPERTY(QColor altHeaderText READ altHeaderText NOTIFY changed)
    // Type (SPEC §4.3, §11): pixel sizes already carry the face factors.
    Q_PROPERTY(int stripHeight READ stripHeight NOTIFY changed)
    Q_PROPERTY(int titlePixelSize READ titlePixelSize NOTIFY changed)
    Q_PROPERTY(QString headingFamily READ headingFamily NOTIFY changed) // "" = the interface font
    Q_PROPERTY(bool headingBold READ headingBold NOTIFY changed)
    Q_PROPERTY(int headingLift READ headingLift NOTIFY changed) // Pacifico sits 1 px higher
    Q_PROPERTY(qreal headingFactor READ headingFactor NOTIFY changed) // for sizes QML derives (the banner)
    Q_PROPERTY(QString bodyFamily READ bodyFamily NOTIFY changed)
    Q_PROPERTY(int bodyPixelSize READ bodyPixelSize NOTIFY changed)
    Q_PROPERTY(QString labelFamily READ labelFamily NOTIFY changed)
    Q_PROPERTY(bool labelBold READ labelBold NOTIFY changed)
    Q_PROPERTY(int labelPixelSize READ labelPixelSize NOTIFY changed)
    Q_PROPERTY(int subheadPixelSize READ subheadPixelSize NOTIFY changed)
    Q_PROPERTY(int captionPixelSize READ captionPixelSize NOTIFY changed)
    // The styled name (SPEC §7)
    Q_PROPERTY(QString nameFamily READ nameFamily NOTIFY changed)
    Q_PROPERTY(int nameBasePixelSize READ nameBasePixelSize NOTIFY changed)
    Q_PROPERTY(int nameMinPixelSize READ nameMinPixelSize NOTIFY changed)
    Q_PROPERTY(int nameEffect READ nameEffect NOTIFY changed)
    Q_PROPERTY(int nameFlourish READ nameFlourish NOTIFY changed)
    Q_PROPERTY(QColor nameColor READ nameColor NOTIFY changed)
    Q_PROPERTY(QColor nameColor2 READ nameColor2 NOTIFY changed)
    // Inks, held at their floors against every box background
    Q_PROPERTY(QColor bodyColor READ bodyColor NOTIFY changed)
    Q_PROPERTY(QColor labelColor READ labelColor NOTIFY changed)
    Q_PROPERTY(QColor linkColor READ linkColor NOTIFY changed)
    Q_PROPERTY(QColor mutedColor READ mutedColor NOTIFY changed)
    Q_PROPERTY(QColor blurbSubheadColor READ blurbSubheadColor NOTIFY changed)
    Q_PROPERTY(QColor cellLabelFill READ cellLabelFill NOTIFY changed)
    Q_PROPERTY(QColor cellValueFill READ cellValueFill NOTIFY changed)
    Q_PROPERTY(QColor cellLabelInk READ cellLabelInk NOTIFY changed)
    Q_PROPERTY(QColor cellValueInk READ cellValueInk NOTIFY changed)
    Q_PROPERTY(QColor tableRuleColor READ tableRuleColor NOTIFY changed)
    // Indexed by the Presence value: Available 0, Away 1, Offline 2, Busy 3.
    Q_PROPERTY(QVariantList presenceInks READ presenceInks NOTIFY changed)
    Q_PROPERTY(QColor monogramTop READ monogramTop NOTIFY changed)
    Q_PROPERTY(QColor monogramBottom READ monogramBottom NOTIFY changed)
    Q_PROPERTY(QColor monogramRim READ monogramRim NOTIFY changed)
    Q_PROPERTY(QColor monogramInk READ monogramInk NOTIFY changed)
    Q_PROPERTY(QColor altMonogramTop READ altMonogramTop NOTIFY changed)
    Q_PROPERTY(QColor altMonogramBottom READ altMonogramBottom NOTIFY changed)
    Q_PROPERTY(QColor altMonogramRim READ altMonogramRim NOTIFY changed)
    Q_PROPERTY(QColor altMonogramInk READ altMonogramInk NOTIFY changed)
    Q_PROPERTY(int songMaterial READ songMaterial NOTIFY changed) // 0 silver glass, 1 smoked glass
    // The last-resort halo, for body text and strip titles alike.
    Q_PROPERTY(bool textHalo READ textHalo NOTIFY changed)
    Q_PROPERTY(QColor haloColor READ haloColor NOTIFY changed)
    Q_PROPERTY(int ambient READ ambient NOTIFY changed)
    Q_PROPERTY(QColor ambientOutline READ ambientOutline NOTIFY changed)
    // What the renderer corrected, for the editor's notices and badges.
    Q_PROPERTY(bool adjusted READ adjusted NOTIFY changed)
    Q_PROPERTY(QVariantList adjustments READ adjustments NOTIFY changed) // [{tab, role, sentence}]
    Q_PROPERTY(QVariantMap inkAdjusted READ inkAdjusted NOTIFY changed)  // {"<InkRole>": bool}

public:
    struct Values final {
        bool plain = false;
        int backgroundKind = 0;
        QColor color1, color2;
        int motif = 0;
        QColor motifInk;
        qreal motifOpacity = 0;
        int motifScale = 0;
        QString imageKey;
        int imageMode = 0;
        bool imageFixed = true;
        QColor boxFill;
        bool boxDark = false;
        QColor borderColor, altBorderColor;
        int borderWidth = 0, borderStyle = 0, radiusPx = 0;
        bool boxGlow = false;
        int tableStyle = 0;
        int headerStyle = 0;
        QVariantList stripStops, stripPositions;
        QColor stripHighlight, stripBottomLine;
        bool stripDark = false;
        QVariantList altStripStops, altStripPositions;
        QColor altStripHighlight, altStripBottomLine;
        bool altStripDark = false;
        bool altHeader = false;
        QColor headerText, altHeaderText;
        int stripHeight = 30, titlePixelSize = 14;
        QString headingFamily;
        bool headingBold = false;
        int headingLift = 0;
        qreal headingFactor = 1.0;
        QString bodyFamily;
        int bodyPixelSize = 13;
        QString labelFamily;
        bool labelBold = false;
        int labelPixelSize = 13, subheadPixelSize = 14, captionPixelSize = 11;
        QString nameFamily;
        int nameBasePixelSize = 28, nameMinPixelSize = 20, nameEffect = 0, nameFlourish = 0;
        QColor nameColor, nameColor2;
        QColor bodyColor, labelColor, linkColor, mutedColor, blurbSubheadColor;
        QColor cellLabelFill, cellValueFill, cellLabelInk, cellValueInk, tableRuleColor;
        QVariantList presenceInks;
        QColor monogramTop, monogramBottom, monogramRim, monogramInk;
        QColor altMonogramTop, altMonogramBottom, altMonogramRim, altMonogramInk;
        int songMaterial = 0;
        bool textHalo = false;
        QColor haloColor;
        int ambient = 0;
        QColor ambientOutline;
        bool adjusted = false;
        QVariantList adjustments;
        QVariantMap inkAdjusted;

        friend bool operator==(const Values &, const Values &) = default;
    };

    // Who looks at the page and how.
    struct Viewer final {
        bool dark = false;  // the viewer's app mode (adaptive Aero Sky follows it)
        bool plain = false; // render the full Aero Sky theme (Plain style, stubs)
    };

    explicit ProfileRenderStyle(QObject *parent = nullptr);

    // The resolved style of `theme` for `viewer`. `imageKey` is the page's
    // registered background picture ("" when none is shown); its statistics,
    // once decoded, join the readability samples.
    [[nodiscard]] static Values compute(const Profile::Theme &theme, const Viewer &viewer, const QString &imageKey);
    // The theme a viewer sees before readability: Plain style's full Aero Sky,
    // or an adaptive theme's Aero Sky colours for the viewer's mode.
    [[nodiscard]] static Profile::Theme viewedTheme(const Profile::Theme &theme, const Viewer &viewer);
    // An adaptive theme's knobs with Aero Sky's colours and box opacity for
    // `dark` in place of its own (ARCH §2.5); everything else is kept.
    [[nodiscard]] static Profile::Theme withAeroSkyColours(Profile::Theme theme, bool dark);

    [[nodiscard]] const Values &values() const noexcept { return m_values; }
    // Emits changed() only when a value differs.
    void update(const Values &values);

    [[nodiscard]] bool plain() const { return m_values.plain; }
    [[nodiscard]] int backgroundKind() const { return m_values.backgroundKind; }
    [[nodiscard]] QColor color1() const { return m_values.color1; }
    [[nodiscard]] QColor color2() const { return m_values.color2; }
    [[nodiscard]] int motif() const { return m_values.motif; }
    [[nodiscard]] QColor motifInk() const { return m_values.motifInk; }
    [[nodiscard]] qreal motifOpacity() const { return m_values.motifOpacity; }
    [[nodiscard]] int motifScale() const { return m_values.motifScale; }
    [[nodiscard]] QString imageKey() const { return m_values.imageKey; }
    [[nodiscard]] int imageMode() const { return m_values.imageMode; }
    [[nodiscard]] bool imageFixed() const { return m_values.imageFixed; }
    [[nodiscard]] QColor boxFill() const { return m_values.boxFill; }
    [[nodiscard]] bool boxDark() const { return m_values.boxDark; }
    [[nodiscard]] QColor borderColor() const { return m_values.borderColor; }
    [[nodiscard]] QColor altBorderColor() const { return m_values.altBorderColor; }
    [[nodiscard]] int borderWidth() const { return m_values.borderWidth; }
    [[nodiscard]] int borderStyle() const { return m_values.borderStyle; }
    [[nodiscard]] int radiusPx() const { return m_values.radiusPx; }
    [[nodiscard]] bool boxGlow() const { return m_values.boxGlow; }
    [[nodiscard]] int tableStyle() const { return m_values.tableStyle; }
    [[nodiscard]] int headerStyle() const { return m_values.headerStyle; }
    [[nodiscard]] QVariantList stripStops() const { return m_values.stripStops; }
    [[nodiscard]] QVariantList stripPositions() const { return m_values.stripPositions; }
    [[nodiscard]] QColor stripHighlight() const { return m_values.stripHighlight; }
    [[nodiscard]] QColor stripBottomLine() const { return m_values.stripBottomLine; }
    [[nodiscard]] bool stripDark() const { return m_values.stripDark; }
    [[nodiscard]] QVariantList altStripStops() const { return m_values.altStripStops; }
    [[nodiscard]] QVariantList altStripPositions() const { return m_values.altStripPositions; }
    [[nodiscard]] QColor altStripHighlight() const { return m_values.altStripHighlight; }
    [[nodiscard]] QColor altStripBottomLine() const { return m_values.altStripBottomLine; }
    [[nodiscard]] bool altStripDark() const { return m_values.altStripDark; }
    [[nodiscard]] bool altHeader() const { return m_values.altHeader; }
    [[nodiscard]] QColor headerText() const { return m_values.headerText; }
    [[nodiscard]] QColor altHeaderText() const { return m_values.altHeaderText; }
    [[nodiscard]] int stripHeight() const { return m_values.stripHeight; }
    [[nodiscard]] int titlePixelSize() const { return m_values.titlePixelSize; }
    [[nodiscard]] QString headingFamily() const { return m_values.headingFamily; }
    [[nodiscard]] bool headingBold() const { return m_values.headingBold; }
    [[nodiscard]] int headingLift() const { return m_values.headingLift; }
    [[nodiscard]] qreal headingFactor() const { return m_values.headingFactor; }
    [[nodiscard]] QString bodyFamily() const { return m_values.bodyFamily; }
    [[nodiscard]] int bodyPixelSize() const { return m_values.bodyPixelSize; }
    [[nodiscard]] QString labelFamily() const { return m_values.labelFamily; }
    [[nodiscard]] bool labelBold() const { return m_values.labelBold; }
    [[nodiscard]] int labelPixelSize() const { return m_values.labelPixelSize; }
    [[nodiscard]] int subheadPixelSize() const { return m_values.subheadPixelSize; }
    [[nodiscard]] int captionPixelSize() const { return m_values.captionPixelSize; }
    [[nodiscard]] QString nameFamily() const { return m_values.nameFamily; }
    [[nodiscard]] int nameBasePixelSize() const { return m_values.nameBasePixelSize; }
    [[nodiscard]] int nameMinPixelSize() const { return m_values.nameMinPixelSize; }
    [[nodiscard]] int nameEffect() const { return m_values.nameEffect; }
    [[nodiscard]] int nameFlourish() const { return m_values.nameFlourish; }
    [[nodiscard]] QColor nameColor() const { return m_values.nameColor; }
    [[nodiscard]] QColor nameColor2() const { return m_values.nameColor2; }
    [[nodiscard]] QColor bodyColor() const { return m_values.bodyColor; }
    [[nodiscard]] QColor labelColor() const { return m_values.labelColor; }
    [[nodiscard]] QColor linkColor() const { return m_values.linkColor; }
    [[nodiscard]] QColor mutedColor() const { return m_values.mutedColor; }
    [[nodiscard]] QColor blurbSubheadColor() const { return m_values.blurbSubheadColor; }
    [[nodiscard]] QColor cellLabelFill() const { return m_values.cellLabelFill; }
    [[nodiscard]] QColor cellValueFill() const { return m_values.cellValueFill; }
    [[nodiscard]] QColor cellLabelInk() const { return m_values.cellLabelInk; }
    [[nodiscard]] QColor cellValueInk() const { return m_values.cellValueInk; }
    [[nodiscard]] QColor tableRuleColor() const { return m_values.tableRuleColor; }
    [[nodiscard]] QVariantList presenceInks() const { return m_values.presenceInks; }
    [[nodiscard]] QColor monogramTop() const { return m_values.monogramTop; }
    [[nodiscard]] QColor monogramBottom() const { return m_values.monogramBottom; }
    [[nodiscard]] QColor monogramRim() const { return m_values.monogramRim; }
    [[nodiscard]] QColor monogramInk() const { return m_values.monogramInk; }
    [[nodiscard]] QColor altMonogramTop() const { return m_values.altMonogramTop; }
    [[nodiscard]] QColor altMonogramBottom() const { return m_values.altMonogramBottom; }
    [[nodiscard]] QColor altMonogramRim() const { return m_values.altMonogramRim; }
    [[nodiscard]] QColor altMonogramInk() const { return m_values.altMonogramInk; }
    [[nodiscard]] int songMaterial() const { return m_values.songMaterial; }
    [[nodiscard]] bool textHalo() const { return m_values.textHalo; }
    [[nodiscard]] QColor haloColor() const { return m_values.haloColor; }
    [[nodiscard]] int ambient() const { return m_values.ambient; }
    [[nodiscard]] QColor ambientOutline() const { return m_values.ambientOutline; }
    [[nodiscard]] bool adjusted() const { return m_values.adjusted; }
    [[nodiscard]] QVariantList adjustments() const { return m_values.adjustments; }
    [[nodiscard]] QVariantMap inkAdjusted() const { return m_values.inkAdjusted; }

signals:
    void changed();

private:
    Values m_values;
};

// One profile page as QML reads and edits it (ARCH §7.3): flat typed
// properties in four NOTIFY groups, so a keystroke in About me never
// re-resolves colours and a style change never rebuilds the text.
//
//   styleChanged    theme and layout knobs (and `render` is recomputed)
//   contentChanged  words, mood and details
//   listsChanged    Top Friends, the module arrangement and the derived rows
//   mediaChanged    the background and song refs and whether they are here
//
// A group is emitted only when one of its values really changed. Writes on a
// read-only instance (the view, the try-on) are ignored; a write on the draft
// applies live sanitising (Profile::sanitizeLive), clamps numbers, refuses
// values the model forbids (a Pixel heading, a body face that is not
// body-safe), and emits edited(), which drives autosave and undo. The
// controller's own loads never emit edited().
//
// On an adaptive (Aero Sky) page the colour getters report what the viewer
// sees for their light/dark mode, and the first write to one of those
// colours first copies that palette into the page and stops following the
// mode, so the preview never jumps (ARCH §2.5).
class ProfilePageObject final : public QObject
{
    Q_OBJECT
    // --- meta [style]
    Q_PROPERTY(bool readOnly READ readOnly CONSTANT)
    Q_PROPERTY(bool showEmptyModules READ showEmptyModules CONSTANT)
    Q_PROPERTY(qint64 revision READ revision NOTIFY styleChanged)
    Q_PROPERTY(int preset READ preset NOTIFY styleChanged)
    Q_PROPERTY(bool adaptive READ adaptive WRITE setAdaptive NOTIFY styleChanged)
    Q_PROPERTY(int layout READ layout WRITE setLayout NOTIFY styleChanged)
    Q_PROPERTY(bool styleEditedSincePreset READ styleEditedSincePreset NOTIFY styleChanged)
    // --- page [style]
    Q_PROPERTY(int backgroundKind READ backgroundKind WRITE setBackgroundKind NOTIFY styleChanged)
    Q_PROPERTY(QColor backgroundColor1 READ backgroundColor1 WRITE setBackgroundColor1 NOTIFY styleChanged)
    Q_PROPERTY(QColor backgroundColor2 READ backgroundColor2 WRITE setBackgroundColor2 NOTIFY styleChanged)
    Q_PROPERTY(int motif READ motif WRITE setMotif NOTIFY styleChanged)
    Q_PROPERTY(QColor motifInk READ motifInk WRITE setMotifInk NOTIFY styleChanged)
    Q_PROPERTY(int motifOpacity READ motifOpacity WRITE setMotifOpacity NOTIFY styleChanged) // 10…80
    Q_PROPERTY(int motifScale READ motifScale WRITE setMotifScale NOTIFY styleChanged)
    Q_PROPERTY(int imageMode READ imageMode WRITE setImageMode NOTIFY styleChanged)
    Q_PROPERTY(bool imageFixed READ imageFixed WRITE setImageFixed NOTIFY styleChanged)
    // --- boxes [style]
    Q_PROPERTY(QColor boxFill READ boxFill WRITE setBoxFill NOTIFY styleChanged)
    Q_PROPERTY(int boxOpacity READ boxOpacity WRITE setBoxOpacity NOTIFY styleChanged) // 60…100
    Q_PROPERTY(QColor borderColor READ borderColor WRITE setBorderColor NOTIFY styleChanged)
    Q_PROPERTY(int borderWidth READ borderWidth WRITE setBorderWidth NOTIFY styleChanged) // 0…4
    Q_PROPERTY(int borderStyle READ borderStyle WRITE setBorderStyle NOTIFY styleChanged)
    Q_PROPERTY(int boxRadius READ boxRadius WRITE setBoxRadius NOTIFY styleChanged) // Profile.BoxRadius
    Q_PROPERTY(bool boxGlow READ boxGlow WRITE setBoxGlow NOTIFY styleChanged)
    Q_PROPERTY(int tableStyle READ tableStyle WRITE setTableStyle NOTIFY styleChanged)
    Q_PROPERTY(QColor altBorderColor READ altBorderColor WRITE setAltBorderColor NOTIFY styleChanged)
    // --- strips [style]
    Q_PROPERTY(int headerStyle READ headerStyle WRITE setHeaderStyle NOTIFY styleChanged)
    Q_PROPERTY(QColor headerFill READ headerFill WRITE setHeaderFill NOTIFY styleChanged)
    Q_PROPERTY(QColor headerText READ headerText WRITE setHeaderText NOTIFY styleChanged)
    Q_PROPERTY(bool altHeader READ altHeader WRITE setAltHeader NOTIFY styleChanged)
    Q_PROPERTY(QColor altHeaderFill READ altHeaderFill WRITE setAltHeaderFill NOTIFY styleChanged)
    Q_PROPERTY(QColor altHeaderText READ altHeaderText WRITE setAltHeaderText NOTIFY styleChanged)
    // --- text [style]
    Q_PROPERTY(int headingFont READ headingFont WRITE setHeadingFont NOTIFY styleChanged) // heading-capable
    Q_PROPERTY(int bodyFont READ bodyFont WRITE setBodyFont NOTIFY styleChanged)          // body-safe
    Q_PROPERTY(int textSize READ textSize WRITE setTextSize NOTIFY styleChanged)
    Q_PROPERTY(QColor bodyColor READ bodyColor WRITE setBodyColor NOTIFY styleChanged)
    Q_PROPERTY(QColor labelColor READ labelColor WRITE setLabelColor NOTIFY styleChanged)
    Q_PROPERTY(QColor linkColor READ linkColor WRITE setLinkColor NOTIFY styleChanged)
    // --- name [style]
    Q_PROPERTY(int nameFont READ nameFont WRITE setNameFont NOTIFY styleChanged)
    Q_PROPERTY(QColor nameColor READ nameColor WRITE setNameColor NOTIFY styleChanged)
    Q_PROPERTY(QColor nameColor2 READ nameColor2 WRITE setNameColor2 NOTIFY styleChanged)
    Q_PROPERTY(int nameEffect READ nameEffect WRITE setNameEffect NOTIFY styleChanged)
    Q_PROPERTY(int nameSize READ nameSize WRITE setNameSize NOTIFY styleChanged)
    Q_PROPERTY(int nameFlourish READ nameFlourish WRITE setNameFlourish NOTIFY styleChanged)
    Q_PROPERTY(int ambient READ ambient WRITE setAmbient NOTIFY styleChanged)
    // --- content [content] (plain text; bounds of Profile::TextBounds)
    Q_PROPERTY(QString displayName READ displayName WRITE setDisplayName NOTIFY contentChanged)
    Q_PROPERTY(QString headline READ headline WRITE setHeadline NOTIFY contentChanged)
    Q_PROPERTY(QString infoLine1 READ infoLine1 WRITE setInfoLine1 NOTIFY contentChanged)
    Q_PROPERTY(QString infoLine2 READ infoLine2 WRITE setInfoLine2 NOTIFY contentChanged)
    Q_PROPERTY(QString infoLine3 READ infoLine3 WRITE setInfoLine3 NOTIFY contentChanged)
    Q_PROPERTY(int mood READ mood WRITE setMood NOTIFY contentChanged)
    Q_PROPERTY(QString interestGeneral READ interestGeneral WRITE setInterestGeneral NOTIFY contentChanged)
    Q_PROPERTY(QString interestMusic READ interestMusic WRITE setInterestMusic NOTIFY contentChanged)
    Q_PROPERTY(QString interestMovies READ interestMovies WRITE setInterestMovies NOTIFY contentChanged)
    Q_PROPERTY(QString interestTelevision READ interestTelevision WRITE setInterestTelevision NOTIFY contentChanged)
    Q_PROPERTY(QString interestBooks READ interestBooks WRITE setInterestBooks NOTIFY contentChanged)
    Q_PROPERTY(QString interestHeroes READ interestHeroes WRITE setInterestHeroes NOTIFY contentChanged)
    Q_PROPERTY(int hereFor READ hereFor WRITE setHereFor NOTIFY contentChanged) // Profile.HereFor bits
    Q_PROPERTY(QString hometown READ hometown WRITE setHometown NOTIFY contentChanged)
    Q_PROPERTY(int zodiac READ zodiac WRITE setZodiac NOTIFY contentChanged)
    Q_PROPERTY(QString occupation READ occupation WRITE setOccupation NOTIFY contentChanged)
    Q_PROPERTY(QString education READ education WRITE setEducation NOTIFY contentChanged)
    Q_PROPERTY(QString languages READ languages WRITE setLanguages NOTIFY contentChanged)
    Q_PROPERTY(QString aboutMe READ aboutMe WRITE setAboutMe NOTIFY contentChanged)
    Q_PROPERTY(QString meet READ meet WRITE setMeet NOTIFY contentChanged)
    Q_PROPERTY(QString songTitle READ songTitle WRITE setSongTitle NOTIFY contentChanged)
    Q_PROPERTY(QString songArtist READ songArtist WRITE setSongArtist NOTIFY contentChanged)
    // --- media [media] (read-only; the controller imports and removes)
    Q_PROPERTY(bool hasBackgroundImage READ hasBackgroundImage NOTIFY mediaChanged)
    Q_PROPERTY(QString backgroundImageKey READ backgroundImageKey NOTIFY mediaChanged)
    Q_PROPERTY(bool backgroundPending READ backgroundPending NOTIFY mediaChanged)
    Q_PROPERTY(bool hasSong READ hasSong NOTIFY mediaChanged)
    Q_PROPERTY(QString songKey READ songKey NOTIFY mediaChanged) // SHA-256 hex, the SongLibrary key
    Q_PROPERTY(qint64 songDurationMs READ songDurationMs NOTIFY mediaChanged)
    Q_PROPERTY(qint64 songBytes READ songBytes NOTIFY mediaChanged)
    Q_PROPERTY(bool songPending READ songPending NOTIFY mediaChanged)
    // --- lists [lists] (read-only)
    // [{index, accountId, name, initials, avatarKey, isContact, isSelf}]
    Q_PROPERTY(QVariantList topFriends READ topFriends NOTIFY listsChanged)
    Q_PROPERTY(QVariantList moduleArrangement READ moduleArrangement NOTIFY listsChanged) // [{module, name, column, visible, hasContent}]
    Q_PROPERTY(QVariantList narrowModules READ narrowModules NOTIFY listsChanged) // module ids to show, in order
    Q_PROPERTY(QVariantList wideModules READ wideModules NOTIFY listsChanged)
    Q_PROPERTY(QVariantList interestRows READ interestRows NOTIFY listsChanged) // [{label, value}], filled only
    Q_PROPERTY(QVariantList detailRows READ detailRows NOTIFY listsChanged)
    Q_PROPERTY(int filledInterestCount READ filledInterestCount NOTIFY listsChanged)
    Q_PROPERTY(int filledDetailCount READ filledDetailCount NOTIFY listsChanged)
    Q_PROPERTY(bool hasInterests READ hasInterests NOTIFY listsChanged)
    Q_PROPERTY(bool hasDetails READ hasDetails NOTIFY listsChanged)
    Q_PROPERTY(bool hasBlurbs READ hasBlurbs NOTIFY listsChanged)
    // "From your page": base, fade, pattern, box, border, strip, body, link.
    Q_PROPERTY(QVariantList paletteSwatches READ paletteSwatches NOTIFY listsChanged)
    // --- render
    Q_PROPERTY(OpenChat::ProfileRenderStyle *render READ render CONSTANT)

public:
    enum class Kind { View, Draft, TryOn };

    // Where a page's blobs are and which picture key it shows; the
    // controller fills it in, since only it knows whose media this is.
    struct MediaState final {
        bool backgroundPresent = false;
        QString imageKey; // registered with ProfileMediaStore; "" when not shown
        bool songPresent = false;
        friend bool operator==(const MediaState &, const MediaState &) = default;
    };

    // The Top Friend tiles for a list of friends, as this viewer sees them
    // (ARCH §7.2: the viewer's own names and pictures for their contacts).
    using TileResolver = std::function<QVariantList(const QVector<Profile::TopFriend> &)>;

    explicit ProfilePageObject(Kind kind, QObject *parent = nullptr);

    [[nodiscard]] Kind kind() const noexcept { return m_kind; }
    [[nodiscard]] const Profile::Page &page() const noexcept { return m_page; }
    // Replaces the page. Emits only the groups that changed, never edited().
    void load(const Profile::Page &page);
    // The same as a write from QML: emits edited() when anything changed.
    // Ignored on a read-only instance.
    void edit(const Profile::Page &page);
    void setViewer(const ProfileRenderStyle::Viewer &viewer);
    [[nodiscard]] const ProfileRenderStyle::Viewer &viewer() const noexcept { return m_viewer; }
    void setMediaState(const MediaState &state);
    [[nodiscard]] const MediaState &mediaState() const noexcept { return m_media; }
    void setTileResolver(TileResolver resolver);
    // The roster (names, pictures) the tiles come from changed.
    void refreshTiles();
    // Decoded picture statistics arrived: the readability pass may differ.
    void refreshRender();
    // The owner's knobs as this viewer sees them (adaptive colours substituted).
    [[nodiscard]] Profile::Theme shownTheme() const;

    [[nodiscard]] bool readOnly() const noexcept { return m_kind != Kind::Draft; }
    [[nodiscard]] bool showEmptyModules() const noexcept { return m_kind != Kind::View; }
    [[nodiscard]] qint64 revision() const { return m_page.revision; }
    [[nodiscard]] int preset() const { return int(m_page.preset); }
    [[nodiscard]] bool adaptive() const { return m_page.theme.adaptive; }
    void setAdaptive(bool adaptive);
    [[nodiscard]] int layout() const { return int(m_page.layout); }
    void setLayout(int layout);
    [[nodiscard]] bool styleEditedSincePreset() const;

    [[nodiscard]] int backgroundKind() const { return int(m_page.theme.backgroundKind); }
    void setBackgroundKind(int kind);
    [[nodiscard]] QColor backgroundColor1() const;
    void setBackgroundColor1(const QColor &color);
    [[nodiscard]] QColor backgroundColor2() const;
    void setBackgroundColor2(const QColor &color);
    [[nodiscard]] int motif() const { return int(m_page.theme.motif); }
    void setMotif(int motif);
    [[nodiscard]] QColor motifInk() const;
    void setMotifInk(const QColor &color);
    [[nodiscard]] int motifOpacity() const { return m_page.theme.motifOpacity; }
    void setMotifOpacity(int percent);
    [[nodiscard]] int motifScale() const { return int(m_page.theme.motifScale); }
    void setMotifScale(int scale);
    [[nodiscard]] int imageMode() const { return int(m_page.theme.imageMode); }
    void setImageMode(int mode);
    [[nodiscard]] bool imageFixed() const { return m_page.theme.imageFixed; }
    void setImageFixed(bool fixed);

    [[nodiscard]] QColor boxFill() const;
    void setBoxFill(const QColor &color);
    [[nodiscard]] int boxOpacity() const;
    void setBoxOpacity(int percent);
    [[nodiscard]] QColor borderColor() const;
    void setBorderColor(const QColor &color);
    [[nodiscard]] int borderWidth() const { return m_page.theme.borderWidth; }
    void setBorderWidth(int width);
    [[nodiscard]] int borderStyle() const { return int(m_page.theme.borderStyle); }
    void setBorderStyle(int style);
    [[nodiscard]] int boxRadius() const { return int(m_page.theme.boxRadius); }
    void setBoxRadius(int radius);
    [[nodiscard]] bool boxGlow() const { return m_page.theme.boxGlow; }
    void setBoxGlow(bool glow);
    [[nodiscard]] int tableStyle() const { return int(m_page.theme.tableStyle); }
    void setTableStyle(int style);
    [[nodiscard]] QColor altBorderColor() const;
    void setAltBorderColor(const QColor &color);

    [[nodiscard]] int headerStyle() const { return int(m_page.theme.headerStyle); }
    void setHeaderStyle(int style);
    [[nodiscard]] QColor headerFill() const;
    void setHeaderFill(const QColor &color);
    [[nodiscard]] QColor headerText() const;
    void setHeaderText(const QColor &color);
    [[nodiscard]] bool altHeader() const { return m_page.theme.altHeader; }
    void setAltHeader(bool alt);
    [[nodiscard]] QColor altHeaderFill() const;
    void setAltHeaderFill(const QColor &color);
    [[nodiscard]] QColor altHeaderText() const;
    void setAltHeaderText(const QColor &color);

    [[nodiscard]] int headingFont() const { return int(m_page.theme.headingFont); }
    void setHeadingFont(int font);
    [[nodiscard]] int bodyFont() const { return int(m_page.theme.bodyFont); }
    void setBodyFont(int font);
    [[nodiscard]] int textSize() const { return int(m_page.theme.textSize); }
    void setTextSize(int size);
    [[nodiscard]] QColor bodyColor() const;
    void setBodyColor(const QColor &color);
    [[nodiscard]] QColor labelColor() const;
    void setLabelColor(const QColor &color);
    [[nodiscard]] QColor linkColor() const;
    void setLinkColor(const QColor &color);

    [[nodiscard]] int nameFont() const { return int(m_page.theme.nameFont); }
    void setNameFont(int font);
    [[nodiscard]] QColor nameColor() const;
    void setNameColor(const QColor &color);
    [[nodiscard]] QColor nameColor2() const;
    void setNameColor2(const QColor &color);
    [[nodiscard]] int nameEffect() const { return int(m_page.theme.nameEffect); }
    void setNameEffect(int effect);
    [[nodiscard]] int nameSize() const { return int(m_page.theme.nameSize); }
    void setNameSize(int size);
    [[nodiscard]] int nameFlourish() const { return int(m_page.theme.nameFlourish); }
    void setNameFlourish(int flourish);
    [[nodiscard]] int ambient() const { return int(m_page.theme.ambient); }
    void setAmbient(int ambient);

    [[nodiscard]] QString displayName() const { return m_page.content.displayName; }
    void setDisplayName(const QString &text);
    [[nodiscard]] QString headline() const { return m_page.content.headline; }
    void setHeadline(const QString &text);
    [[nodiscard]] QString infoLine1() const { return infoLine(0); }
    void setInfoLine1(const QString &text) { setInfoLine(0, text); }
    [[nodiscard]] QString infoLine2() const { return infoLine(1); }
    void setInfoLine2(const QString &text) { setInfoLine(1, text); }
    [[nodiscard]] QString infoLine3() const { return infoLine(2); }
    void setInfoLine3(const QString &text) { setInfoLine(2, text); }
    [[nodiscard]] int mood() const { return int(m_page.content.mood); }
    void setMood(int mood);
    [[nodiscard]] QString interestGeneral() const { return m_page.content.interests.general; }
    void setInterestGeneral(const QString &text);
    [[nodiscard]] QString interestMusic() const { return m_page.content.interests.music; }
    void setInterestMusic(const QString &text);
    [[nodiscard]] QString interestMovies() const { return m_page.content.interests.movies; }
    void setInterestMovies(const QString &text);
    [[nodiscard]] QString interestTelevision() const { return m_page.content.interests.television; }
    void setInterestTelevision(const QString &text);
    [[nodiscard]] QString interestBooks() const { return m_page.content.interests.books; }
    void setInterestBooks(const QString &text);
    [[nodiscard]] QString interestHeroes() const { return m_page.content.interests.heroes; }
    void setInterestHeroes(const QString &text);
    [[nodiscard]] int hereFor() const { return m_page.content.details.hereFor; }
    void setHereFor(int bits);
    [[nodiscard]] QString hometown() const { return m_page.content.details.hometown; }
    void setHometown(const QString &text);
    [[nodiscard]] int zodiac() const { return int(m_page.content.details.zodiac); }
    void setZodiac(int zodiac);
    [[nodiscard]] QString occupation() const { return m_page.content.details.occupation; }
    void setOccupation(const QString &text);
    [[nodiscard]] QString education() const { return m_page.content.details.education; }
    void setEducation(const QString &text);
    [[nodiscard]] QString languages() const { return m_page.content.details.languages; }
    void setLanguages(const QString &text);
    [[nodiscard]] QString aboutMe() const { return m_page.content.aboutMe; }
    void setAboutMe(const QString &text);
    [[nodiscard]] QString meet() const { return m_page.content.meet; }
    void setMeet(const QString &text);
    [[nodiscard]] QString songTitle() const { return m_page.content.songTitle; }
    void setSongTitle(const QString &text);
    [[nodiscard]] QString songArtist() const { return m_page.content.songArtist; }
    void setSongArtist(const QString &text);

    [[nodiscard]] bool hasBackgroundImage() const { return m_page.background.isSet(); }
    [[nodiscard]] QString backgroundImageKey() const { return m_media.imageKey; }
    [[nodiscard]] bool backgroundPending() const { return m_page.background.isSet() && !m_media.backgroundPresent; }
    [[nodiscard]] bool hasSong() const { return m_page.song.isSet(); }
    [[nodiscard]] QString songKey() const;
    [[nodiscard]] qint64 songDurationMs() const { return m_page.song.durationMs; }
    [[nodiscard]] qint64 songBytes() const { return m_page.song.bytes; }
    [[nodiscard]] bool songPending() const { return m_page.song.isSet() && !m_media.songPresent; }

    [[nodiscard]] QVariantList topFriends() const { return m_lists.topFriends; }
    [[nodiscard]] QVariantList moduleArrangement() const { return m_lists.moduleArrangement; }
    [[nodiscard]] QVariantList narrowModules() const { return m_lists.narrowModules; }
    [[nodiscard]] QVariantList wideModules() const { return m_lists.wideModules; }
    [[nodiscard]] QVariantList interestRows() const { return m_lists.interestRows; }
    [[nodiscard]] QVariantList detailRows() const { return m_lists.detailRows; }
    [[nodiscard]] int filledInterestCount() const { return m_lists.interestRows.size(); }
    [[nodiscard]] int filledDetailCount() const { return m_lists.detailRows.size(); }
    [[nodiscard]] bool hasInterests() const { return !m_lists.interestRows.isEmpty(); }
    [[nodiscard]] bool hasDetails() const { return !m_lists.detailRows.isEmpty(); }
    [[nodiscard]] bool hasBlurbs() const { return m_lists.hasBlurbs; }
    [[nodiscard]] QVariantList paletteSwatches() const { return m_lists.paletteSwatches; }

    [[nodiscard]] ProfileRenderStyle *render() noexcept { return &m_render; }

    // Whether module `module` has anything to show on this page (the handle
    // box is the app's, so it always has).
    [[nodiscard]] static bool moduleHasContent(const Profile::Page &page, Profile::Module module);
    // A monogram's letters: the first letter of the first and last words, or
    // of the only one, upper-cased; "?" when the name has no letter at all.
    [[nodiscard]] static QString initialsFor(const QString &name);
    // Tiles for friends nobody resolved: the owner's labels over monograms.
    [[nodiscard]] static QVariantList monogramTiles(const QVector<Profile::TopFriend> &friends);

signals:
    void styleChanged();
    void contentChanged();
    void listsChanged();
    void mediaChanged();
    // A write changed the page (never emitted by load()).
    void edited();

private:
    // Everything the lists group derives from the page and the viewer.
    struct Lists final {
        QVariantList topFriends, moduleArrangement, narrowModules, wideModules;
        QVariantList interestRows, detailRows;
        bool hasBlurbs = false;
        QVariantList paletteSwatches;
        friend bool operator==(const Lists &, const Lists &) = default;
    };

    void assign(const Profile::Page &page, bool write);
    [[nodiscard]] Lists computeLists() const;
    void updateLists();
    // A theme write: `change` is applied to what the viewer sees; `colour`
    // marks the knobs an adaptive page takes from Aero Sky.
    void writeTheme(const std::function<void(Profile::Theme &)> &change, bool colour);
    void writeColour(quint32 Profile::Theme::*field, const QColor &color);
    void writeContent(const std::function<void(Profile::Content &)> &change);
    void writeText(QString Profile::Content::*field, const QString &text, int bound, bool multiLine);
    void writeInterest(QString Profile::Interests::*field, const QString &text);
    void writeDetail(QString Profile::Details::*field, const QString &text);
    [[nodiscard]] QColor shownColour(quint32 Profile::Theme::*field) const;
    [[nodiscard]] QString infoLine(int index) const;
    void setInfoLine(int index, const QString &text);

    Kind m_kind;
    Profile::Page m_page;
    ProfileRenderStyle::Viewer m_viewer;
    MediaState m_media;
    TileResolver m_tiles;
    Lists m_lists;
    ProfileRenderStyle m_render;
};

} // namespace OpenChat
