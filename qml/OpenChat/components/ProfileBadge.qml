import QtQuick
import OpenChat

// The round profile badge a clickable picture wears on its bottom-right
// corner while it is hovered (SPEC §12): the switch-knob blue orb with an
// accentOrbRim, a white gloss cap over its top 42% and a white ID-card glyph
// at 62% of its size. 18 px on small pictures, 22 px from 68 px up, 20 px on
// Top Friend tiles.
Rectangle {
    id: badge
    property int size: 18

    width: size
    height: size
    radius: size / 2
    border.width: 1
    border.color: Theme.accentOrbRim
    gradient: Gradient {
        GradientStop { position: 0; color: Theme.switchTop }
        GradientStop { position: 1; color: Theme.switchBottom }
    }
    Accessible.ignored: true

    Rectangle {
        x: 3
        y: 1.5
        width: parent.width - 6
        height: parent.height * 0.42
        radius: height / 2
        color: "#55ffffff"
    }
    ProfileGlyph {
        anchors.centerIn: parent
        width: Math.round(badge.size * 0.62)
        height: width
        kind: "idCard"
        ink: "white"
        stroke: 1.2
    }
}
