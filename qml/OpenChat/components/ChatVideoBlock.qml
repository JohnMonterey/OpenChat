import QtQuick
import OpenChat
import OpenChat.Native

// A video in its bubble: its poster frame cropped to the bubble's shape, a
// glass play button and its length. It plays in the viewer, opened by a
// click, never in the bubble; while it travels the ring over it fills.
Item {
    id: video
    objectName: "chatVideoBlock"
    required property MessageDelegate row
    // The ChatAttachmentMedia handle AttachmentBlock holds for this message.
    required property ChatAttachmentMedia media
    readonly property bool complete: row.transferState === 1

    Accessible.role: Accessible.Button
    Accessible.name: "Video, " + video.clock(row.durationMs) + (row.body.length > 0 ? ": " + row.body : "")
    Accessible.onPressAction: video.open()

    function open() {
        if (video.complete)
            video.row.mediaOpened(poster);
    }
    function clock(ms) {
        const seconds = Math.max(0, Math.round(ms / 1000));
        return Math.floor(seconds / 60) + ":" + ("0" + seconds % 60).slice(-2);
    }

    Rectangle {
        anchors.fill: parent
        radius: 4
        color: "#1b1f27"
    }
    ProfileGlyph {
        anchors.centerIn: parent
        visible: !poster.ready && !overlay.visible
        width: 34
        height: 34
        kind: "film"
        ink: "#8a9aac"
    }
    ProfilePanelImage {
        id: poster
        objectName: "chatVideoPoster"
        anchors.fill: parent
        mediaKey: video.media.previewKey
        crop: true
        radius: 4
    }
    Rectangle {
        anchors.fill: parent
        radius: 4
        color: "transparent"
        border.width: 1
        border.color: "#1c000000"
    }

    // The play button, once there is something to play.
    Rectangle {
        objectName: "chatVideoPlay"
        visible: video.complete
        anchors.centerIn: parent
        width: 48
        height: 48
        radius: 24
        color: playMouse.containsMouse ? "#c0000000" : "#a0000000"
        border.width: 2
        border.color: "#e6ffffff"
        // The play glyph is drawn with its centroid in the middle, which is
        // where the eye puts a triangle's centre; no further nudge.
        ProfileGlyph {
            anchors.centerIn: parent
            width: 22
            height: 22
            kind: "play"
            ink: "white"
        }
    }
    ChatTimeChip {
        visible: video.row.durationMs > 0
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.margins: 6
        text: video.clock(video.row.durationMs)
    }
    ChatTimeChip {
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 6
        visible: !video.row.hasCaption
        text: video.row.timestamp
    }

    ChatTransferOverlay {
        id: overlay
        anchors.fill: parent
        row: video.row
    }

    MouseArea {
        id: playMouse
        anchors.fill: parent
        z: -1
        hoverEnabled: true
        cursorShape: video.complete ? Qt.PointingHandCursor : Qt.ArrowCursor
        onClicked: video.open()
    }
}
