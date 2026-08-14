import QtQuick
import QtQuick.Window
import OpenChat

Window {
    id: root
    objectName: "openChatWindow"

    width: 860
    height: 680
    minimumWidth: 720
    minimumHeight: 560
    visible: true
    title: "OpenChat"
    color: "transparent"
    flags: Qt.Window | Qt.FramelessWindowHint

    AeroWindowFrame {
        anchors.fill: parent
        window: root
    }

    Item {
        id: applicationSurface
        x: Theme.frameInset + 1
        y: Theme.titleBarHeight + 1
        width: parent.width - (Theme.frameInset + 1) * 2
        height: parent.height - Theme.titleBarHeight - Theme.frameInset - 2
        clip: true

        ContactSidebar {
            width: Theme.sidebarWidth
            height: parent.height
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
            }

            MessageHistory {
                id: history
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: conversationHeader.bottom
                anchors.bottom: messageComposer.top
            }

            Composer {
                id: messageComposer
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: Theme.composerHeight
                onMessageSent: history.positionAtEnd()
            }
        }
    }
}
