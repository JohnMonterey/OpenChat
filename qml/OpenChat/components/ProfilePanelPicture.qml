import QtQuick
import OpenChat
import OpenChat.Native

// One picture of a panel's picture block: the tile in its frame (plain,
// rounded, a tilted Polaroid card with the caption written on it, or a
// circle), the caption under it, or a quiet placeholder while the picture
// is still on its way. Activating it (click, Enter, Space) opens it large.
Item {
    id: tile
    // {key, mediaKey, present, width, height, caption} (ProfilePageObject.block)
    property var entry: ({})
    property int frame: Profile.RoundedFrame
    property bool stack: false  // full width at its own shape
    property bool strip: false  // a fixed-height tile in a sideways row
    property bool single: false // the only picture: its own shape
    property real tileWidth: 120
    property real tilt: 0
    property Item panelBlock: null
    signal activated()

    readonly property bool polaroid: frame === Profile.PolaroidFrame
    readonly property bool circle: frame === Profile.CircleFrame
    readonly property real aspect: entry.width > 0 && entry.height > 0 ? entry.width / entry.height : 1
    readonly property real cardPad: polaroid ? 6 : 0
    readonly property real cardBottom: polaroid ? (captionText.text.length > 0 ? captionText.implicitHeight + 12 : 18) : 0
    readonly property real pictureWidth: tileWidth - 2 * cardPad
    readonly property real pictureHeight: strip ? 150 - cardPad - cardBottom
                                          : circle || (!stack && !single) ? pictureWidth
                                          : Math.max(pictureWidth * 0.4, Math.min(pictureWidth * 1.3, pictureWidth / aspect))
    readonly property string caption: entry.caption || ""

    width: tileWidth
    height: card.height + (below.visible ? below.height + 4 : 0)
    activeFocusOnTab: true
    Accessible.role: Accessible.Button
    Accessible.name: caption.length > 0 ? "Picture: " + caption : "Picture"
    Keys.onReturnPressed: tile.activated()
    Keys.onEnterPressed: tile.activated()
    Keys.onSpacePressed: tile.activated()

    Rectangle {
        // The Polaroid's soft shadow.
        visible: tile.polaroid
        x: card.x + 2
        y: card.y + 3
        width: card.width
        height: card.height
        rotation: card.rotation
        radius: 2
        color: "#30000000"
    }
    Rectangle {
        id: card
        width: tile.tileWidth
        height: tile.cardPad + tile.pictureHeight + tile.cardBottom
        rotation: tile.tilt
        antialiasing: true
        radius: tile.polaroid ? 2 : 0
        color: tile.polaroid ? "#fbfbf7" : "transparent"
        border.width: tile.polaroid ? 1 : 0
        border.color: "#1a000000"

        Item {
            id: picture
            x: tile.cardPad
            y: tile.cardPad
            width: tile.pictureWidth
            height: tile.pictureHeight
            readonly property real radius: tile.frame === Profile.RoundedFrame ? 8 : tile.polaroid ? 1 : 0

            Rectangle {
                visible: !image.ready
                anchors.fill: parent
                radius: tile.circle ? width / 2 : picture.radius
                color: Qt.rgba(0.5, 0.55, 0.62, 0.14)
                border.width: 1
                border.color: Qt.rgba(0.5, 0.55, 0.62, 0.3)
                ProfileGlyph {
                    anchors.centerIn: parent
                    width: Math.min(28, parent.width / 3)
                    height: width
                    kind: "image"
                    ink: "#8a96a8"
                }
            }
            ProfilePanelImage {
                id: image
                objectName: "profilePanelPicture"
                anchors.fill: parent
                mediaKey: tile.entry.mediaKey || ""
                crop: true
                circle: tile.circle
                radius: picture.radius
            }
        }
        Text {
            id: captionText
            visible: tile.polaroid && text.length > 0
            x: 8
            y: tile.cardPad + tile.pictureHeight + 5
            width: parent.width - 16
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            maximumLineCount: 2
            elide: Text.ElideRight
            textFormat: Text.PlainText
            text: tile.caption
            color: "#33302a"
            font.family: tile.panelBlock ? tile.panelBlock.bodyFamily : Theme.uiFont
            font.pixelSize: 12
            renderType: Text.NativeRendering
        }
    }
    Text {
        id: below
        visible: !tile.polaroid && tile.caption.length > 0
        y: card.height + 4
        width: tile.width
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
        text: tile.caption
        color: tile.panelBlock ? tile.panelBlock.mutedInk : "gray"
        style: tile.panelBlock && tile.panelBlock.halo ? Text.Outline : Text.Normal
        styleColor: tile.panelBlock ? tile.panelBlock.haloColor : "transparent"
        font.family: tile.panelBlock ? tile.panelBlock.bodyFamily : Theme.uiFont
        font.pixelSize: tile.panelBlock ? tile.panelBlock.captionSize + 1 : 12
        renderType: Text.NativeRendering
    }
    ProfileFocusRing {
        x: card.x
        y: card.y
        width: card.width
        height: card.height
        rotation: card.rotation
        radius: tile.circle ? card.width / 2 : 4
        shown: tile.activeFocus
        onDark: tile.panelBlock !== null && tile.panelBlock.render !== null && tile.panelBlock.render.boxDark
    }
    MouseArea {
        anchors.fill: card
        cursorShape: Qt.PointingHandCursor
        onClicked: tile.activated()
    }
}
