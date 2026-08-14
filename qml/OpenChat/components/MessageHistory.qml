import QtQuick
import OpenChat

Item {
    id: history
    objectName: "messageHistory"

    function positionAtEnd() {
        messageList.positionViewAtEnd()
    }

    Item {
        id: dateDivider
        x: 18
        y: 20
        width: parent.width - 36
        height: 32

        Rectangle {
            anchors.left: parent.left
            anchors.right: dateLabel.left
            anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            height: 1
            color: "#d7e1e8"
        }
        Text {
            id: dateLabel
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.verticalCenter: parent.verticalCenter
            text: "May 24, 2010"
            color: Theme.textSecondary
            font.family: Theme.uiFont
            font.pixelSize: 14
            renderType: Text.NativeRendering
        }
        Rectangle {
            anchors.left: dateLabel.right
            anchors.leftMargin: 10
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            height: 1
            color: "#d7e1e8"
        }
    }

    ListView {
        id: messageList
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: dateDivider.bottom
        anchors.topMargin: 4
        anchors.bottom: parent.bottom
        model: chatController.messages
        clip: true
        interactive: contentHeight > height
        boundsBehavior: Flickable.StopAtBounds
        spacing: 0

        delegate: MessageDelegate {
            width: messageList.width
            messageDirection: model.direction
            body: model.body
            timestamp: model.timestamp
            messageKind: model.kind
        }

        onCountChanged: Qt.callLater(positionViewAtEnd)
        Component.onCompleted: positionViewAtEnd()
    }
}
