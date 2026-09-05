import QtQuick
import QtQuick.Shapes
import OpenChat

Item {
    id: header
    objectName: "conversationHeader"
    required property var controller
    // Optional voice-call bridge; null in the default/capture paths, where the
    // call button stays inert exactly as it was.
    property var callController: null
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
        avatarKey: header.controller.currentAvatarKey
    }

    Text {
        x: 110
        y: 31
        text: header.controller.currentContactName
        color: Theme.textPrimary
        font.family: Theme.uiFont
        font.pixelSize: 20
        renderType: Text.NativeRendering
    }

    PresenceBead {
        x: 110
        y: 63
        beadSize: 11
        presence: header.controller.currentPresence
    }

    Text {
        x: 127
        y: 58
        text: header.controller.currentStatusText
        color: Theme.textSecondary
        font.family: Theme.uiFont
        font.pixelSize: 14
        renderType: Text.NativeRendering
    }

    Item {
        id: videoButton
        objectName: "videoCallButton"
        opacity: phoneButton.callable || header.callController === null ? 1.0 : 0.4
        width: 40
        height: 42
        anchors.right: phoneButton.left
        anchors.rightMargin: 22
        anchors.verticalCenter: parent.verticalCenter

        Image {
            id: videoImage
            anchors.centerIn: parent
            width: 28
            height: 22
            source: Qt.resolvedUrl("../../../assets/icons/video-call.svg")
            sourceSize: Qt.size(width * 2, height * 2)
        }
        Item {
            objectName: "videoCallFallback"
            anchors.centerIn: parent
            width: 28
            height: 22
            visible: videoImage.status === Image.Error || videoImage.status === Image.Null

            Rectangle {
                x: 0; y: 3; width: 20; height: 16; radius: 3
                color: Theme.iconInk
            }
            Shape {
                x: 20; y: 4; width: 8; height: 14
                ShapePath {
                    fillColor: Theme.iconInk
                    strokeWidth: 0
                    PathSvg { path: "M 0 4 L 8 0 L 8 14 L 0 10 Z" }
                }
            }
        }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            enabled: phoneButton.callable
            onClicked: header.callController.callCurrentContact(true)
        }
    }

    Item {
        id: phoneButton
        objectName: "phoneCallButton"
        // Calls need a bridge and a usable microphone; without either the button
        // says so by dimming rather than by silently doing nothing when pressed.
        readonly property bool callable: header.callController !== null
                                         && header.callController.callsAvailable
        width: 42
        height: 42
        anchors.right: menuButton.left
        anchors.rightMargin: 20
        anchors.verticalCenter: parent.verticalCenter
        opacity: callable || header.callController === null ? 1.0 : 0.4

        Image {
            id: phoneImage
            anchors.centerIn: parent
            width: 28
            height: 28
            source: Qt.resolvedUrl("../../../assets/icons/phone-call.svg")
            sourceSize: Qt.size(width * 2, height * 2)
        }
        Shape {
            objectName: "phoneCallFallback"
            anchors.centerIn: parent
            width: 28
            height: 28
            visible: phoneImage.status === Image.Error || phoneImage.status === Image.Null
            ShapePath {
                fillColor: Theme.iconInk
                strokeWidth: 0
                PathSvg {
                    path: "M 6.1 1.5 C 6.9 1 7.9 1.2 8.4 2 L 11.5 7 C 11.9 7.7 11.8 8.6 11.2 9.2 L 9 11.1 C 10.5 14.2 12.9 16.7 16 18.2 L 17.9 15.9 C 18.4 15.3 19.4 15.1 20.1 15.6 L 25.1 18.7 C 25.9 19.2 26.1 20.2 25.6 21 L 23.6 24.4 C 22.9 25.6 21.5 26.3 20.1 26 C 10.6 24.4 3.6 17.4 2 7.9 C 1.8 6.5 2.4 5.1 3.6 4.4 Z"
                }
            }
        }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            enabled: phoneButton.callable
            onClicked: header.callController.callCurrentContact()
        }
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
            source: Qt.resolvedUrl("../../../assets/icons/chevron-down.svg")
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
