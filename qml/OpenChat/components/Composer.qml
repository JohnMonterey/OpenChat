import QtQuick
import OpenChat

Item {
    id: composer
    objectName: "messageComposer"
    signal messageSent

    Rectangle {
        anchors.fill: parent
        color: "#eef4f8"
    }
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 1
        color: Theme.rule
    }

    Rectangle {
        id: inputFrame
        x: 17
        y: 17
        width: parent.width - 142
        height: parent.height - 34
        radius: 5
        color: "#ffffff"
        border.width: 1
        border.color: Theme.inputBorder

        TextEdit {
            id: input
            objectName: "messageInput"
            x: 12
            y: 10
            width: parent.width - 56
            height: parent.height - 20
            text: chatController.composerText
            color: Theme.textPrimary
            font.family: Theme.uiFont
            font.pixelSize: 16
            wrapMode: TextEdit.Wrap
            selectByMouse: true
            clip: true
            onTextChanged: {
                if (text !== chatController.composerText)
                    chatController.setComposerText(text)
            }
            Keys.onReturnPressed: event => {
                if (!(event.modifiers & Qt.ShiftModifier) && chatController.canSend) {
                    chatController.sendMessage()
                    composer.messageSent()
                    event.accepted = true
                }
            }
        }

        Rectangle {
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.right: parent.right
            width: 38
            radius: 5
            color: "#fbfdfe"
            border.width: 0

            Rectangle {
                anchors.left: parent.left
                width: 1
                height: parent.height
                color: "#d9e1e7"
            }
            Image {
                anchors.centerIn: parent
                width: 11
                height: 7
                source: "qrc:/qt/qml/OpenChat/assets/icons/chevron-down.svg"
            }
            MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor }
        }
    }

    Item {
        id: send
        objectName: "sendButton"
        property bool enabled: chatController.canSend
        signal clicked
        x: inputFrame.x + inputFrame.width + 17
        y: 32
        width: parent.width - x - 18
        height: 43

        Rectangle {
            anchors.fill: parent
            radius: 4
            color: send.enabled ? (sendMouse.containsMouse ? "#f8fbfd" : "#f5f8fa") : "#eef2f5"
            border.width: 1
            border.color: send.enabled ? "#aebdca" : "#c5d0d9"
        }
        Text {
            anchors.centerIn: parent
            text: "Send"
            color: send.enabled ? "#596c83" : "#8b99aa"
            font.family: Theme.uiFont
            font.pixelSize: 15
            renderType: Text.NativeRendering
        }
        MouseArea {
            id: sendMouse
            anchors.fill: parent
            enabled: send.enabled
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: send.clicked()
        }
        onClicked: {
            if (send.enabled && chatController.sendMessage())
                composer.messageSent()
        }
    }
}
