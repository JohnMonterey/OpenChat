import QtQuick
import QtQuick.Window
import OpenChat
import OpenChat.Native

Window {
    id: root
    objectName: "openChatWindow"
    required property var chatController
    readonly property int sidebarWidth: Math.round(
        Math.max(250, Math.min(300, width * Theme.sidebarWidth / 860)))

    width: 860
    height: 680
    minimumWidth: 720
    minimumHeight: 560
    visible: true
    title: "OpenChat"
    color: Theme.contentBackground

    Item {
        id: applicationSurface
        anchors.fill: parent
        clip: true

        ContactSidebar {
            width: root.sidebarWidth
            height: parent.height
            controller: root.chatController
        }

        // The three navigation sections share the pane to the right of the
        // sidebar and are mutually exclusive. Chat is the default and renders
        // the approved conversation interface exactly as before; Call and
        // Settings are placeholder stubs shown only when selected.
        Item {
            id: conversationPane
            objectName: "conversationPane"
            x: root.sidebarWidth
            width: parent.width - root.sidebarWidth
            height: parent.height
            visible: root.chatController.navSection === ChatController.NavSection.Chat

            Rectangle {
                anchors.fill: parent
                gradient: Gradient {
                    GradientStop { position: 0; color: Theme.contentBackground }
                    GradientStop { position: 1; color: Theme.contentBottom }
                }
            }

            ConversationHeader {
                id: conversationHeader
                width: parent.width
                height: Theme.conversationHeaderHeight
                controller: root.chatController
            }

            // Connection/security posture strip. Collapses to zero height and is
            // invisible while the session is Ready, so the approved interface
            // renders unchanged; it expands only when a state needs explaining.
            Item {
                id: securityBanner
                objectName: "securityBanner"
                readonly property bool active: root.chatController.sessionStateText.length > 0
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: conversationHeader.bottom
                height: active ? 30 : 0
                visible: active
                clip: true

                Rectangle {
                    anchors.fill: parent
                    color: "#fdf3d8"

                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: 1
                        color: "#e4d5a3"
                    }
                    Text {
                        objectName: "securityBannerText"
                        anchors.left: parent.left
                        anchors.leftMargin: 18
                        anchors.right: parent.right
                        anchors.rightMargin: 18
                        anchors.verticalCenter: parent.verticalCenter
                        elide: Text.ElideRight
                        text: root.chatController.sessionStateText
                        color: "#7a6828"
                        font.family: Theme.uiFont
                        font.pixelSize: 13
                        renderType: Text.NativeRendering
                    }
                }
            }

            MessageHistory {
                id: history
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: securityBanner.bottom
                anchors.bottom: messageComposer.top
                controller: root.chatController
            }

            Composer {
                id: messageComposer
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: implicitHeight
                controller: root.chatController
                onMessageSent: history.positionAtEnd()
            }
        }

        Item {
            id: callView
            objectName: "callView"
            x: root.sidebarWidth
            width: parent.width - root.sidebarWidth
            height: parent.height
            visible: root.chatController.navSection === ChatController.NavSection.Call

            Rectangle {
                anchors.fill: parent
                gradient: Gradient {
                    GradientStop { position: 0; color: Theme.contentBackground }
                    GradientStop { position: 1; color: Theme.contentBottom }
                }
            }

            Text {
                anchors.centerIn: parent
                text: "Calls"
                color: Theme.textPrimary
                font.family: Theme.uiFont
                font.pixelSize: 22
                renderType: Text.NativeRendering
            }
        }

        Item {
            id: settingsView
            objectName: "settingsView"
            x: root.sidebarWidth
            width: parent.width - root.sidebarWidth
            height: parent.height
            visible: root.chatController.navSection === ChatController.NavSection.Settings

            Rectangle {
                anchors.fill: parent
                gradient: Gradient {
                    GradientStop { position: 0; color: Theme.contentBackground }
                    GradientStop { position: 1; color: Theme.contentBottom }
                }
            }

            Text {
                anchors.centerIn: parent
                text: "Settings"
                color: Theme.textPrimary
                font.family: Theme.uiFont
                font.pixelSize: 22
                renderType: Text.NativeRendering
            }
        }
    }
}
