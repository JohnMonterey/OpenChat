import QtQuick

Item {
    id: bead
    property int presence: 0
    property int beadSize: 12

    implicitWidth: beadSize
    implicitHeight: beadSize

    Rectangle {
        anchors.fill: parent
        radius: width / 2
        border.width: 1
        border.color: bead.presence === 0 ? "#4e9f0f"
                    : bead.presence === 1 ? "#d69a0c" : "#9ba7b2"
        gradient: Gradient {
            GradientStop { position: 0; color: bead.presence === 0 ? "#9be146" : bead.presence === 1 ? "#ffe15b" : "#f0f2f3" }
            GradientStop { position: 0.52; color: bead.presence === 0 ? "#68c821" : bead.presence === 1 ? "#ffc92c" : "#d7dde1" }
            GradientStop { position: 1; color: bead.presence === 0 ? "#45ad0b" : bead.presence === 1 ? "#eaa70a" : "#bac3ca" }
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
