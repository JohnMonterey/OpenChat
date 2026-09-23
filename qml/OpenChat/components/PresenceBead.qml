import QtQuick
import OpenChat
import OpenChat.Native

// One presence, as a glossy Aero bead: green Available (0), amber Away (1),
// grey Offline (2), red Busy (3). An equipped bead style (a catalogue id such
// as "bead.gem") swaps the material but keeps the state colour.
Item {
    id: bead
    property int presence: 0
    property int beadSize: 12
    property string styleId: ""
    readonly property bool styled: styleId.length > 0

    readonly property color rimColor: presence === 0 ? "#4e9f0f"
                               : presence === 1 ? "#d69a0c"
                               : presence === 3 ? "#b8392c" : "#9ba7b2"
    readonly property color topColor: presence === 0 ? "#9be146"
                               : presence === 1 ? "#ffe15b"
                               : presence === 3 ? "#ff9d8c" : "#f0f2f3"
    readonly property color midColor: presence === 0 ? "#68c821"
                               : presence === 1 ? "#ffc92c"
                               : presence === 3 ? "#ec5a45" : "#d7dde1"
    readonly property color bottomColor: presence === 0 ? "#45ad0b"
                                  : presence === 1 ? "#eaa70a"
                                  : presence === 3 ? "#c93a28" : "#bac3ca"

    implicitWidth: beadSize
    implicitHeight: beadSize

    Rectangle {
        anchors.fill: parent
        visible: !bead.styled
        radius: width / 2
        border.width: 1
        border.color: bead.rimColor
        gradient: Gradient {
            GradientStop { position: 0; color: bead.topColor }
            GradientStop { position: 0.52; color: bead.midColor }
            GradientStop { position: 1; color: bead.bottomColor }
        }
    }

    Rectangle {
        visible: !bead.styled
        x: 2
        y: 2
        width: Math.max(2, parent.width - 5)
        height: Math.max(1, parent.height / 3)
        radius: height / 2
        color: Theme.gloss
    }

    // Centred on the bead; glows and rings reach past it by the overhang.
    Loader {
        objectName: "beadArtLoader"
        active: bead.styled
        anchors.centerIn: parent
        sourceComponent: BeadArt {
            objectName: "beadArt"
            styleId: bead.styleId
            presence: bead.presence
            beadSize: bead.beadSize
            darkMode: Theme.darkMode
            width: bead.beadSize + 2 * overhang
            height: width
        }
    }
}
