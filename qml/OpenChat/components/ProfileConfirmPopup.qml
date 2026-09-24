import QtQuick
import QtQuick.Controls.Basic
import OpenChat
import OpenChat.Native

// "Block @grace?" (SPEC §13), in the app's modal pattern (DailyCaseModal): a
// scrim, a small card, and Cancel as the default with the initial focus, so
// Enter and Esc both leave everything as it was. Only Block blocks.
Popup {
    id: popup
    objectName: "profileConfirmPopup"
    property string title: ""
    property string body: ""
    property string confirmLabel: "Block"
    property Item returnFocus: null
    signal confirmed()

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(400, parent ? parent.width - 32 : 400)
    padding: 22
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    Overlay.modal: Rectangle { color: Theme.dialogScrim }

    enter: Transition {
        NumberAnimation {
            property: "opacity"; from: 0; to: 1
            duration: ProfileRenderPolicy.animationsAllowed ? 120 : 0
        }
    }

    background: Rectangle {
        color: Theme.contentBackground
        border.color: Theme.inputBorder
        radius: 8
        Rectangle { x: 8; y: 1; width: parent.width - 16; height: 1; color: Theme.glossStrong }
        Rectangle { x: 8; y: parent.height - 2; width: parent.width - 16; height: 1; color: Theme.gloss }
    }

    onOpened: cancelButton.forceActiveFocus()
    onClosed: if (popup.returnFocus) popup.returnFocus.forceActiveFocus()

    contentItem: Column {
        spacing: 10
        Text {
            width: parent.width
            wrapMode: Text.Wrap
            text: popup.title
            textFormat: Text.PlainText
            color: Theme.textPrimary
            font.family: Theme.uiFont
            font.pixelSize: 17
            renderType: Text.NativeRendering
        }
        Text {
            width: parent.width
            wrapMode: Text.Wrap
            text: popup.body
            textFormat: Text.PlainText
            color: Theme.textSecondaryStrong
            font.family: Theme.uiFont
            font.pixelSize: 13
            lineHeight: 1.12
            renderType: Text.NativeRendering
        }
        Item {
            width: parent.width
            height: 8
        }
        Row {
            anchors.right: parent.right
            spacing: 8
            ProfileChipButton {
                id: blockButton
                objectName: "profileConfirmButton"
                label: popup.confirmLabel
                onClicked: {
                    popup.close();
                    popup.confirmed();
                }
            }
            ProfileDefaultButton {
                id: cancelButton
                objectName: "profileConfirmCancel"
                label: "Cancel"
                width: Math.max(84, implicitWidth)
                onClicked: popup.close()
            }
        }
    }
}
