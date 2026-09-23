pragma ComponentBehavior: Bound

import QtQuick
import OpenChat

Item {
    id: history
    objectName: "messageHistory"
    required property var controller
    // Typing that landed on a message after its text was selected; it belongs
    // in the composer.
    signal typed(string text)

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
            onRetryRequested: (messageBody) => history.controller.setComposerText(messageBody)
            onCopyRequested: history.controller.copyMessage(row.stableId)
            onEditRequested: history.controller.beginEdit(row.stableId)
            onReplyRequested: history.controller.beginReply(row.stableId)
            onQuoteActivated: history.showMessage(row.replyToId)
            onTyped: (text) => history.typed(text)
        }

        onCountChanged: Qt.callLater(positionViewAtEnd)
        Component.onCompleted: positionViewAtEnd()
    }

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
