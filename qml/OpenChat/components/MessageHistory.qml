pragma ComponentBehavior: Bound

import QtQuick
import OpenChat
import OpenChat.Native

Item {
    id: history
    objectName: "messageHistory"
    required property var controller
    // A call rings, runs or has just ended: the chat's sound keeps quiet.
    property bool callActive: false
    // The chat's one audio player, for the bubbles to drive.
    readonly property alias audio: chatAudio
    // Typing that landed on a message after its text was selected; it belongs
    // in the composer.
    signal typed(string text)
    // An attachment asked to be opened large (`info` says which and what it
    // is, `source` is its picture in the bubble, to grow from), or to be saved.
    signal mediaRequested(var info, Item source)
    signal saveRequested(string stableId)

    function positionAtEnd() {
        messageList.positionViewAtEnd()
    }

    // Brings a message into view, e.g. the one a reply quotes. Nothing happens
    // for one no longer (or never) in this history.
    function showMessage(stableId) {
        const row = history.controller.messages.rowOf(stableId);
        if (row >= 0)
            messageList.positionViewAtIndex(row, ListView.Center);
    }

    onHeightChanged: Qt.callLater(positionAtEnd)

    ListView {
        id: messageList
        objectName: "messageList"
        // A scroll never leaves the rows on half pixels, where a button's
        // disc and its glyph would be drawn half a pixel apart.
        pixelAligned: true
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        model: history.controller.messages
        visible: history.controller.plaintextVisible
        clip: true
        interactive: contentHeight > height
        boundsBehavior: Flickable.StopAtBounds
        spacing: 0

        delegate: MessageDelegate {
            id: row
            // The roles MessageDelegate leaves optional come through the model
            // object: a delegate with required properties gets no others.
            required property var model
            width: ListView.view.width
            stableId: model.stableId
            edited: model.edited
            editable: model.editable
            replyToId: model.replyToId
            quotedSender: model.quotedSender
            quotedBody: model.quotedBody
            senderAccount: model.senderAccount
            editing: stableId.length > 0 && stableId === history.controller.editingMessageId
            attachmentKind: model.attachmentKind
            fileName: model.fileName
            mimeType: model.mimeType
            byteCount: model.byteCount
            sizeText: model.sizeText
            mediaWidth: model.mediaWidth
            mediaHeight: model.mediaHeight
            durationMs: model.durationMs
            peaks: model.peaks
            transferState: model.transferState
            transferReason: model.transferReason
            transferProgress: model.transferProgress
            transferText: model.transferText
            hasPreview: model.hasPreview
            previewRevision: model.previewRevision
            canCancel: model.canCancel
            canRetry: model.canRetry
            canSave: model.canSave
            chatController: history.controller
            chatAudio: history.audio
            // A failed attachment goes back to the tray as a new one; a failed
            // text back into the composer.
            onRetryRequested: (messageBody) => {
                if (row.attachment)
                    history.controller.retryAttachment(row.stableId);
                else
                    history.controller.setComposerText(messageBody);
            }
            onMediaOpened: source => history.mediaRequested({
                stableId: row.stableId,
                attachmentKind: row.attachmentKind,
                mediaWidth: row.mediaWidth,
                mediaHeight: row.mediaHeight,
                durationMs: row.durationMs,
                caption: row.body
            }, source)
            onSaveRequested: history.saveRequested(row.stableId)
            onCancelRequested: history.controller.cancelAttachment(row.stableId)
            onCopyRequested: history.controller.copyMessage(row.stableId)
            onEditRequested: history.controller.beginEdit(row.stableId)
            onReplyRequested: history.controller.beginReply(row.stableId)
            onQuoteActivated: history.showMessage(row.replyToId)
            onTyped: (text) => history.typed(text)
        }

        onCountChanged: Qt.callLater(positionViewAtEnd)
        Component.onCompleted: positionViewAtEnd()
    }

    // The chat's one audio player. A sound in a bubble plays here rather than
    // in the bubble, so a scroll or a new message (which rebuild bubbles)
    // never cuts it off; a bubble shows what the player is doing when the
    // sound is its own, and asks it to play, pause or seek. The sound is only
    // read out of the message once asked for, and it stops with the chat,
    // whenever the messages are hidden, and when a call starts.
    Item {
        id: chatAudio
        objectName: "chatAudioPlayer"
        // The message whose sound is loaded (playing or not), "" for none.
        property string activeId: ""
        property bool playWhenLoaded: false
        property real startAtMs: 0
        readonly property bool playing: player.playing
        readonly property real positionMs: player.positionMs
        readonly property real durationMs: player.valid ? player.durationMs : 0
        readonly property bool loading: activeId.length > 0 && playWhenLoaded

        function toggle(stableId) {
            if (stableId === activeId && player.valid) {
                player.toggle();
                return;
            }
            load(stableId, 0);
        }
        // Moves `stableId`'s sound to `ms` and plays it from there.
        function seek(stableId, ms) {
            if (stableId === activeId && player.valid) {
                player.seek(ms);
                if (!player.playing)
                    player.play();
                return;
            }
            load(stableId, ms);
        }
        function stop() {
            playWhenLoaded = false;
            player.stop();
            activeId = "";
        }
        function load(stableId, ms) {
            player.stop();
            startAtMs = ms;
            playWhenLoaded = true;
            activeId = stableId;
            startIfLoaded();
        }
        function startIfLoaded() {
            if (!playWhenLoaded || !player.valid)
                return;
            playWhenLoaded = false;
            if (startAtMs > 0)
                player.seek(startAtMs);
            player.play();
        }

        ChatAttachmentMedia {
            id: audioMedia
            controller: history.controller
            stableId: chatAudio.activeId
            transferState: 1
            wantSong: chatAudio.activeId.length > 0
        }
        SongPlayer {
            id: player
            longForm: true
            songKey: audioMedia.songKey
            suspended: history.callActive
            onSourceChanged: chatAudio.startIfLoaded()
        }
    }
    readonly property bool plaintextVisible: history.controller.plaintextVisible
    readonly property string chatId: history.controller.currentContactId
    onPlaintextVisibleChanged: if (!plaintextVisible) chatAudio.stop()
    onChatIdChanged: chatAudio.stop()

    Text {
        objectName: "noMessagesYet"
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: 32
        visible: history.controller.plaintextVisible
            && history.controller.messages.count === 0
        text: "No messages yet."
        color: Theme.textSecondary
        font.family: Theme.uiFont
        font.pixelSize: 14
        renderType: Text.NativeRendering
    }

    // Shown when the current state withholds message plaintext (locked vault,
    // quarantined conversation, unverified device change). The message model is
    // emptied by the controller in these states, so no plaintext is present in
    // the scene graph to display.
    Item {
        id: securityNotice
        objectName: "securityNotice"
        anchors.fill: parent
        visible: !history.controller.plaintextVisible

        Column {
            anchors.centerIn: parent
            width: Math.min(360, parent.width - 48)
            spacing: 10

            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: "Messages hidden"
                color: Theme.textPrimary
                font.family: Theme.uiFont
                font.pixelSize: 16
                font.bold: true
                renderType: Text.NativeRendering
            }
            Text {
                objectName: "securityNoticeText"
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: history.controller.securityNoticeText
                color: Theme.textSecondary
                font.family: Theme.uiFont
                font.pixelSize: 14
                renderType: Text.NativeRendering
            }
        }
    }
}
