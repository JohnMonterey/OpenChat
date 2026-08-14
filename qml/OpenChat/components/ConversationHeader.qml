import QtQuick
import OpenChat

Item {
    id: header
    objectName: "conversationHeader"
    implicitHeight: Theme.conversationHeaderHeight

    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0; color: "#fbfdff" }
            GradientStop { position: 1; color: "#edf5fb" }
        }
    }

    Avatar {
        id: contactAvatar
        x: 24
        y: 21
        width: 68
        height: 68
        cornerRadius: 6
        avatarKey: chatController.currentAvatarKey
    }

    Text {
        x: 110
        y: 31
        text: chatController.currentContactName
        color: Theme.textPrimary
        font.family: Theme.uiFont
        font.pixelSize: 20
        renderType: Text.NativeRendering
    }

    PresenceBead {
        x: 110
        y: 63
        beadSize: 11
        presence: chatController.currentStatusText === "Available" ? 0
                : chatController.currentStatusText === "Away" ? 1 : 2
    }

    Text {
        x: 127
        y: 58
        text: chatController.currentStatusText
        color: Theme.textSecondary
        font.family: Theme.uiFont
        font.pixelSize: 14
        renderType: Text.NativeRendering
    }

    Item {
        id: videoButton
        objectName: "videoCallButton"
        width: 40
        height: 42
        anchors.right: phoneButton.left
        anchors.rightMargin: 22
        anchors.verticalCenter: parent.verticalCenter

        Image {
            anchors.centerIn: parent
            width: 28
            height: 22
            source: "qrc:/qt/qml/OpenChat/assets/icons/video-call.svg"
            sourceSize: Qt.size(width * 2, height * 2)
        }
        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor }
    }

    Item {
        id: phoneButton
        objectName: "phoneCallButton"
        width: 42
        height: 42
        anchors.right: menuButton.left
        anchors.rightMargin: 20
        anchors.verticalCenter: parent.verticalCenter

        Image {
            anchors.centerIn: parent
            width: 28
            height: 28
            source: "qrc:/qt/qml/OpenChat/assets/icons/phone-call.svg"
            sourceSize: Qt.size(width * 2, height * 2)
        }
        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor }
    }

    Item {
        id: menuButton
        width: 34
        height: 42
        anchors.right: parent.right
        anchors.rightMargin: 17
        anchors.verticalCenter: parent.verticalCenter

        Image {
            anchors.centerIn: parent
            width: 13
            height: 9
            source: "qrc:/qt/qml/OpenChat/assets/icons/chevron-down.svg"
        }
        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: Theme.rule
    }
}
