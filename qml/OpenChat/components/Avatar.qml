import QtQuick

Item {
    id: avatar
    property string avatarKey: "neutral"
    property real cornerRadius: 5
    readonly property bool knownArtwork: avatarKey === "landscape"
                                         || avatarKey === "beach"
                                         || avatarKey === "mono"
                                         || avatarKey === "sarah"
                                         || avatarKey === "jessica"
                                         || avatarKey === "alex"

    implicitWidth: 44
    implicitHeight: 44
    clip: true

    Rectangle {
        anchors.fill: parent
        radius: avatar.cornerRadius
        border.width: 1
        border.color: "#b2c3cf"
        gradient: Gradient {
            GradientStop {
                position: 0
                color: avatar.avatarKey === "landscape" ? "#79b9ed"
                     : avatar.avatarKey === "beach" ? "#86d8ee"
                     : avatar.avatarKey === "mono" || avatar.avatarKey === "alex" ? "#d5d5d5"
                     : "#e9c9ae"
            }
            GradientStop {
                position: 1
                color: avatar.avatarKey === "landscape" ? "#d9edfb"
                     : avatar.avatarKey === "beach" ? "#f5df9d"
                     : avatar.avatarKey === "mono" || avatar.avatarKey === "alex" ? "#8b8b8b"
                     : "#a86e4c"
            }
        }
    }

    Item {
        anchors.fill: parent
        visible: avatar.avatarKey === "landscape" || avatar.avatarKey === "beach" || avatar.avatarKey === "mono"

        Rectangle {
            x: 0
            y: avatar.avatarKey === "beach" ? parent.height * 0.57 : parent.height * 0.66
            width: parent.width
            height: parent.height
            color: avatar.avatarKey === "beach" ? "#62c2cf"
                 : avatar.avatarKey === "mono" ? "#626262" : "#506554"
        }
        Rectangle {
            visible: avatar.avatarKey === "landscape"
            x: -6; y: parent.height * 0.60; width: parent.width * 0.72; height: 16
            rotation: -8; radius: 8; color: "#3f5548"
        }
        Rectangle {
            visible: avatar.avatarKey === "beach"
            x: 0; y: parent.height * 0.76; width: parent.width; height: parent.height
            color: "#f0d18c"
        }
    }

    Item {
        anchors.fill: parent
        visible: avatar.avatarKey === "sarah" || avatar.avatarKey === "jessica" || avatar.avatarKey === "alex"

        Rectangle {
            x: parent.width * 0.20
            y: parent.height * 0.14
            width: parent.width * 0.60
            height: parent.height * 0.62
            radius: width / 2
            color: avatar.avatarKey === "alex" ? "#3e3e3e" : "#704a34"
        }
        Rectangle {
            x: parent.width * 0.29
            y: parent.height * 0.18
            width: parent.width * 0.43
            height: parent.height * 0.47
            radius: width / 2
            color: avatar.avatarKey === "alex" ? "#d2d2d2" : "#f1c6a7"
        }
        Rectangle {
            x: parent.width * 0.14
            y: parent.height * 0.68
            width: parent.width * 0.72
            height: parent.height * 0.42
            radius: width / 2
            color: avatar.avatarKey === "alex" ? "#353535"
                 : avatar.avatarKey === "jessica" ? "#6ba3a5" : "#e3a58a"
        }
    }

    Item {
        objectName: "neutralAvatarFallback"
        anchors.fill: parent
        visible: !avatar.knownArtwork

        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            y: parent.height * 0.18
            width: parent.width * 0.38
            height: width
            radius: width / 2
            color: "#7f8c97"
        }
        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            y: parent.height * 0.56
            width: parent.width * 0.70
            height: parent.height * 0.42
            radius: width / 2
            color: "#6e7b86"
        }
    }

    Rectangle {
        anchors.fill: parent
        radius: avatar.cornerRadius
        color: "transparent"
        border.width: 1
        border.color: "#8fa8b8"
    }
}
