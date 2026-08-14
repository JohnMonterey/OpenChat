pragma ComponentBehavior: Bound

import QtQuick
import OpenChat

Item {
    id: history
    objectName: "messageHistory"
    required property var controller

    function positionAtEnd() {
        messageList.positionViewAtEnd()
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
        clip: true
        interactive: contentHeight > height
        boundsBehavior: Flickable.StopAtBounds
        spacing: 0

        delegate: MessageDelegate {
            width: ListView.view.width
        }

        onCountChanged: Qt.callLater(positionViewAtEnd)
        Component.onCompleted: positionViewAtEnd()
    }

    Text {
        objectName: "noMessagesYet"
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: 32
        visible: history.controller.messages.count === 0
        text: "No messages yet."
        color: Theme.textSecondary
        font.family: Theme.uiFont
        font.pixelSize: 14
        renderType: Text.NativeRendering
    }
}
