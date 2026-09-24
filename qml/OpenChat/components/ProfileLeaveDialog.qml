import QtQuick
import QtQuick.Controls
import OpenChat

// Leaving the editor with unsaved changes (SPEC §14.12, `final-editor-leave.png`):
// a focus-trapping modal in the DailyCaseModal pattern. [Discard] on the left,
// [Save] and the default [Keep editing] on the right. Keep editing starts with
// focus, and Enter and Esc both mean it, whichever button has focus: the
// draft survives every choice except Discard.
Popup {
    id: dialog
    objectName: "profileLeaveDialog"

    signal saveChosen()
    signal discardChosen()
    signal keepEditingChosen()

    function keepEditing() {
        close();
        keepEditingChosen();
    }

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(400, parent ? parent.width - 32 : 400)
    padding: 0
    modal: true
    focus: true
    // Esc is handled below, so it can mean Keep editing like Enter.
    closePolicy: Popup.NoAutoClose
    Overlay.modal: Rectangle { color: Theme.dialogScrim }
    onOpened: keepButton.forceActiveFocus(Qt.PopupFocusReason)

    background: Rectangle {
        radius: 8
        color: Theme.contentBackground
        border.width: 1
        border.color: Theme.inputBorder
        Rectangle { x: 8; y: 1; width: parent.width - 16; height: 1; color: Theme.glossStrong }
        Rectangle { x: 8; y: parent.height - 2; width: parent.width - 16; height: 1; color: Theme.gloss }
    }

    contentItem: FocusScope {
        implicitHeight: column.implicitHeight + 42
        Accessible.role: Accessible.Dialog
        Accessible.name: "Save changes to your profile?"
        Keys.onEscapePressed: dialog.keepEditing()
        Keys.onReturnPressed: dialog.keepEditing()
        Keys.onEnterPressed: dialog.keepEditing()

        Column {
            id: column
            x: 24
            y: 22
            width: parent.width - 48
            spacing: 10

            Text {
                width: parent.width
                wrapMode: Text.Wrap
                text: "Save changes to your profile?"
                color: Theme.textPrimary
                font.family: Theme.uiFont
                font.pixelSize: 17
                renderType: Text.NativeRendering
            }
            Text {
                width: parent.width
                wrapMode: Text.Wrap
                text: "Your contacts still see the version you last saved. Only you can see these changes until you save."
                color: Theme.textSecondaryStrong
                font.family: Theme.uiFont
                font.pixelSize: 13
                lineHeight: 1.1
                renderType: Text.NativeRendering
            }
            Item { width: 1; height: 8 }
            Item {
                width: parent.width
                height: 32

                // Enter and Return are the dialog's (Keep editing) even on the
                // other two buttons, so they take Space and clicks only.
                ProfileEditorRail.Button {
                    objectName: "profileLeaveDiscardButton"
                    anchors.left: parent.left
                    width: 92
                    height: 32
                    label: "Discard"
                    returnActivates: false
                    onClicked: {
                        dialog.close();
                        dialog.discardChosen();
                    }
                }
                Row {
                    anchors.right: parent.right
                    spacing: 8
                    ProfileEditorRail.Button {
                        objectName: "profileLeaveSaveButton"
                        width: 84
                        height: 32
                        label: "Save"
                        returnActivates: false
                        onClicked: {
                            dialog.close();
                            dialog.saveChosen();
                        }
                    }
                    ProfileEditorRail.DefaultButton {
                        id: keepButton
                        objectName: "profileLeaveKeepButton"
                        width: 116
                        height: 32
                        label: "Keep editing"
                        onClicked: dialog.keepEditing()
                    }
                }
            }
        }
    }
}
