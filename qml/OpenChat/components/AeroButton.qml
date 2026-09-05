import QtQuick
import OpenChat

// A primary Aero button matching the composer's send button treatment: an
// enabled-aware palette with a hover lift, the shared border, and native text. The
// caller sets its objectName, label, and (optionally) a smaller font for compact
// rows.
Item {
    id: button
    property string label: ""
    property int fontPixelSize: 16
    signal clicked
    height: 44

    Rectangle {
        anchors.fill: parent
        radius: 4
        color: button.enabled ? (buttonMouse.containsMouse ? Theme.buttonHover : Theme.buttonBackground)
                               : Theme.buttonDisabled
        border.width: 1
        border.color: button.enabled ? Theme.buttonBorder : Theme.buttonDisabledBorder
    }
    Text {
        anchors.centerIn: parent
        text: button.label
        color: button.enabled ? Theme.buttonText : Theme.buttonDisabledText
        font.family: Theme.uiFont
        font.pixelSize: button.fontPixelSize
        renderType: Text.NativeRendering
    }
    MouseArea {
        id: buttonMouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: button.clicked()
    }
}
