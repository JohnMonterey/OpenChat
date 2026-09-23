import QtQuick
import OpenChat
import OpenChat.Native

Item {
    id: avatar
    property string avatarKey: "neutral"
    property real cornerRadius: 5
    readonly property bool knownArtwork: avatarKey === "landscape" || avatarKey === "beach" || avatarKey === "mono" || avatarKey === "sarah" || avatarKey === "jessica" || avatarKey === "alex" || avatarKey === "michael" || avatarKey === "ryan" || avatarKey === "userpfp_none" || avatarKey === "group" || avatarKey.startsWith("blob:")
    readonly property bool usesRoundedArtworkMask: true
    // An equipped avatar frame (a catalogue id such as "frame.aero"), drawn
    // round the picture and reaching a few pixels past it. Empty: no frame,
    // and nothing extra is created.
    property string frameId: ""
    // Animated frames tick while shown; the gallery holds them still.
    property bool frameAnimated: true
    readonly property alias frameItem: frameLoader.item

    implicitWidth: 44
    implicitHeight: 44

    AvatarArtwork {
        objectName: "roundedAvatarArtwork"
        anchors.fill: parent
        avatarKey: avatar.avatarKey
        cornerRadius: avatar.cornerRadius
    }

    Item {
        objectName: "neutralAvatarFallback"
        visible: !avatar.knownArtwork
    }

    Rectangle {
        anchors.fill: parent
        radius: avatar.cornerRadius
        color: "transparent"
        border.width: 1
        border.color: Theme.avatarBorder
    }

    Loader {
        id: frameLoader
        objectName: "avatarFrameLoader"
        active: avatar.frameId.length > 0
        sourceComponent: AvatarFrame {
            objectName: "avatarFrame"
            frameId: avatar.frameId
            avatarSize: avatar.width
            cornerRadius: avatar.cornerRadius
            darkMode: Theme.darkMode
            animate: avatar.frameAnimated
            x: -insetLeft
            y: -insetTop
            width: avatar.width + insetLeft + insetRight
            height: avatar.height + insetTop + insetBottom
        }
    }
}
