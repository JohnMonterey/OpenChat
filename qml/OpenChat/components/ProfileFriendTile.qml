import QtQuick
import OpenChat

// One Top Friend (SPEC §5.8): the name above the picture, MySpace's order.
// A friend who is the viewer's own contact (or the viewer) shows the viewer's
// copy of their real picture under the viewer's own name for them; anyone else
// is a monogram under the owner's label, so a page can never put a chosen
// name under a recognisable face. Hover and keyboard focus wear the app-wide
// picture affordance with a 20 px badge, and the name is underlined.
Item {
    id: tile
    // {index, accountId, name, initials, avatarKey, isContact, isSelf}
    property var friend: ({})
    property var render: null
    property int size: 85
    property real pictureRadius: 4
    // The keyboard focus is on this tile (the grid moves it between tiles).
    property bool keyboardFocus: false
    signal activated()

    readonly property bool hovered: area.containsMouse
    readonly property bool hasPicture: (tile.friend.avatarKey || "").length > 0
    readonly property string name: tile.friend.name || ""

    width: size
    height: nameText.height + 4 + size

    Accessible.role: Accessible.Button
    Accessible.name: tile.hasPicture ? tile.name + ", open profile" : tile.name + ", not in your contacts"
    Accessible.onPressAction: tile.activated()

    Text {
        id: nameText
        objectName: "profileFriendName"
        width: tile.size
        horizontalAlignment: Text.AlignHCenter
        elide: Text.ElideRight
        text: tile.name
        textFormat: Text.PlainText
        color: tile.render ? tile.render.linkColor : "#1f6fa3"
        style: tile.render && tile.render.textHalo ? Text.Outline : Text.Normal
        styleColor: tile.render ? tile.render.haloColor : "transparent"
        font.family: tile.render && tile.render.bodyFamily.length > 0 ? tile.render.bodyFamily : Theme.uiFont
        font.pixelSize: 12
        font.underline: tile.hovered || tile.keyboardFocus
        renderType: Text.NativeRendering
    }

    Item {
        id: picture
        objectName: "profileFriendPicture"
        y: nameText.height + 4
        width: tile.size
        height: tile.size

        Avatar {
            visible: tile.hasPicture
            anchors.fill: parent
            avatarKey: tile.hasPicture ? tile.friend.avatarKey : "userpfp_none"
            cornerRadius: tile.pictureRadius
        }
        Rectangle {
            visible: tile.hasPicture
            anchors.fill: parent
            radius: tile.pictureRadius
            color: "transparent"
            border.width: 1
            border.color: tile.render && tile.render.boxDark ? "#40ffffff" : "#33000000"
        }
        ProfileMonogramTile {
            visible: !tile.hasPicture
            anchors.fill: parent
            render: tile.render
            initials: tile.friend.initials || "?"
            radius: tile.pictureRadius
        }
    }

    ProfileAvatarAffordance {
        target: picture
        interactive: false
        hovered: tile.hovered
        pressed: area.pressed
        focused: tile.keyboardFocus
        badgeSize: 20
        cornerRadius: tile.pictureRadius
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: tile.activated()
    }

    ProfileTip {
        parent: picture
        visible: !tile.hasPicture && tile.hovered
        text: "Not in your contacts"
        y: picture.height + 6
    }
}
