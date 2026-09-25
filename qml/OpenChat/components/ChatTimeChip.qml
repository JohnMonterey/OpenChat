import QtQuick
import OpenChat

// A small dark glass chip with light text, for what rides on a picture in a
// bubble: the time a caption-less photo or video was sent, a video's length.
Rectangle {
    id: chip
    property string text: ""

    width: label.implicitWidth + 12
    height: label.implicitHeight + 3
    radius: height / 2
    color: "#8c101820"
    Accessible.ignored: true

    Text {
        id: label
        anchors.centerIn: parent
        text: chip.text
        textFormat: Text.PlainText
        color: "#ffffff"
        font.family: Theme.uiFont
        font.pixelSize: 11
        renderType: Text.NativeRendering
    }
}
