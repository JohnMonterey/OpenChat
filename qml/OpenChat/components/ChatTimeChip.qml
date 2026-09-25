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

    Text {
        id: label
        width: chip.width
        horizontalAlignment: Text.AlignHCenter
        // Digits and capitals have no descenders, so their middle, not the
        // line's, goes in the middle of the chip: the baseline sits half a
        // cap height (about 0.36 em) below it, on a whole device pixel.
        anchors.baseline: chip.top
        anchors.baselineOffset: Math.round((chip.height / 2 + 0.36 * font.pixelSize) * Screen.devicePixelRatio)
                                / Screen.devicePixelRatio
        text: chip.text
        textFormat: Text.PlainText
        color: "#ffffff"
        font.family: Theme.uiFont
        font.pixelSize: 11
        renderType: Text.NativeRendering
    }
}
