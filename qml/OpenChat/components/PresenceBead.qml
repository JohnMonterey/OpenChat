import QtQuick

// One presence, as a glossy Aero bead: green Available (0), amber Away (1),
// grey Offline (2), red Busy (3).
Item {
    id: bead
    property int presence: 0
    property int beadSize: 12

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
        x: 2
        y: 2
        width: Math.max(2, parent.width - 5)
        height: Math.max(1, parent.height / 3)
        radius: height / 2
        color: "#70ffffff"
    }
}
