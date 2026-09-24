import QtQuick
import OpenChat

// A Top Friend who is not the viewer's contact (SPEC §5.8): no picture ever
// travels, so the renderer draws a quiet tile tinted from the box and its
// border, a gloss over the top half, and the initials in the heading face's
// regular weight. It must never be louder than a real photo: pale glass,
// no bold, no white-on-colour. The colours are the renderer's (held at 3:1).
Item {
    id: tile
    property var render: null
    property string initials: "?"
    // Wide-column boxes use the alt border as the tint's accent.
    property bool alt: true
    property real radius: 4

    readonly property bool darkBox: render ? render.boxDark : false
    readonly property color tintTop: render ? (alt ? render.altMonogramTop : render.monogramTop) : "#e8f2fa"
    readonly property color tintBottom: render ? (alt ? render.altMonogramBottom : render.monogramBottom) : "#c4dcef"
    readonly property color tintRim: render ? (alt ? render.altMonogramRim : render.monogramRim) : "#a9c8e2"
    readonly property color initialsInk: render ? (alt ? render.altMonogramInk : render.monogramInk) : "#35618f"

    // The heading face's regular cut (the renderer's monogramFamily): a
    // monogram is never bold.
    readonly property string family: render && render.monogramFamily ? render.monogramFamily : Theme.uiFont

    Accessible.ignored: true

    Rectangle {
        anchors.fill: parent
        radius: tile.radius
        border.width: 1
        border.color: tile.tintRim
        gradient: Gradient {
            GradientStop { position: 0; color: tile.tintTop }
            GradientStop { position: 1; color: tile.tintBottom }
        }
    }
    Rectangle {
        x: 1
        y: 1
        width: parent.width - 2
        height: parent.height * 0.5
        radius: tile.radius
        gradient: Gradient {
            GradientStop { position: 0; color: tile.darkBox ? "#1cffffff" : "#80ffffff" }
            GradientStop { position: 1; color: "#00ffffff" }
        }
    }
    Text {
        objectName: "profileMonogramInitials"
        anchors.centerIn: parent
        anchors.verticalCenterOffset: tile.render ? 2 * tile.render.headingLift : 0
        text: tile.initials
        textFormat: Text.PlainText
        color: tile.initialsInk
        font.family: tile.family
        font.pixelSize: Math.max(8, Math.round(tile.height * 0.27 * (tile.render ? tile.render.headingFactor : 1)))
        font.weight: Font.Normal
        renderType: Text.NativeRendering
    }
}
