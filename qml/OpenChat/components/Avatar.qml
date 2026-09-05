import QtQuick
import OpenChat.Native

Item {
    id: avatar
    property string avatarKey: "neutral"
    property real cornerRadius: 5
    readonly property bool knownArtwork: avatarKey === "landscape" || avatarKey === "beach" || avatarKey === "mono" || avatarKey === "sarah" || avatarKey === "jessica" || avatarKey === "alex" || avatarKey === "michael" || avatarKey === "ryan" || avatarKey === "userpfp_none" || avatarKey.startsWith("blob:")
    readonly property bool usesRoundedArtworkMask: true

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
        border.color: "#8fa8b8"
    }
}
