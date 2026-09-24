import QtQuick
import QtQuick.Controls
import OpenChat
import OpenChat.Native

// A themed profile page body (SPEC §3-§5): everything under the app's top
// bar. Bottom to top: the fixed backdrop (the page's one viewport-sized
// raster), the background picture as a scene-graph texture, the ambient
// sprites, then the scrolling columns of boxes. Identity and Contacting open
// the narrow column and the banner opens the wide one; the owner's movable
// modules follow in their order. Two columns at every viewer width (the
// window's minimum is 720); one column for the Single layout or an editor
// preview narrower than 632 px.
//
// mode "view" is the page on screen; mode "preview" is the editor's live
// preview of the draft: every module wears the editor's affordances, empty
// modules show as placeholders, the app's own boxes are inert and a Top
// Friend opens the Top Friends tab instead of navigating.
Item {
    id: pageView
    objectName: "profilePageView"

    // --- Inputs (frozen, ARCH §8.1)
    property var page: null
    property var profiles: null
    property var chatController: null
    property var contactController: null
    property var callController: null
    property var songPlayer: null
    property string mode: "view"
    property string editingTarget: ""
    signal editRequested(string target)

    // --- Additions
    // Glitter, ambient sprites and the equaliser may move (the page's open
    // fade has finished); they also stop by themselves when hidden.
    property bool animate: true
    // How a Top Friend is opened: the page passes a function that runs the
    // move inside its fade-through. Unset: openTopFriend at once.
    property var navigate: null
    // Your own photo or "Change Picture" was chosen: the page opens the
    // picture dialog it owns.
    signal changePictureRequested()

    readonly property bool preview: mode === "preview"
    readonly property var render: page ? page.render : null
    readonly property string ownerFirstName: profiles ? profiles.personFirstName : ""
    readonly property bool popupOpen: false

    // --- Geometry (SPEC §3.1)
    readonly property int margin: Math.max(16, Math.min(32, 16 + Math.round((width - 720) / 4)))
    readonly property int contentWidth: Math.max(0, Math.min(940, width - 2 * margin))
    readonly property int gutter: contentWidth >= 800 ? 16 : 14
    readonly property int vgap: contentWidth >= 800 ? 14 : 12
    readonly property bool twoColumns: page !== null && page.layout !== Profile.SingleLayout && contentWidth >= 600
    readonly property int narrowWidth: twoColumns ? Math.round((contentWidth - gutter) * 0.41) : Math.min(620, contentWidth)
    readonly property int wideWidth: twoColumns ? contentWidth - gutter - narrowWidth : narrowWidth
    readonly property int leftEdge: Math.round((width - (twoColumns ? contentWidth : narrowWidth)) / 2)
    readonly property bool flipped: page !== null && page.layout === Profile.FlippedLayout
    readonly property int topPad: 16
    readonly property int bottomPad: 24
    readonly property alias flickable: flick

    // "Michael's", "James'".
    function possessive(name) {
        return name.length > 0 && name.charAt(name.length - 1).toLowerCase() === "s" ? name + "'" : name + "'s";
    }
    function requestChangePicture() {
        pageView.changePictureRequested();
    }
    function activateTopFriend(index) {
        if (pageView.preview) {
            pageView.editRequested("friends");
            return;
        }
        if (!pageView.profiles)
            return;
        const profiles = pageView.profiles;
        if (pageView.navigate)
            pageView.navigate(() => profiles.openTopFriend(index));
        else
            profiles.openTopFriend(index);
    }

    // --- Which boxes, in which column. Recomputed only when something they
    // depend on changes, and applied only when the lists really differ, so a
    // refresh never rebuilds a module (or loses keyboard focus in one).
    property var narrowKeys: []
    property var wideKeys: []
    function moduleKeys() {
        if (!pageView.page || !pageView.profiles)
            return { narrow: [], wide: [] };
        const narrow = ["identity", "contacting"];
        const wide = ["banner"];
        if (!pageView.preview && pageView.profiles.relationship === Profile.ContactPerson
                && pageView.profiles.pageState === Profile.DefaultPage)
            wide.push("nopage");
        const narrowModules = pageView.page.narrowModules;
        const wideModules = pageView.page.wideModules;
        for (let i = 0; i < narrowModules.length; ++i)
            narrow.push("m" + narrowModules[i]);
        for (let j = 0; j < wideModules.length; ++j)
            wide.push("m" + wideModules[j]);
        if (pageView.twoColumns)
            return { narrow: narrow, wide: wide };
        // One column: Identity, Contacting, the banner, then the wide
        // column's modules, then the narrow column's.
        return { narrow: narrow.slice(0, 2).concat(wide).concat(narrow.slice(2)), wide: [] };
    }
    function sameKeys(a, b) {
        if (a.length !== b.length)
            return false;
        for (let i = 0; i < a.length; ++i) {
            if (a[i] !== b[i])
                return false;
        }
        return true;
    }
    readonly property var computedKeys: moduleKeys()
    onComputedKeysChanged: {
        if (!sameKeys(pageView.narrowKeys, computedKeys.narrow))
            pageView.narrowKeys = computedKeys.narrow;
        if (!sameKeys(pageView.wideKeys, computedKeys.wide))
            pageView.wideKeys = computedKeys.wide;
    }
    Component.onCompleted: {
        pageView.narrowKeys = computedKeys.narrow;
        pageView.wideKeys = computedKeys.wide;
        pageView.applyNavigationScroll();
    }

    // --- Scrolling. contentY moves only on navigation (0 on a push, the
    // saved position on a pop) and is reported back, debounced, so this
    // entry reopens where it was left. A resize keeps the top module in view.
    property real pendingScroll: -1
    property string anchorKey: ""
    property real anchorOffset: 0
    property bool restoringAnchor: false
    // While a resize reflows the columns, the anchor is the one from before.
    property bool anchorLocked: false

    function applyNavigationScroll() {
        if (pageView.preview || !pageView.profiles)
            return;
        pageView.pendingScroll = Math.max(0, pageView.profiles.entryScrollY);
        pageView.anchorKey = "";
        pendingScrollExpiry.restart();
        Qt.callLater(pageView.flushPendingScroll);
    }
    function maxScroll() {
        return Math.max(0, flick.contentHeight - flick.height);
    }
    function flushPendingScroll() {
        if (pageView.pendingScroll < 0)
            return;
        const wanted = pageView.pendingScroll;
        flick.contentY = Math.min(wanted, pageView.maxScroll());
        // Keep waiting while the columns are still growing towards it.
        if (flick.contentY >= wanted - 0.5)
            pageView.pendingScroll = -1;
    }
    function slotAt(y) {
        const columns = [narrowColumn, wideColumn];
        let best = null;
        for (let c = 0; c < columns.length; ++c) {
            const column = columns[c];
            if (!column.visible)
                continue;
            for (let i = 0; i < column.children.length; ++i) {
                const slot = column.children[i];
                if (!slot.visible || slot.moduleKey === undefined)
                    continue;
                // The module the top edge of the view cuts through (or the
                // first one below it): of those, the one that starts lowest.
                const top = column.y + slot.y;
                if (top + slot.height <= y)
                    continue;
                const crossing = top <= y;
                if (best === null || (crossing && (!best.crossing || top > best.top))
                        || (!crossing && !best.crossing && top < best.top))
                    best = { key: slot.moduleKey, top: top, crossing: crossing };
            }
        }
        return best;
    }
    function findSlot(key) {
        const columns = [narrowColumn, wideColumn];
        for (let c = 0; c < columns.length; ++c) {
            for (let i = 0; i < columns[c].children.length; ++i) {
                const slot = columns[c].children[i];
                if (slot.moduleKey === key && slot.visible)
                    return { top: columns[c].y + slot.y };
            }
        }
        return null;
    }
    function rememberAnchor() {
        if (pageView.restoringAnchor || pageView.anchorLocked)
            return;
        const slot = pageView.slotAt(flick.contentY);
        pageView.anchorKey = slot ? slot.key : "";
        pageView.anchorOffset = slot ? flick.contentY - slot.top : 0;
    }
    function restoreAnchor() {
        if (pageView.anchorKey.length === 0 || flick.contentY <= 0)
            return;
        const slot = pageView.findSlot(pageView.anchorKey);
        if (!slot)
            return;
        pageView.restoringAnchor = true;
        flick.contentY = Math.max(0, Math.min(slot.top + pageView.anchorOffset, pageView.maxScroll()));
        pageView.restoringAnchor = false;
    }
    onWidthChanged: {
        pageView.anchorLocked = true;
        anchorUnlock.restart();
        Qt.callLater(pageView.restoreAnchor);
    }
    Timer {
        id: anchorUnlock
        interval: 150
        onTriggered: {
            pageView.restoreAnchor();
            pageView.anchorLocked = false;
        }
    }
    // Only a resize moves the view to its anchor: content that settles
    // after a restored position must not nudge it.
    function columnsSettled() {
        if (pageView.anchorLocked && pageView.pendingScroll < 0)
            pageView.restoreAnchor();
    }

    Connections {
        target: pageView.profiles
        ignoreUnknownSignals: true
        function onNavigationChanged() {
            if (pageView.profiles.open)
                pageView.applyNavigationScroll();
        }
    }
    // A restored position waits for the columns to grow back to it, but
    // not for ever, and never against the viewer's own scrolling.
    Timer {
        id: pendingScrollExpiry
        interval: 1000
        onTriggered: pageView.pendingScroll = -1
    }
    Timer {
        id: reportScroll
        interval: 200
        onTriggered: {
            if (!pageView.preview && pageView.profiles && pageView.profiles.open)
                pageView.profiles.setEntryScrollY(flick.contentY);
        }
    }

    // --- Layers
    ProfileBackdrop {
        id: backdrop
        objectName: "profileBackdrop"
        anchors.fill: parent
        kind: pageView.render ? pageView.render.backgroundKind : Profile.SolidBackground
        color1: pageView.render ? pageView.render.color1 : Theme.contentBackground
        color2: pageView.render ? pageView.render.color2 : Theme.contentBackground
        motif: pageView.render ? pageView.render.motif : 0
        motifScale: pageView.render ? pageView.render.motifScale : Profile.MediumMotif
        motifInk: pageView.render ? pageView.render.motifInk : "transparent"
        motifOpacity: pageView.render ? pageView.render.motifOpacity : 0
        Accessible.ignored: true
    }
    ProfileImageLayer {
        id: imageLayer
        objectName: "profileImageLayer"
        anchors.fill: parent
        imageKey: pageView.render ? pageView.render.imageKey : ""
        imageMode: pageView.render ? pageView.render.imageMode : Profile.FillImage
        scrollOffset: pageView.render && !pageView.render.imageFixed ? flick.contentY : 0
        Accessible.ignored: true
    }
    ProfileAmbient {
        id: ambient
        objectName: "profileAmbient"
        anchors.fill: parent
        kind: pageView.render ? pageView.render.ambient : Profile.NoAmbient
        outlineColor: pageView.render ? pageView.render.ambientOutline : "transparent"
        running: pageView.animate && pageView.render !== null && !pageView.render.plain
        seed: 7
        Accessible.ignored: true
    }

    // The backdrop is the Background tab's target in the preview.
    MouseArea {
        anchors.fill: parent
        enabled: pageView.preview
        cursorShape: pageView.preview ? Qt.PointingHandCursor : Qt.ArrowCursor
        onClicked: pageView.editRequested("backdrop")
    }

    Flickable {
        id: flick
        objectName: "profilePageFlickable"
        anchors.fill: parent
        contentWidth: width
        contentHeight: content.height
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        onContentYChanged: {
            pageView.rememberAnchor();
            if (!pageView.preview)
                reportScroll.restart();
        }
        onContentHeightChanged: Qt.callLater(pageView.flushPendingScroll)
        onMovementStarted: {
            pageView.pendingScroll = -1;
            pageView.anchorLocked = false;
        }

        ScrollBar.vertical: ScrollBar {
            id: scrollBar
            objectName: "profilePageScrollBar"
            parent: pageView
            x: pageView.width - 9
            y: 3
            height: pageView.height - 6
            width: 6
            padding: 0
            minimumSize: 0.08
            policy: ScrollBar.AsNeeded
            background: Item {}
            contentItem: Rectangle {
                implicitWidth: 6
                radius: 3
                color: Theme.mediaChip
                border.width: 1
                border.color: Theme.mediaChipBorder
                visible: scrollBar.size < 1.0
            }
        }

        Item {
            id: content
            width: flick.width
            height: Math.max(narrowColumn.height, wideColumn.visible ? wideColumn.height : 0) + pageView.topPad
                    + pageView.bottomPad

            // The backdrop's click target reaches under the columns too.
            MouseArea {
                anchors.fill: parent
                enabled: pageView.preview
                cursorShape: pageView.preview ? Qt.PointingHandCursor : Qt.ArrowCursor
                onClicked: pageView.editRequested("backdrop")
            }

            Column {
                id: narrowColumn
                objectName: "profileNarrowColumn"
                x: pageView.twoColumns && pageView.flipped ? pageView.leftEdge + pageView.wideWidth + pageView.gutter : pageView.leftEdge
                y: pageView.topPad
                width: pageView.narrowWidth
                spacing: pageView.vgap
                onPositioningComplete: pageView.columnsSettled()
                Repeater {
                    model: pageView.narrowKeys
                    delegate: slotDelegate
                }
            }
            Column {
                id: wideColumn
                objectName: "profileWideColumn"
                visible: pageView.twoColumns
                x: pageView.flipped ? pageView.leftEdge : pageView.leftEdge + pageView.narrowWidth + pageView.gutter
                y: pageView.topPad
                width: pageView.wideWidth
                spacing: pageView.vgap
                onPositioningComplete: pageView.columnsSettled()
                Repeater {
                    model: pageView.wideKeys
                    delegate: slotDelegate
                }
            }
        }
    }

    // One module in its column, with the editor's affordances over it in
    // the preview.
    Component {
        id: slotDelegate
        Item {
            id: slot
            required property var modelData
            readonly property string moduleKey: modelData
            readonly property Item module: loader.item
            readonly property bool placeholder: pageView.preview && module !== null && module.hasPreviewContent === false
            objectName: "profileModule_" + moduleKey
            width: parent ? parent.width : 0
            height: loader.item ? loader.item.implicitHeight : 0
            // A module that has nothing to show right now (a handle not yet
            // known) says so through `present`.
            visible: loader.item !== null && loader.item.present !== false

            Loader {
                id: loader
                width: parent.width
                sourceComponent: pageView.componentFor(slot.moduleKey)
            }
            Loader {
                anchors.fill: parent
                active: pageView.preview && loader.item !== null
                sourceComponent: ProfilePreviewDecor {
                    target: pageView.targetOf(slot.moduleKey)
                    targets: pageView.targetsOf(slot.moduleKey)
                    regions: pageView.regionsOf(slot.moduleKey, slot.module)
                    editingTarget: pageView.editingTarget
                    inert: pageView.targetOf(slot.moduleKey).length === 0
                    inertTip: pageView.inertTipOf(slot.moduleKey)
                    radius: slot.module && slot.module.radius !== undefined ? slot.module.radius : 5
                    placeholder: slot.placeholder
                    placeholderText: pageView.placeholderOf(slot.moduleKey)
                    onActivated: target => pageView.editRequested(target)
                }
            }
        }
    }

    function componentFor(key) {
        switch (key) {
        case "identity": return identityModule;
        case "contacting": return contactingModule;
        case "banner": return bannerModule;
        case "nopage": return noPageModule;
        case "m" + Profile.HandleModule: return handleModule;
        case "m" + Profile.SongModule: return songModule;
        case "m" + Profile.InterestsModule: return interestsModule;
        case "m" + Profile.DetailsModule: return detailsModule;
        case "m" + Profile.BlurbsModule: return blurbsModule;
        case "m" + Profile.TopFriendsModule: return friendsModule;
        }
        return null;
    }
    // The editor tab a module opens (ARCH §8.1's frozen strings); "" for
    // the app's own boxes, which are inert.
    function targetOf(key) {
        switch (key) {
        case "identity": return "name";
        case "m" + Profile.SongModule: return "song";
        case "m" + Profile.InterestsModule: return "interests";
        case "m" + Profile.DetailsModule: return "details";
        case "m" + Profile.BlurbsModule: return "aboutMe";
        case "m" + Profile.TopFriendsModule: return "friends";
        }
        return "";
    }
    function targetsOf(key) {
        switch (key) {
        case "identity": return ["name", "photo", "headline", "info", "mood"];
        case "m" + Profile.BlurbsModule: return ["aboutMe", "meet"];
        }
        const own = pageView.targetOf(key);
        return own.length > 0 ? [own] : [];
    }
    function regionsOf(key, module) {
        if (!module)
            return [];
        const regions = [];
        if (key === "identity") {
            regions.push({ target: "name", item: module.nameItem });
            regions.push({ target: "photo", item: module.photoItem });
            regions.push({ target: "headline", item: module.headlineItem });
            regions.push({ target: "info", item: module.infoItem });
            regions.push({ target: "mood", item: module.moodItem });
            regions.push({ target: "", item: module.statusItem });
        } else if (key === "m" + Profile.BlurbsModule) {
            regions.push({ target: "aboutMe", item: module.aboutItem });
            regions.push({ target: "meet", item: module.meetItem });
        }
        if (module.headerItem && module.showHeader)
            regions.unshift({ target: "strip", item: module.headerItem });
        return regions;
    }
    function inertTipOf(key) {
        switch (key) {
        case "contacting": return "Contacting is written by OpenChat and always shows real actions.";
        case "m" + Profile.HandleModule: return "Your handle comes from your account.";
        case "banner": return "OpenChat writes this line, so it is always true.";
        }
        return "";
    }
    function placeholderOf(key) {
        switch (key) {
        case "m" + Profile.SongModule: return "Add a song. Empty boxes are hidden from contacts.";
        case "m" + Profile.InterestsModule: return "Add your interests. Empty boxes are hidden from contacts.";
        case "m" + Profile.DetailsModule: return "Add your details. Empty boxes are hidden from contacts.";
        case "m" + Profile.BlurbsModule: return "Write about yourself. Empty boxes are hidden from contacts.";
        case "m" + Profile.TopFriendsModule: return "Pick your Top Friends. Empty boxes are hidden from contacts.";
        }
        return "";
    }

    Component {
        id: identityModule
        ProfileIdentityModule { view: pageView }
    }
    Component {
        id: contactingModule
        ProfileContactingModule { view: pageView }
    }
    Component {
        id: bannerModule
        ProfileBannerModule { view: pageView }
    }
    Component {
        id: noPageModule
        ProfileNoPageModule { view: pageView }
    }
    Component {
        id: handleModule
        ProfileHandleModule { view: pageView }
    }
    Component {
        id: songModule
        ProfileSongModule {
            view: pageView
            // An empty song slot still shows (as a placeholder) in the preview.
            readonly property bool hasPreviewContent: pageView.page !== null && pageView.page.hasSong
            readonly property bool present: pageView.page !== null && (pageView.page.hasSong || pageView.preview)
        }
    }
    Component {
        id: interestsModule
        ProfileInterestsModule {
            view: pageView
            readonly property bool hasPreviewContent: pageView.page !== null && pageView.page.hasInterests
        }
    }
    Component {
        id: detailsModule
        ProfileDetailsModule {
            view: pageView
            readonly property bool hasPreviewContent: pageView.page !== null && pageView.page.hasDetails
        }
    }
    Component {
        id: blurbsModule
        ProfileBlurbsModule {
            view: pageView
            readonly property bool hasPreviewContent: pageView.page !== null && pageView.page.hasBlurbs
        }
    }
    Component {
        id: friendsModule
        ProfileFriendSpaceModule {
            view: pageView
            readonly property bool hasPreviewContent: pageView.page !== null && pageView.page.topFriends.length > 0
        }
    }
}
