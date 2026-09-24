import QtQuick
import QtQuick.Controls
import OpenChat

// The editor's 300 px panel (SPEC §14.3): contentBackground with a right rule,
// a Flickable with the house 6 px scrollbar holding the chosen tab, and the
// focused control kept in view as Tab moves through it (and a text area's
// cursor while it grows). A surviving draft is announced at the top
// ("You have unsaved changes from 3:12 PM." [Continue] [Start over]), and a
// tab may put a fixed footer under its scrolling content (Themes does).
//
// Each tab is an Item with the panel's `profiles` and `editor`, an
// implicitHeight, and optionally `popupOpen` (a menu of its own is open),
// `editingTarget` (the preview target its focused field edits),
// `focusField(name)` (the preview asked for a field) and `footer`.
Rectangle {
    id: panel
    objectName: "profileEditorPanel"

    property var profiles: null
    property Item editor: null
    property string tab: "themes"

    readonly property Item tabItem: tabLoader.item
    readonly property bool popupOpen: tabItem !== null && tabItem.popupOpen === true
    readonly property string editingTarget: tabItem !== null && typeof tabItem.editingTarget === "string"
                                            ? tabItem.editingTarget : ""
    readonly property var components: ({
        themes: themesTab, background: backgroundTab, boxes: boxesTab, text: textTab, name: nameTab,
        about: aboutTab, friends: friendsTab, song: songTab, layout: layoutTab
    })
    // A field the preview asked for, focused once its tab has loaded.
    property string pendingField: ""

    function focusField(field) {
        pendingField = field;
        if (tabItem)
            Qt.callLater(applyPendingField);
    }
    function applyPendingField() {
        const field = pendingField;
        pendingField = "";
        if (!tabItem)
            return;
        if (field.length > 0 && typeof tabItem.focusField === "function")
            tabItem.focusField(field);
        else
            focusFirst();
    }
    function focusFirst() {
        const first = flick.contentItem.nextItemInFocusChain(true);
        if (first && isInside(first))
            first.forceActiveFocus(Qt.TabFocusReason);
        else
            flick.forceActiveFocus(Qt.TabFocusReason);
    }
    function isInside(item) {
        for (let at = item; at; at = at.parent) {
            if (at === panel)
                return true;
        }
        return false;
    }
    // Scrolls just enough to show `rect` (in the flickable's content).
    function reveal(top, bottom) {
        const margin = 12;
        if (top - margin < flick.contentY)
            flick.contentY = Math.max(0, top - margin);
        else if (bottom + margin > flick.contentY + flick.height)
            flick.contentY = Math.min(Math.max(0, flick.contentHeight - flick.height), bottom + margin - flick.height);
    }
    function revealFocus() {
        const item = panel.Window.activeFocusItem;
        if (!item || !isInside(item) || item === flick)
            return;
        let top = item.mapToItem(flick.contentItem, 0, 0).y;
        let bottom = top + item.height;
        // A growing text area: follow its cursor rather than its top edge.
        if (item.cursorRectangle !== undefined && item.height > flick.height / 2) {
            const cursor = item.mapToItem(flick.contentItem, item.cursorRectangle.x, item.cursorRectangle.y);
            top = cursor.y;
            bottom = cursor.y + item.cursorRectangle.height;
        }
        reveal(top, bottom);
    }

    color: Theme.contentBackground
    clip: true
    onTabChanged: flick.contentY = 0

    Connections {
        target: panel.Window.window
        function onActiveFocusItemChanged() { panel.revealFocus(); }
    }
    Connections {
        target: panel.Window.activeFocusItem
        ignoreUnknownSignals: true
        function onCursorRectangleChanged() { panel.revealFocus(); }
    }

    Rectangle {
        id: survivingDraft
        objectName: "profileSurvivingDraftNotice"
        readonly property real atMs: panel.profiles ? panel.profiles.survivingDraftAtMs : 0
        visible: atMs > 0
        x: 16
        y: visible ? 12 : 0
        width: parent.width - 33
        height: visible ? noticeColumn.implicitHeight + 20 : 0
        radius: 5
        color: Theme.warningBackground
        border.width: 1
        border.color: Theme.warningBorder
        Accessible.role: Accessible.AlertMessage
        Accessible.name: noticeText.text

        Column {
            id: noticeColumn
            x: 10
            y: 10
            width: parent.width - 20
            spacing: 8
            Text {
                id: noticeText
                width: parent.width
                wrapMode: Text.Wrap
                text: "You have unsaved changes from "
                      + new Date(survivingDraft.atMs).toLocaleTimeString(Qt.locale(), Locale.ShortFormat) + "."
                color: Theme.warningText
                font.family: Theme.uiFont
                font.pixelSize: 13
                renderType: Text.NativeRendering
            }
            Row {
                spacing: 8
                ProfileEditorRail.Button {
                    objectName: "profileContinueDraftButton"
                    height: 28
                    label: "Continue"
                    fontPixelSize: 13
                    onClicked: panel.profiles.continueDraft()
                }
                ProfileEditorRail.Button {
                    objectName: "profileStartOverButton"
                    height: 28
                    label: "Start over"
                    fontPixelSize: 13
                    onClicked: panel.profiles.startOver()
                }
            }
        }
    }

    Flickable {
        id: flick
        objectName: "profileEditorPanelFlickable"
        y: survivingDraft.visible ? survivingDraft.y + survivingDraft.height : 0
        width: parent.width - 1
        height: footerLoader.y - y
        contentWidth: width
        contentHeight: tabLoader.height + 24
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        activeFocusOnTab: false
        Keys.onPressed: event => {
            if (event.key === Qt.Key_PageDown) {
                panel.reveal(contentY + height, contentY + 2 * height);
                event.accepted = true;
            } else if (event.key === Qt.Key_PageUp) {
                contentY = Math.max(0, contentY - height);
                event.accepted = true;
            }
        }

        ScrollBar.vertical: ScrollBar {
            id: scrollBar
            objectName: "profileEditorPanelScrollBar"
            parent: panel
            x: panel.width - 10
            y: flick.y
            height: flick.height
            padding: 0
            minimumSize: 0.1
            policy: ScrollBar.AsNeeded
            background: Item {}
            contentItem: Rectangle {
                implicitWidth: 6
                radius: 3
                color: scrollBar.pressed || scrollBar.hovered ? Theme.iconHover : Theme.inputBorder
                visible: scrollBar.size < 1.0
            }
        }

        Loader {
            id: tabLoader
            width: flick.width
            height: item ? item.implicitHeight : 0
            sourceComponent: panel.components[panel.tab] || null
            onLoaded: {
                if (panel.pendingField.length > 0)
                    Qt.callLater(panel.applyPendingField);
            }
        }
    }

    Loader {
        id: footerLoader
        objectName: "profileEditorPanelFooter"
        y: parent.height - height
        width: parent.width - 1
        height: item ? item.implicitHeight : 0
        sourceComponent: panel.tabItem && panel.tabItem.footer ? panel.tabItem.footer : null
    }

    Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.rule }

    Component { id: themesTab; ProfileThemesTab { profiles: panel.profiles; editor: panel.editor } }
    Component { id: backgroundTab; ProfileBackgroundTab { profiles: panel.profiles; editor: panel.editor } }
    Component { id: boxesTab; ProfileBoxesTab { profiles: panel.profiles; editor: panel.editor } }
    Component { id: textTab; ProfileTextTab { profiles: panel.profiles; editor: panel.editor } }
    Component { id: nameTab; ProfileNameTab { profiles: panel.profiles; editor: panel.editor } }
    Component { id: aboutTab; ProfileAboutTab { profiles: panel.profiles; editor: panel.editor } }
    Component { id: friendsTab; ProfileFriendsTab { profiles: panel.profiles; editor: panel.editor } }
    Component { id: songTab; ProfileSongTab { profiles: panel.profiles; editor: panel.editor } }
    Component { id: layoutTab; ProfileLayoutTab { profiles: panel.profiles; editor: panel.editor } }
}
