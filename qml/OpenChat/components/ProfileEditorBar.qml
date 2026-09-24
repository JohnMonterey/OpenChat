import QtQuick
import QtQuick.Controls
import OpenChat

// The top bar in edit mode (SPEC §2): the app's own 48 px chrome with
// ‹ Profile, "Edit profile" and the Unsaved changes pill on the left, and
// Undo · Redo | Preview · Discard · Save on the right. Below 900 px the pill
// shrinks to its dot and Preview drops its label (`final-editor-min.png`).
//
// ProfileTopBar hosts it while the owner edits. Back leaves through the
// editor (the leave dialog when there are unsaved changes), as does picking
// an entry from its history menu. Preview hides the editor's rail and panel.
//
// The host may set `page`, `profiles` and `editor`; whatever it leaves unset
// is found from the ProfilePage around the bar (objectName "profilePage", its
// `profiles`) and the ProfileEditor inside that page (objectName
// "profileEditor"), so the bar works however the page kit loads it.
Item {
    id: bar
    objectName: "profileEditorBar"

    property Item page: null
    property var profiles: page ? page.profiles : null
    property Item editor: null

    readonly property bool compact: width < 900
    readonly property bool dirty: profiles !== null && profiles.draftDirty
    readonly property bool popupOpen: discardPopover.opened || historyMenu.opened

    function findAncestor(name) {
        for (let item = parent; item; item = item.parent) {
            if (item.objectName === name)
                return item;
        }
        return null;
    }
    function findDescendant(root, name) {
        if (!root)
            return null;
        for (let i = 0; i < root.children.length; ++i) {
            const child = root.children[i];
            if (child.objectName === name)
                return child;
            const found = findDescendant(child, name);
            if (found)
                return found;
        }
        return null;
    }
    // The page and the editor may be created in either order when editing
    // begins, so the editor is looked for again once both exist.
    function resolve() {
        if (!page)
            page = findAncestor("profilePage");
        if (!editor)
            editor = findDescendant(page, "profileEditor");
    }
    function leave(proceed) {
        if (editor)
            editor.requestLeave(proceed);
        else
            proceed();
    }
    function focusFirst() {
        backChip.forceActiveFocus(Qt.TabFocusReason);
    }

    height: 48
    Component.onCompleted: {
        resolve();
        if (!editor)
            Qt.callLater(resolve);
    }

    // The editor learns about the bar (for F6) and its popups (for popupOpen).
    Binding {
        target: bar.editor
        property: "bar"
        value: bar
        when: bar.editor !== null
    }
    Binding {
        target: bar.editor
        property: "barPopupOpen"
        value: bar.popupOpen
        when: bar.editor !== null
    }

    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0; color: Theme.headerTop }
            GradientStop { position: 1; color: Theme.headerBottom }
        }
    }
    Rectangle { width: parent.width; height: 1; color: Theme.gloss }
    Rectangle { y: parent.height - 1; width: parent.width; height: 1; color: Theme.rule }

    // ‹ Profile: back to your page. Right-click, Shift+F10 or Menu: the stack.
    ProfileEditorRail.Button {
        id: backChip
        objectName: "profileEditorBackButton"
        x: 10
        anchors.verticalCenter: parent.verticalCenter
        glyph: "back"
        glyphSize: 14
        label: bar.profiles ? bar.profiles.backLabel : "Profile"
        Accessible.name: "Back to your profile"
        onClicked: bar.leave(() => bar.profiles.endEditing())
        Keys.onPressed: event => {
            if (event.key === Qt.Key_Menu || (event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier))) {
                bar.openHistory();
                event.accepted = true;
            }
        }
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.RightButton
            onClicked: bar.openHistory()
        }
    }

    function openHistory() {
        if (profiles && profiles.history.length > 0)
            historyMenu.popup(backChip, 0, backChip.height + 4);
    }

    AeroMenu {
        id: historyMenu
        objectName: "profileEditorHistoryMenu"
        Instantiator {
            model: bar.profiles ? bar.profiles.history : []
            delegate: AeroMenuItem {
                required property var modelData
                text: modelData.name
                onTriggered: {
                    const index = modelData.index;
                    bar.leave(() => bar.profiles.popTo(index));
                }
            }
            onObjectAdded: (index, object) => historyMenu.insertItem(index, object)
            onObjectRemoved: (index, object) => historyMenu.removeItem(object)
        }
    }

    Rectangle {
        id: divider
        x: backChip.x + backChip.width + 11
        y: 12
        width: 1
        height: parent.height - 24
        color: Theme.rule
    }

    Row {
        x: divider.x + 14
        anchors.verticalCenter: parent.verticalCenter
        spacing: 12

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: "Edit profile"
            color: Theme.textPrimary
            font.family: Theme.uiFont
            font.pixelSize: 17
            renderType: Text.NativeRendering
            Accessible.role: Accessible.Heading
            Accessible.name: text
        }

        Rectangle {
            id: pill
            objectName: "profileUnsavedPill"
            visible: bar.dirty
            anchors.verticalCenter: parent.verticalCenter
            width: bar.compact ? 22 : pillRow.implicitWidth + 20
            height: 22
            radius: 11
            color: Theme.warningBackground
            border.width: 1
            border.color: Theme.warningBorder
            Accessible.role: Accessible.StaticText
            Accessible.name: "Unsaved changes. Only you can see these changes until you save."

            Row {
                id: pillRow
                anchors.centerIn: parent
                spacing: 6
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 7
                    height: 7
                    radius: 3.5
                    color: Theme.unsavedDot
                }
                Text {
                    objectName: "profileUnsavedPillText"
                    visible: !bar.compact
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Unsaved changes"
                    color: Theme.warningText
                    font.family: Theme.uiFont
                    font.pixelSize: 12
                    renderType: Text.NativeRendering
                }
            }
            MouseArea {
                id: pillMouse
                anchors.fill: parent
                hoverEnabled: true
            }
            ProfileEditorRail.Tip {
                objectName: "profileUnsavedPillTip"
                visible: pillMouse.containsMouse
                title: bar.compact ? "Unsaved changes" : ""
                text: "Only you can see these changes until you save."
            }
        }
    }

    Row {
        anchors.right: parent.right
        anchors.rightMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        spacing: 6

        ProfileEditorRail.Button {
            id: undoButton
            objectName: "profileUndoButton"
            width: 30
            glyph: "undo"
            glyphSize: 16
            enabled: bar.profiles !== null && bar.profiles.canUndo
            Accessible.name: "Undo"
            onClicked: bar.profiles.undo()
            ProfileEditorRail.Tip {
                visible: undoButton.hovered
                text: "Undo (Ctrl+Z)"
            }
        }
        ProfileEditorRail.Button {
            id: redoButton
            objectName: "profileRedoButton"
            width: 30
            glyph: "redo"
            glyphSize: 16
            enabled: bar.profiles !== null && bar.profiles.canRedo
            Accessible.name: "Redo"
            onClicked: bar.profiles.redo()
            ProfileEditorRail.Tip {
                visible: redoButton.hovered
                text: "Redo (Ctrl+Shift+Z)"
            }
        }
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: 1
            height: 22
            color: Theme.rule
        }
        ProfileEditorRail.Button {
            id: previewToggle
            objectName: "profilePreviewToggle"
            glyph: "eye"
            label: bar.compact ? "" : "Preview"
            fontPixelSize: 13
            width: bar.compact ? 32 : implicitWidth
            checkable: true
            checked: bar.editor !== null && bar.editor.previewOnly
            enabled: bar.editor !== null
            Accessible.name: "Preview"
            onClicked: bar.editor.previewOnly = !bar.editor.previewOnly
            ProfileEditorRail.Tip {
                visible: bar.compact && previewToggle.hovered
                text: previewToggle.checked ? "Show the editor again" : "Preview your page at full width"
            }
        }
        Item { width: 2; height: 1 }
        ProfileEditorRail.Button {
            id: discardButton
            objectName: "profileDiscardButton"
            width: 84
            label: "Discard"
            enabled: bar.dirty
            onClicked: discardPopover.open()
        }
        ProfileEditorRail.DefaultButton {
            objectName: "profileSaveButton"
            width: 84
            label: bar.profiles && bar.profiles.publishPending ? "Saving…" : "Save"
            enabled: bar.dirty && !(bar.profiles && bar.profiles.publishPending)
            onClicked: bar.profiles.publish()
        }
    }

    ProfileDiscardPopover {
        id: discardPopover
        anchorItem: discardButton
        onDiscardChosen: bar.profiles.discardChanges()
    }
}
