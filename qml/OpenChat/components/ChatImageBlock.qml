import QtQuick
import OpenChat
import OpenChat.Native

// A photo in its bubble, cropped to the bubble's shape: its small preview as
// soon as that has arrived, the whole picture over it once that has, and a
// click opens it large. While it travels the ring over it fills; without a
// caption the time rides on its corner.
Item {
    id: photo
    objectName: "chatImageBlock"
    required property MessageDelegate row
    // The ChatAttachmentMedia handle AttachmentBlock holds for this message.
    required property ChatAttachmentMedia media
    readonly property bool complete: row.transferState === 1
    readonly property bool shown: preview.ready || picture.ready

    Accessible.role: Accessible.Button
    Accessible.name: "Photo" + (row.body.length > 0 ? ": " + row.body : "")
    Accessible.onPressAction: photo.open()

    function open() {
        if (photo.complete)
            photo.row.mediaOpened(picture);
    }

    Rectangle {
        anchors.fill: parent
        radius: 4
        color: Theme.mediaPlaceholder
    }
    ProfileGlyph {
        anchors.centerIn: parent
        visible: !photo.shown && !overlay.visible
        width: 34
        height: 34
        kind: "image"
        ink: Theme.textSecondary
        opacity: 0.6
    }
    ProfilePanelImage {
        id: preview
        objectName: "chatImagePreview"
        anchors.fill: parent
        mediaKey: photo.media.previewKey
        crop: true
        radius: 4
        visible: !picture.ready
    }
    ProfilePanelImage {
        id: picture
        objectName: "chatImage"
        anchors.fill: parent
        mediaKey: photo.media.imageKey
        crop: true
        radius: 4
    }
    // A hairline rim, so a pale photo still has an edge in a pale bubble.
    Rectangle {
        anchors.fill: parent
        radius: 4
        color: "transparent"
        border.width: 1
        border.color: "#1c000000"
    }

    ChatTransferOverlay {
        id: overlay
        anchors.fill: parent
        row: photo.row
    }
    ChatTimeChip {
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 6
        visible: !photo.row.hasCaption
        text: photo.row.timestamp
    }

    MouseArea {
        anchors.fill: parent
        z: -1
        cursorShape: photo.complete ? Qt.PointingHandCursor : Qt.ArrowCursor
        onClicked: photo.open()
    }
}
