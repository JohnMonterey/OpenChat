import QtQuick
import QtQuick.Controls
import OpenChat

// The bar's Discard (SPEC §14.12): a small popover anchored under the button,
// "Discard all changes since your last save?" [Discard] [Keep editing]. Keep
// editing is the default; Esc or a click elsewhere keeps editing too.
Popup {
    id: popover
    objectName: "profileDiscardPopover"

    // The button it hangs from.
    property Item anchorItem: null
    signal discardChosen()

    parent: Overlay.overlay
    x: {
        if (!anchorItem || !parent)
            return 0;
        const right = anchorItem.mapToItem(parent, anchorItem.width, 0).x;
        return Math.max(8, Math.min(parent.width - width - 8, right - width));
    }
    y: anchorItem && parent ? anchorItem.mapToItem(parent, 0, anchorItem.height).y + 8 : 0
    width: 272
    padding: 0
    modal: false
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    onOpened: keepButton.forceActiveFocus(Qt.PopupFocusReason)

    background: Item {
        Rectangle {
            y: 2
            width: parent.width
            height: parent.height
            radius: 6
            color: Theme.tooltipShadowFill
        }
        Rectangle {
            anchors.fill: parent
            radius: 6
            border.width: 1
            border.color: Theme.tooltipBorder
            gradient: Gradient {
                GradientStop { position: 0; color: Theme.tooltipTop }
                GradientStop { position: 0.48; color: Theme.tooltipMid }
                GradientStop { position: 1; color: Theme.tooltipBottom }
            }
            Rectangle { x: 6; y: 1; width: parent.width - 12; height: 1; color: Theme.tooltipHighlight }
        }
        // The nose, under the middle of the button.
        Item {
            x: popover.anchorItem && popover.parent
               ? popover.anchorItem.mapToItem(popover.parent, popover.anchorItem.width / 2, 0).x - popover.x - 8
               : 20
            y: -7
            width: 16
            height: 8
            clip: true
            Rectangle {
                x: 2
                y: 3
                width: 11
                height: 11
                rotation: 45
                color: Theme.tooltipTop
                border.width: 1
                border.color: Theme.tooltipBorder
            }
        }
    }

    contentItem: FocusScope {
        implicitHeight: content.implicitHeight + 28
        Accessible.role: Accessible.Dialog
        Accessible.name: "Discard all changes since your last save?"

        Column {
            id: content
            x: 14
            y: 14
            width: parent.width - 28
            spacing: 12
            Text {
                width: parent.width
                wrapMode: Text.Wrap
                text: "Discard all changes since your last save?"
                color: Theme.textPrimary
                font.family: Theme.uiFont
                font.pixelSize: 13
                renderType: Text.NativeRendering
            }
            Row {
                anchors.right: parent.right
                spacing: 8
                ProfileEditorRail.Button {
                    objectName: "profileDiscardConfirmButton"
                    height: 30
                    width: 84
                    label: "Discard"
                    fontPixelSize: 13
                    labelColor: Theme.errorText
                    onClicked: {
                        popover.close();
                        popover.discardChosen();
                    }
                }
                ProfileEditorRail.DefaultButton {
                    id: keepButton
                    objectName: "profileDiscardKeepButton"
                    height: 30
                    width: 112
                    label: "Keep editing"
                    fontPixelSize: 13
                    onClicked: popover.close()
                }
            }
        }
    }
}
