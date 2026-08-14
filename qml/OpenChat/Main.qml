import QtQuick
import QtQuick.Window
import OpenChat

Window {
    id: root
    objectName: "openChatWindow"
    required property var chatController

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
            width: Theme.sidebarWidth
            height: parent.height
            controller: root.chatController
        }

        Item {
            id: conversationPane
            x: Theme.sidebarWidth
            width: parent.width - Theme.sidebarWidth
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
                height: Theme.composerHeight
                controller: root.chatController
                onMessageSent: history.positionAtEnd()
            }
        }
    }
}
