import QtQuick
import QtQuick.Controls.Basic
import OpenChat
import OpenChat.Native

// The owner's editor (SPEC §14): the tab rail (76) and the panel (300) docked
// left, and the live preview taking the rest. Entering, the rail and panel
// slide in from x −376 over 140 ms while the preview reflows at once; Preview
// in the bar slides them out the same way (`previewOnly`). Leaving, the page
// keeps the editor for slideOut(): the preview gives way to the page itself
// underneath and the rail and panel slide off over it. All of it is instant
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
// ProfilePage hosts it while `profiles.editing` and while it slides out. The
// host may set any of the inputs below; those it leaves unset come from the
// ProfilePage around the editor (objectName "profilePage", ARCH §8.1): its
// profiles and controllers, its SongPlayer ("profileSongPlayer") and its
// picture FileDialog ("localAvatarFileDialog"), so the editor works however
// the page loads it. A leave whose Save waits for an import goes on through
// the page (leaveAfterPublish), which outlives the editor.
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
    // What "Show me" pulses in the preview for a moment (SPEC §9): the text
    // an adjustment changed, by kind (ProfilePageView.pulseTarget).
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
    // Editing has ended: the rail and panel are sliding away (slideOut()).
    property bool leaving: false
    signal slidOut()

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
    // An ink role (Profile.InkRole), or -1 for the boxes' own adjustments.
    function showMe(role) {
        const kinds = { 0: "body", 1: "label", 2: "link", 3: "name", 4: "strip", 5: "altStrip" };
        pulseTarget = ""; // asked again: the pulse starts over
        pulseTarget = kinds[role] || "boxes";
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
    // Editing has ended and the page shows under the editor: the rail and
    // panel slide off over it, then slidOut() tells the page to let go.
    function slideOut() {
        leaving = true;
        if (slide >= 1)
            slidOut();
        else
            slide = 1;
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
    onSlideChanged: {
        if (leaving && slide >= 1)
            slidOut();
    }
    // Nothing of a leaving editor answers the pointer or the keyboard.
    enabled: !leaving

    // Two pulses of 600 ms.
    Timer {
        id: pulseTimer
        interval: 1200
        onTriggered: editor.pulseTarget = ""
    }

    ProfilePreviewFrame {
        id: preview
        visible: !editor.leaving
        x: editor.previewOnly ? 0 : rail.width + panelArea.width
        width: editor.width - x
        height: editor.height
        profiles: editor.profiles
        chatController: editor.chatController
        contactController: editor.contactController
        callController: editor.callController
        songPlayer: editor.songPlayer
        editingTarget: panel.editingTarget
        pulseTarget: editor.pulseTarget
        onEditRequested: target => editor.handleEditRequest(target)
    }

    Rectangle {
        // Under the rail and panel while they slide in: the preview has
        // already moved over, so the strip they cross is the panel's surface.
        visible: editor.slide > 0 && !editor.previewOnly && !editor.leaving
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
                // "Saving…" until the import lands. That save ends editing and
                // takes this editor with it, so the page, which stays, goes on.
                if (proceed && editor.page && typeof editor.page.leaveAfterPublish === "function")
                    editor.page.leaveAfterPublish(proceed);
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
