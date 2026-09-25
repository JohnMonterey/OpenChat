import QtQuick
import OpenChat
import OpenChat.Native

// A message's attachment inside its bubble (MessageDelegate loads it, and
// only for an attachment): the photo, the video's poster, the sound's player
// row or the file's row, each drawn by its own component, over the media
// handle they share. The handle holds the preview from the start and a
// photo's whole picture once it has arrived; a video's segments and a sound
// are only read when the viewer or the chat's player asks for them.
Item {
    id: block
    objectName: "attachmentBlock"
    // The MessageDelegate this belongs to: every role, the bubble's skin and
    // the signals back to the history.
    required property MessageDelegate row

    ChatAttachmentMedia {
        id: handle
        objectName: "attachmentMedia"
        controller: block.row.chatController
        stableId: block.row.stableId
        transferState: block.row.transferState
        previewRevision: block.row.previewRevision
        // A photo is shown whole in its bubble (the preview only until then).
        wantFull: block.row.attachmentKind === 1
    }

    Loader {
        anchors.fill: parent
        sourceComponent: block.row.attachmentKind === 1 ? photo
            : block.row.attachmentKind === 2 ? video
            : block.row.attachmentKind === 3 ? audio : file
    }

    Component {
        id: photo
        ChatImageBlock {
            row: block.row
            media: handle
        }
    }
    Component {
        id: video
        ChatVideoBlock {
            row: block.row
            media: handle
        }
    }
    Component {
        id: audio
        ChatAudioRow {
            row: block.row
        }
    }
    Component {
        id: file
        ChatFileRow {
            row: block.row
        }
    }
}
