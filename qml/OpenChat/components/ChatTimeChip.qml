import QtQuick
import OpenChat

// A small dark glass chip with light text, for what rides on a picture in a
// bubble: the time a caption-less photo or video was sent, a video's length.
Rectangle {
    id: chip
    property string text: ""

    width: Math.ceil(label.implicitWidth) + 12
    height: label.implicitHeight + 3
    radius: height / 2
    color: "#8c101820"
    Accessible.ignored: true

    // Digits and capitals have no descenders, so the text is centred on its
    // cap height rather than on its line, which would sit it a pixel high.
    FontMetrics {
        id: metrics
        font: label.font
    }
    Text {
        id: label
        width: chip.width
        horizontalAlignment: Text.AlignHCenter
        y: Math.round((chip.height - metrics.capitalHeight) / 2 - (metrics.ascent - metrics.capitalHeight))
        text: chip.text
        textFormat: Text.PlainText
        color: "#ffffff"
        font.family: Theme.uiFont
        font.pixelSize: 11
        renderType: Text.NativeRendering
    }
}
