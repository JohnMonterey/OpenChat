import QtQuick
import OpenChat

Item {
    id: composer
    objectName: "messageComposer"
    required property var controller
    signal messageSent
    readonly property real inputHeight: Math.max(48, Math.min(112, Math.ceil(input.contentHeight) + 20))
    implicitHeight: inputHeight + 34

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

    Item {
        id: inputFrame
        objectName: "composerInputFrame"
        x: 17
        y: 17
        width: parent.width - 142
        height: composer.inputHeight

        Rectangle {
            anchors.fill: parent
            radius: 5
            color: "#ffffff"
        }

        TextEdit {
            id: input
            objectName: "messageInput"
            anchors.left: parent.left
            anchors.right: attachment.left
            anchors.leftMargin: 12
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            height: Math.max(contentHeight, 20)
            text: composer.controller.composerText
            color: Theme.textPrimary
            font.family: Theme.uiFont
            font.pixelSize: 16
            wrapMode: TextEdit.Wrap
            selectByMouse: true
            clip: false
            onTextChanged: {
                if (text !== composer.controller.composerText)
                    composer.controller.setComposerText(text);
            }
            Keys.onReturnPressed: event => {
                if (!(event.modifiers & Qt.ShiftModifier) && composer.controller.canSend) {
                    composer.controller.sendMessage();
                    composer.messageSent();
                    event.accepted = true;
                }
            }
        }

        Item {
            id: attachment
            objectName: "attachmentButton"
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.right: parent.right
            width: 38

            Rectangle {
                anchors.fill: parent
                radius: 5
                color: "#fbfdfe"
            }
            Rectangle {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width / 2
                height: parent.height
                color: "#fbfdfe"
            }

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
                source: Qt.resolvedUrl("../../../assets/icons/chevron-down.svg")
            }
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
            }
        }

        // Restrained Aero inset: a shadow along the inner top/left edges and a
        // faint highlight opposite it. The shared outer outline is drawn last.
        Rectangle {
            x: 5
            y: 1
            width: parent.width - 10
            height: 1
            color: "#24526878"
        }
        Rectangle {
            x: 1
            y: 5
            width: 1
            height: parent.height - 10
            color: "#18526878"
        }
        Rectangle {
            x: 5
            y: parent.height - 2
            width: parent.width - 10
            height: 1
            color: "#70ffffff"
        }
        Rectangle {
            anchors.fill: parent
            z: 10
            radius: 5
            color: "transparent"
            border.width: 1
            border.color: Theme.inputBorder
        }
    }

    Item {
        id: send
        objectName: "sendButton"
        enabled: composer.controller.canSend
        signal clicked
        x: inputFrame.x + inputFrame.width + 17
        y: Math.round((parent.height - height) / 2)
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
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: send.clicked()
        }
        onClicked: {
            if (send.enabled && composer.controller.sendMessage())
                composer.messageSent();
        }
    }
}
