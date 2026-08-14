import QtQuick
import QtQuick.Window
import OpenChat

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

        Item {
            id: conversationPane
            x: root.sidebarWidth
            width: parent.width - root.sidebarWidth
            height: parent.height

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

            MessageHistory {
                id: history
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: conversationHeader.bottom
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
    }
}
