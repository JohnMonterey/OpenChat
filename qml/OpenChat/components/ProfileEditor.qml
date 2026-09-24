import QtQuick
import QtQuick.Controls
import OpenChat
import OpenChat.Native

// The owner's editor (SPEC §14): the tab rail (76) and the panel (300) docked
// left, and the live preview taking the rest. Entering, the rail and panel
// slide in from x −376 over 140 ms while the preview reflows at once; Preview
// in the bar slides them out the same way (`previewOnly`). Both are instant
// when animations are off (Low memory mode, reduced motion).
//
// Clicking a box in the preview opens its tab and field (SPEC §14.4). The
// editor's shortcuts are Ctrl+S, Ctrl+Z, Ctrl+Shift+Z / Ctrl+Y, Ctrl+1…9 and
// F6 (rail → panel → preview → top bar); none fires while a popup is open,
// and inside a focused text field Ctrl+Z stays the field's own undo (its
// focus period becomes one history step when focus leaves). Leaving with
// unsaved changes goes through requestLeave(), which asks with the leave
// dialog; a surviving draft is offered at the top of the panel.
//
// ProfilePage hosts it while `profiles.editing`. The host may set any of the
// inputs below; those it leaves unset come from the ProfilePage around the
// editor (objectName "profilePage", ARCH §8.1): its profiles and controllers,
// its SongPlayer ("profileSongPlayer") and its picture FileDialog
// ("localAvatarFileDialog"), so the editor works however the page loads it.
FocusScope {
    id: editor
    objectName: "profileEditor"

    property Item page: null
    property var profiles: page ? page.profiles : null
    property var chatController: page ? page.chatController : null
    property var contactController: page ? page.contactController : null
    property var callController: page ? page.callController : null
    property var songPlayer: null
    property var avatarFileDialog: null

    // Set by ProfileEditorBar: the bar itself (F6 reaches it) and whether its
    // Discard popover or history menu is open.
    property Item bar: null
    property bool barPopupOpen: false

    property bool previewOnly: false
    readonly property bool popupOpen: leaveDialog.opened || colorPicker.opened || panel.popupOpen || barPopupOpen
    readonly property Item colorPickerWell: colorPicker.opened ? colorPicker.well : null
    readonly property var tabNames: ["themes", "background", "boxes", "text", "name", "about", "friends", "song", "layout"]
    readonly property string currentTab: profiles ? tabNames[profiles.lastTab] || "themes" : "themes"
    readonly property bool animated: ProfileRenderPolicy.animationsAllowed
    readonly property bool shortcutsEnabled: visible && profiles !== null && profiles.editing && !popupOpen
                                             && !(page && page.popupOpen === true)
    // The preview target "Show me" points at for a moment.
    property string pulseTarget: ""

    // Where each preview target is edited (SPEC §14.4); app-owned boxes
    // (contacting, banner, handle, status) have no entry and stay inert.
    readonly property var editTargets: ({
        name: ["name", ""], photo: ["about", "photo"], headline: ["about", "headline"],
        info: ["about", "info"], mood: ["about", "mood"], aboutMe: ["about", "aboutMe"],
        meet: ["about", "meet"], interests: ["about", "interests"], details: ["about", "details"],
        song: ["song", ""], friends: ["friends", ""], strip: ["boxes", "strip"], backdrop: ["background", ""]
    })

    // 1 while the rail and panel are out of view.
    property real slide: 1
    Behavior on slide {
        enabled: editor.animated
        NumberAnimation { duration: 140; easing.type: Easing.InOutQuad }
    }

    // A pending "leave" (Back, Esc, the history menu, closing the window).
    property var pendingLeave: null
    // A leave waiting for a Save that waits for an import ("Saving…").
    property var leaveAfterPublish: null

    // Opens tab `name`. Focus goes to `field` in it (a preview target's
    // field), to its first control for "", or stays on the rail for null.
    function openTab(name, field) {
        const index = tabNames.indexOf(name);
        if (!profiles || index < 0)
            return;
        previewOnly = false;
        profiles.lastTab = index;
        if (field === null)
            rail.forceActiveFocus(Qt.ShortcutFocusReason);
        else
            panel.focusField(field);
    }
    function handleEditRequest(target) {
        const entry = editTargets[target];
        if (entry)
            openTab(entry[0], entry[1]);
    }
    function showMe(role) {
        const targets = { 0: "aboutMe", 1: "details", 2: "contacting", 3: "name", 4: "strip", 5: "strip" };
        pulseTarget = targets[role] || "backdrop";
        pulseTimer.restart();
    }
    // Leaves the editor through `proceed` (end editing, pop to a page, close
    // the window), asking first when the draft has unsaved changes.
    function requestLeave(proceed) {
        if (!profiles || !profiles.draftDirty) {
            proceed();
            return;
        }
        pendingLeave = proceed;
        leaveDialog.open();
    }
    function openColorPicker(well) {
        if (colorPicker.opened)
            colorPicker.close();
        colorPicker.openFor(well, panelArea.mapToItem(colorPicker.parent, panelArea.width - 6, 0).x);
    }
    function focusRegion(region) {
        if (region === "rail")
            rail.forceActiveFocus(Qt.TabFocusReason);
        else if (region === "panel")
            panel.focusFirst();
        else if (region === "preview")
            preview.focusPreview();
        else if (region === "bar" && bar)
            bar.focusFirst();
    }
    function regionOf(item) {
        for (let at = item; at; at = at.parent) {
            if (at === rail)
                return "rail";
            if (at === panelArea)
                return "panel";
            if (at === preview)
                return "preview";
            if (bar && at === bar)
                return "bar";
        }
        return "";
    }
    // F6: rail → panel → preview → top bar, skipping what Preview hides.
    function cycleFocus() {
        const order = previewOnly ? ["preview", "bar"] : ["rail", "panel", "preview", "bar"];
        const at = order.indexOf(regionOf(Window.activeFocusItem));
        let next = order[(at + 1) % order.length];
        if (next === "bar" && !bar)
            next = order[0];
        focusRegion(next);
    }
    function findAncestor(name) {
        for (let item = parent; item; item = item.parent) {
            if (item.objectName === name)
                return item;
        }
        return null;
    }
    function findOwned(owner, name) {
        if (!owner)
            return null;
        for (let i = 0; i < owner.data.length; ++i) {
            if (owner.data[i] && owner.data[i].objectName === name)
                return owner.data[i];
        }
        return null;
    }

    Component.onCompleted: {
        if (!page)
            page = findAncestor("profilePage");
        if (!songPlayer)
            songPlayer = findOwned(page, "profileSongPlayer");
        if (!avatarFileDialog)
            avatarFileDialog = findOwned(page, "localAvatarFileDialog");
        slide = previewOnly ? 1 : 0;
    }
    onPreviewOnlyChanged: slide = previewOnly ? 1 : 0

    Timer {
        id: pulseTimer
        interval: 1500
        onTriggered: editor.pulseTarget = ""
    }

    ProfilePreviewFrame {
        id: preview
        x: editor.previewOnly ? 0 : rail.width + panelArea.width
        width: editor.width - x
        height: editor.height
        profiles: editor.profiles
        chatController: editor.chatController
        contactController: editor.contactController
        callController: editor.callController
        songPlayer: editor.songPlayer
        editingTarget: editor.pulseTarget.length > 0 ? editor.pulseTarget : panel.editingTarget
        onEditRequested: target => editor.handleEditRequest(target)
    }

    Rectangle {
        // Under the rail and panel while they slide in: the preview has
        // already moved over, so the strip they cross is the panel's surface.
        visible: editor.slide > 0 && !editor.previewOnly
        width: rail.width + panelArea.width
        height: editor.height
        color: Theme.contentBackground
    }

    ProfileEditorRail {
        id: rail
        x: -(rail.width + panelArea.width) * editor.slide
        visible: editor.slide < 1
        height: editor.height
        profiles: editor.profiles
    }

    Item {
        id: panelArea
        x: rail.x + rail.width
        visible: editor.slide < 1
        width: 300
        height: editor.height

        ProfileEditorPanel {
            id: panel
            anchors.fill: parent
            profiles: editor.profiles
            editor: editor
            tab: editor.currentTab
        }
    }

    ProfileColorPicker {
        id: colorPicker
        profiles: editor.profiles
        page: editor.profiles ? editor.profiles.draft : null
    }

    ProfileLeaveDialog {
        id: leaveDialog
        onSaveChosen: {
            const profiles = editor.profiles;
            const proceed = editor.pendingLeave;
            editor.pendingLeave = null;
            if (!profiles || !profiles.publish())
                return; // the reason shows as the page's notice; the owner stays
            if (profiles.publishPending) {
                editor.leaveAfterPublish = proceed; // "Saving…" until the import lands
                return;
            }
            if (proceed)
                proceed();
        }
        onDiscardChosen: {
            const profiles = editor.profiles;
            const proceed = editor.pendingLeave;
            editor.pendingLeave = null;
            if (profiles)
                profiles.discardChanges();
            if (proceed)
                proceed();
        }
        onKeepEditingChosen: editor.pendingLeave = null
    }

    Connections {
        target: editor.profiles
        function onPublished() {
            const proceed = editor.leaveAfterPublish;
            editor.leaveAfterPublish = null;
            if (proceed)
                proceed();
        }
        function onPublishedChanged() {
            // The Save it waited for gave up (an import failed): stay.
            if (editor.leaveAfterPublish && editor.profiles && !editor.profiles.publishPending
                && editor.profiles.editing)
                editor.leaveAfterPublish = null;
        }
    }

    Shortcut {
        sequences: [StandardKey.Save]
        enabled: editor.shortcutsEnabled
        onActivated: {
            if (editor.profiles.draftDirty)
                editor.profiles.publish();
        }
    }
    Shortcut {
        sequences: [StandardKey.Undo]
        enabled: editor.shortcutsEnabled
        onActivated: editor.profiles.undo()
    }
    Shortcut {
        sequences: ["Ctrl+Shift+Z", "Ctrl+Y"]
        enabled: editor.shortcutsEnabled
        onActivated: editor.profiles.redo()
    }
    Shortcut {
        sequences: ["F6"]
        enabled: editor.shortcutsEnabled
        onActivated: editor.cycleFocus()
    }
    Instantiator {
        model: 9
        delegate: Shortcut {
            required property int index
            sequence: "Ctrl+" + (index + 1)
            enabled: editor.shortcutsEnabled
            onActivated: editor.openTab(editor.tabNames[index], null)
        }
    }
}
