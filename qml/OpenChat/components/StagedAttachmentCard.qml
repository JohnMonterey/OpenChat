import QtQuick
import QtQuick.Shapes
import OpenChat
import OpenChat.Native

// One staged attachment in the tray. A photo or a video that is ready (or on
// its way) is just its thumbnail, a video with a play badge and its length. A
// sound or a file, or a picture with something to say ("Only the first minute
// will be sent.", or why it could not be prepared), is a wider card: the
// thumbnail or the kind's chip, the name and one line under it. Until it is
// ready a thin bar along its foot fills up. The × takes it back out.
Item {
    id: card
    objectName: "stagedAttachmentCard"
    // The model's roles (StagedAttachmentModel).
    required property int index
    required property string stagedId
    required property int kind
    required property string name
    required property string sizeText
    required property real progress
    required property bool ready
    required property bool failed
    required property string error
    required property string notice
    required property string previewKey
    required property string durationText
    signal removeRequested(string stagedId)

    readonly property bool visual: kind === 1 || kind === 2
    readonly property bool thumbnail: visual && !failed && notice.length === 0
    readonly property bool preparing: !ready && !failed
    readonly property real motion: ProfileRenderPolicy.animationsAllowed ? 1 : 0
    readonly property string status: failed ? error
        : preparing ? "Preparing… " + Math.round(Math.max(0, Math.min(1, progress)) * 100) + "%"
        : notice.length > 0 ? notice
        : durationText.length > 0 ? sizeText + " · " + durationText : sizeText

    // A card with something to say is wider, so a notice fits on its line.
    width: thumbnail ? 60 : failed || notice.length > 0 ? 256 : 200
    height: 60
    Accessible.role: Accessible.ListItem
    Accessible.name: (kind === 1 ? "Photo" : kind === 2 ? "Video" : kind === 3 ? "Audio" : "File")
                     + (name.length > 0 ? ": " + name : "") + ", " + status

    // The wider card's face: a raised field.
    Rectangle {
        visible: !card.thumbnail
        anchors.fill: parent
        radius: 5
        border.width: 1
        border.color: card.failed ? Theme.noticeBorder : Theme.stagedCardBorder
        gradient: Gradient {
            GradientStop { position: 0; color: Theme.stagedCardTop }
            GradientStop { position: 1; color: Theme.stagedCardBottom }
        }
        Rectangle { x: 5; y: 1; width: parent.width - 10; height: 1; color: Theme.gloss }
    }

    // The picture: the whole card as a thumbnail, or a smaller square at the
    // card's left.
    Item {
        id: picture
        visible: card.visual
        x: card.thumbnail ? 0 : 8
        y: card.thumbnail ? 0 : 8
        width: card.thumbnail ? card.width : 44
        height: card.thumbnail ? card.height : 44

        Rectangle {
            anchors.fill: parent
            radius: 5
            color: Theme.mediaPlaceholder
        }
        ProfileGlyph {
            anchors.centerIn: parent
            visible: !image.ready
            width: Math.round(parent.width * 0.4)
            height: width
            kind: card.kind === 2 ? "film" : "image"
            ink: Theme.textSecondary
            opacity: 0.7
        }
        ProfilePanelImage {
            id: image
            objectName: "stagedAttachmentThumbnail"
            anchors.fill: parent
            mediaKey: card.previewKey
            crop: true
            radius: 5
        }
        // A video: a small play badge and its length.
        Rectangle {
            visible: card.kind === 2 && image.ready
            anchors.centerIn: parent
            width: 22
            height: 22
            radius: 11
            color: "#a0000000"
            border.width: 1
            border.color: "#d0ffffff"
            // Even like the badge, so it centres on whole pixels; play is
            // drawn optically centred already.
            ProfileGlyph {
                anchors.centerIn: parent
                width: 12
                height: 12
                kind: "play"
                ink: "white"
            }
        }
        Rectangle {
            visible: card.kind === 2 && card.thumbnail && card.durationText.length > 0 && !card.preparing
            x: 4
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 4
            width: Math.ceil(lengthText.implicitWidth) + 8
            height: lengthText.implicitHeight + 2
            radius: 3
            color: "#b0000000"
            // Centred on the digits' height, not the line's (see ChatTimeChip).
            FontMetrics {
                id: lengthMetrics
                font: lengthText.font
            }
            Text {
                id: lengthText
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                y: Math.round((parent.height - lengthMetrics.capitalHeight) / 2
                              - (lengthMetrics.ascent - lengthMetrics.capitalHeight))
                text: card.durationText
                textFormat: Text.PlainText
                color: "white"
                font.family: Theme.uiFont
                font.pixelSize: 10
                renderType: Text.NativeRendering
            }
        }
        Rectangle {
            anchors.fill: parent
            radius: 5
            color: "transparent"
            border.width: 1
            border.color: card.thumbnail ? Theme.stagedCardBorder : "#18000000"
        }
    }

    AttachmentChip {
        visible: !card.visual
        x: 12
        anchors.verticalCenter: parent.verticalCenter
        width: 34
        height: 34
        kind: card.kind
    }

    Text {
        id: title
        objectName: "stagedAttachmentName"
        visible: !card.thumbnail
        x: 58
        y: 12
        width: parent.width - x - 22
        text: card.name
        textFormat: Text.PlainText
        elide: Text.ElideMiddle
        color: Theme.textPrimary
        font.family: Theme.uiFont
        font.pixelSize: 13
        renderType: Text.NativeRendering
    }
    Text {
        objectName: "stagedAttachmentStatus"
        visible: !card.thumbnail
        x: title.x
        y: title.y + title.implicitHeight + 1
        width: parent.width - x - 10
        text: card.status
        textFormat: Text.PlainText
        elide: Text.ElideRight
        color: card.failed ? Theme.errorText : Theme.textSecondary
        font.family: Theme.uiFont
        font.pixelSize: 11
        renderType: Text.NativeRendering
    }

    // How far the preparation has come, along the foot: light over a
    // picture, the theme's blue elsewhere.
    Rectangle {
        objectName: "stagedAttachmentProgress"
        readonly property bool overPicture: card.thumbnail && image.ready
        visible: card.preparing
        x: card.thumbnail ? 6 : title.x
        y: parent.height - (card.thumbnail ? 8 : 10)
        width: parent.width - x - (card.thumbnail ? 6 : 12)
        height: 3
        radius: 1.5
        color: overPicture ? "#70000000" : Theme.progressTrack
        Rectangle {
            width: Math.max(parent.height, parent.width * Math.max(0, Math.min(1, card.progress)))
            height: parent.height
            radius: parent.radius
            color: parent.overPicture ? "#ffffff" : Theme.progressFill
            Behavior on width { NumberAnimation { duration: 100 * card.motion } }
        }
    }

    // Take it back out: a small glass button on a thumbnail, a bare cross on
    // a card.
    Item {
        objectName: "removeStagedAttachment"
        anchors.right: parent.right
        anchors.top: parent.top
        width: 22
        height: 22
        Accessible.role: Accessible.Button
        Accessible.name: "Remove " + (card.name.length > 0 ? card.name : "attachment")
        Accessible.onPressAction: card.removeRequested(card.stagedId)

        Rectangle {
            visible: card.thumbnail
            x: 4
            y: 4
            width: 16
            height: 16
            radius: 8
            color: removeMouse.containsMouse ? Theme.mediaChipHover : Theme.mediaChip
            border.width: 1
            border.color: Theme.mediaChipBorder
        }
        Shape {
            x: card.thumbnail ? 9 : 8
            y: card.thumbnail ? 9 : 8
            width: 6
            height: 6
            ShapePath {
                fillColor: "transparent"
                strokeColor: card.thumbnail ? Theme.mediaChipGlyph
                           : removeMouse.containsMouse ? Theme.textPrimary : Theme.timestampText
                strokeWidth: 1.4
                capStyle: ShapePath.RoundCap
                PathSvg { path: "M 0 0 L 6 6 M 6 0 L 0 6" }
            }
        }
        MouseArea {
            id: removeMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: card.removeRequested(card.stagedId)
        }
    }
}
