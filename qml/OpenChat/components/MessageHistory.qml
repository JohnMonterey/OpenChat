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
        objectName: "messageList"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: dateDivider.bottom
        anchors.topMargin: 12
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
}
