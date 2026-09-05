import QtQuick
import OpenChat

// A reusable Aero text frame: a rounded white field with the shared inset
// highlight and outline used across the app, carrying a single-line input, a muted
// placeholder, and an optional inline prefix (e.g. "@"). The caller sets its
// objectName and reads the entered value through the `text` alias.
Item {
    id: field
    property alias text: fieldInput.text
    property string placeholder: ""
    property string prefix: ""
    height: 38

    Rectangle {
        anchors.fill: parent
        radius: 5
        color: Theme.fieldBackground
    }

    Text {
        id: prefixLabel
        visible: field.prefix.length > 0
        anchors.left: parent.left
        anchors.leftMargin: 11
        anchors.verticalCenter: parent.verticalCenter
        text: field.prefix
        color: Theme.textSecondary
        font.family: Theme.uiFont
        font.pixelSize: 16
        renderType: Text.NativeRendering
    }

    TextInput {
        id: fieldInput
        anchors.left: prefixLabel.visible ? prefixLabel.right : parent.left
        anchors.leftMargin: prefixLabel.visible ? 1 : 12
        anchors.right: parent.right
        anchors.rightMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        color: Theme.textPrimary
        font.family: Theme.uiFont
        font.pixelSize: 16
        clip: true
        selectByMouse: true
        selectionColor: Theme.selectionBackground
        selectedTextColor: Theme.selectionText

        Text {
            anchors.fill: parent
            visible: !fieldInput.text && !fieldInput.activeFocus
            text: field.placeholder
            color: Theme.placeholderText
            font: fieldInput.font
            verticalAlignment: Text.AlignVCenter
            renderType: Text.NativeRendering
        }
    }

    // Aero inset: shadow along the inner top/left, a faint highlight along the
    // bottom, then the shared outline drawn last.
    Rectangle { x: 5; y: 1; width: parent.width - 10; height: 1; color: Theme.insetTop }
    Rectangle { x: 1; y: 5; width: 1; height: parent.height - 10; color: Theme.insetLeft }
    Rectangle { x: 5; y: parent.height - 2; width: parent.width - 10; height: 1; color: Theme.gloss }
    Rectangle {
        anchors.fill: parent
        z: 10
        radius: 5
        color: "transparent"
        border.width: 1
        border.color: Theme.inputBorder
    }
}
