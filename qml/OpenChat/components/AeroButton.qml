import QtQuick
import OpenChat

// A primary Aero button matching the composer's send button treatment: an
// enabled-aware palette with a hover lift, the shared border, and native text. The
// caller sets its objectName, label, and (optionally) a smaller font for compact
// rows.
Item {
    id: button
    property string label: ""
    property bool enabled: true
    property int fontPixelSize: 16
    signal clicked
    height: 44

    Rectangle {
        anchors.fill: parent
        radius: 4
        color: button.enabled ? (buttonMouse.containsMouse ? "#f8fbfd" : "#f5f8fa")
                               : "#eef2f5"
        border.width: 1
        border.color: button.enabled ? "#aebdca" : "#c5d0d9"
    }
    Text {
        anchors.centerIn: parent
        text: button.label
        color: button.enabled ? "#3f5570" : "#8b99aa"
        font.family: Theme.uiFont
        font.pixelSize: button.fontPixelSize
        renderType: Text.NativeRendering
    }
    MouseArea {
        id: buttonMouse
        anchors.fill: parent
        enabled: button.enabled
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: button.clicked()
    }
}
