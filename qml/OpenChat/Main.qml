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

        // Neutral no-selection pane for the Call section. Call history lives in
        // the sidebar; until a call is picked there is nothing to show here, so
        // this stays an empty gradient pane matching the conversation backdrop.
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
        }

        // Settings detail pane: the title and element rows of the category
        // selected in the sidebar. The element rows are visual stubs — a label
        // and a muted disclosure chevron — until the individual controls are
        // wired to real preferences.
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

            Item {
                id: settingsDetail
                objectName: "settingsDetail"
                anchors.fill: parent
                anchors.leftMargin: 34
                anchors.rightMargin: 34
                anchors.topMargin: 28

                Text {
                    id: settingsDetailTitle
                    objectName: "settingsDetailTitle"
                    anchors.left: parent.left
                    anchors.right: parent.right
                    text: root.chatController.currentSettingsCategoryName
                    color: Theme.textPrimary
                    font.family: Theme.uiFont
                    font.pixelSize: 22
                    elide: Text.ElideRight
                    renderType: Text.NativeRendering
                }

                Rectangle {
                    id: settingsTitleRule
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: settingsDetailTitle.bottom
                    anchors.topMargin: 16
                    height: 1
                    color: Theme.rule
                }

                Column {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: settingsTitleRule.bottom
                    anchors.topMargin: 6

                    Repeater {
                        model: root.chatController.currentSettingsElements

                        Item {
                            id: settingsElementRow
                            property string elementLabel: modelData
                            width: parent.width
                            height: 48

                            Text {
                                anchors.left: parent.left
                                anchors.verticalCenter: parent.verticalCenter
                                text: settingsElementRow.elementLabel
                                color: Theme.textPrimary
                                font.family: Theme.uiFont
                                font.pixelSize: 15
                                renderType: Text.NativeRendering
                            }

                            Item {
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                width: 14
                                height: 14

                                Image {
                                    anchors.centerIn: parent
                                    width: 13
                                    height: 8
                                    opacity: 0.45
                                    rotation: -90
                                    source: Qt.resolvedUrl(
                                                "../../assets/icons/chevron-down.svg")
                                    sourceSize: Qt.size(width * 2, height * 2)
                                }
                            }

                            Rectangle {
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                height: 1
                                color: "#e4edf3"
                            }
                        }
                    }
                }
            }
        }
    }
}
