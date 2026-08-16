import QtQuick
import OpenChat
import OpenChat.Native

Item {
    id: sidebar
    objectName: "contactSidebar"
    required property var controller
    readonly property real contactRowHeight: Math.max(44, Math.min(65, (height - 290) / 6))

    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0; color: Theme.sidebarTop }
            GradientStop { position: 1; color: Theme.sidebarBottom }
        }
    }

    Rectangle {
        anchors.right: parent.right
        width: 1
        height: parent.height
        color: Theme.rule
    }

    Item {
        id: localUser
        width: parent.width
        height: 88

        Avatar {
            objectName: "localUserAvatar"
            x: 13
            y: 22
            width: 44
            height: 44
            avatarKey: "userpfp_none"
        }
        Text {
            x: 67; y: 22
            text: "Daniel"
            color: Theme.textPrimary
            font.family: Theme.uiFont
            font.pixelSize: 17
            renderType: Text.NativeRendering
        }
        PresenceBead { x: 67; y: 51; beadSize: 11; presence: 0 }
        Text {
            x: 84; y: 46
            text: "Available"
            color: Theme.textSecondary
            font.family: Theme.uiFont
            font.pixelSize: 14
            renderType: Text.NativeRendering
        }
        Text {
            x: 159; y: 28
            text: "▾"
            color: Theme.iconInk
            font.pixelSize: 13
        }
    }

    Item {
        id: searchArea
        anchors.top: localUser.bottom
        width: parent.width
        height: 46

        Rectangle {
            x: 13
            y: 7
            width: parent.width - 26
            height: 32
            radius: 4
            color: "#f9fcfe"
            border.width: 1
            border.color: Theme.inputBorder

            TextInput {
                id: searchInput
                objectName: "contactSearch"
                x: 7
                y: 5
                width: parent.width - 38
                height: parent.height - 10
                color: Theme.textPrimary
                font.family: Theme.uiFont
                font.pixelSize: 14
                clip: true
                selectByMouse: true
                onTextEdited: sidebar.controller.setSearchQuery(text)

                Text {
                    anchors.fill: parent
                    visible: !searchInput.text && !searchInput.activeFocus
                    text: "Search & Find"
                    color: "#98a7ba"
                    font: searchInput.font
                    verticalAlignment: Text.AlignVCenter
                }
            }

            Item {
                anchors.right: parent.right
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                width: 15
                height: 15

                Rectangle {
                    x: 1; y: 1; width: 8; height: 8; radius: 4
                    color: "transparent"; border.width: 2; border.color: "#8798aa"
                }
                Rectangle {
                    x: 9; y: 9; width: 7; height: 2
                    rotation: 45; color: "#8798aa"; transformOrigin: Item.Left
                }
            }
        }
    }

    // The region between the search field and the bottom navigation swaps its
    // content with the active section. Exactly one container is visible at a
    // time; each fills the same middle band so the panes line up. Chat keeps the
    // approved contact list unchanged; Call shows the (currently empty) call
    // history list.
    Item {
        id: chatContactArea
        objectName: "chatContactArea"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: searchArea.bottom
        anchors.bottom: bottomNav.top
        visible: sidebar.controller.navSection === ChatController.NavSection.Chat

        Column {
            anchors.top: parent.top
            width: parent.width

            ContactCategory {
                objectName: "favoritesCategory"
                width: parent.width
                label: "Requests"
                favoriteCategory: true
                contactModel: sidebar.controller.contacts
                controller: sidebar.controller
                rowHeight: sidebar.contactRowHeight
            }

            ContactCategory {
                objectName: "contactsCategory"
                width: parent.width
                label: "Chats"
                favoriteCategory: false
                contactModel: sidebar.controller.contacts
                controller: sidebar.controller
                rowHeight: sidebar.contactRowHeight
            }
        }

        Text {
            objectName: "noContactsFound"
            anchors.top: parent.top
            anchors.topMargin: 30
            anchors.horizontalCenter: parent.horizontalCenter
            visible: sidebar.controller.contacts.favoriteCount
                     + sidebar.controller.contacts.regularCount === 0
            text: "No contacts found"
            color: Theme.textSecondary
            font.family: Theme.uiFont
            font.pixelSize: 14
            renderType: Text.NativeRendering
        }
    }

    Item {
        id: sidebarCallList
        objectName: "sidebarCallList"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: searchArea.bottom
        anchors.bottom: bottomNav.top
        visible: sidebar.controller.navSection === ChatController.NavSection.Call

        // Empty state until call history exists. When callCount grows, a call
        // ListView slots in here in place of this placeholder.
        Text {
            objectName: "noCallsYet"
            anchors.top: parent.top
            anchors.topMargin: 30
            anchors.horizontalCenter: parent.horizontalCenter
            visible: sidebar.controller.callCount === 0
            text: "No calls yet."
            color: Theme.textSecondary
            font.family: Theme.uiFont
            font.pixelSize: 14
            renderType: Text.NativeRendering
        }
    }

    // Bottom navigation. Three equal columns switch the main pane between the
    // conversation view (Chat) and the Call / Settings placeholders. The active
    // section is owned by the controller so this highlight and the pane shown on
    // the right stay in agreement; the badge counts are model-backed and a badge
    // only appears while its count is greater than zero.
    Rectangle {
        id: bottomNav
        objectName: "bottomNav"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 64
        color: Theme.contentBottom

        Row {
            anchors.fill: parent

            Item {
                id: callTab
                objectName: "callTab"
                width: bottomNav.width / 3
                height: bottomNav.height
                readonly property bool active:
                    sidebar.controller.navSection === ChatController.NavSection.Call

                Rectangle {
                    anchors.fill: parent
                    color: callTab.active ? "#e7f2f8" : "transparent"
                }

                Item {
                    id: callIcon
                    width: 26
                    height: 26
                    anchors.horizontalCenter: parent.horizontalCenter
                    y: 10

                    Image {
                        anchors.centerIn: parent
                        width: 24
                        height: 24
                        source: Qt.resolvedUrl("../../../assets/icons/phone-call.svg")
                        sourceSize: Qt.size(width * 2, height * 2)
                    }

                    Rectangle {
                        objectName: "callBadge"
                        anchors.right: parent.right
                        anchors.rightMargin: -4
                        anchors.top: parent.top
                        anchors.topMargin: -3
                        visible: sidebar.controller.callMissedCount > 0
                        height: 15
                        width: Math.max(15, callBadgeLabel.implicitWidth + 8)
                        radius: height / 2
                        color: "#e0503d"

                        Text {
                            id: callBadgeLabel
                            objectName: "callBadgeLabel"
                            anchors.centerIn: parent
                            text: sidebar.controller.callMissedCount
                            color: "white"
                            font.family: Theme.uiFont
                            font.pixelSize: 10
                            font.bold: true
                        }
                    }
                }

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 10
                    text: "Call"
                    color: callTab.active ? Theme.categoryText : "#6f8eac"
                    font.family: Theme.uiFont
                    font.pixelSize: 13
                    renderType: Text.NativeRendering
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: sidebar.controller.setNavSection(ChatController.NavSection.Call)
                }
            }

            Item {
                id: chatTab
                objectName: "chatTab"
                width: bottomNav.width / 3
                height: bottomNav.height
                readonly property bool active:
                    sidebar.controller.navSection === ChatController.NavSection.Chat

                Rectangle {
                    anchors.fill: parent
                    color: chatTab.active ? "#e7f2f8" : "transparent"
                }

                Item {
                    id: chatIcon
                    width: 26
                    height: 26
                    anchors.horizontalCenter: parent.horizontalCenter
                    y: 10

                    // Speech bubble drawn from primitives, matching the house
                    // style of the search magnifier and presence beads.
                    Rectangle {
                        x: 2
                        y: 2
                        width: 22
                        height: 16
                        radius: 6
                        color: Theme.iconInk
                    }
                    Rectangle {
                        x: 5
                        y: 13
                        width: 7
                        height: 7
                        rotation: 45
                        color: Theme.iconInk
                    }

                    Rectangle {
                        objectName: "chatBadge"
                        anchors.right: parent.right
                        anchors.rightMargin: -4
                        anchors.top: parent.top
                        anchors.topMargin: -3
                        visible: sidebar.controller.chatUnreadCount > 0
                        height: 15
                        width: Math.max(15, chatBadgeLabel.implicitWidth + 8)
                        radius: height / 2
                        color: "#e0503d"

                        Text {
                            id: chatBadgeLabel
                            objectName: "chatBadgeLabel"
                            anchors.centerIn: parent
                            text: sidebar.controller.chatUnreadCount
                            color: "white"
                            font.family: Theme.uiFont
                            font.pixelSize: 10
                            font.bold: true
                        }
                    }
                }

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 10
                    text: "Chat"
                    color: chatTab.active ? Theme.categoryText : "#6f8eac"
                    font.family: Theme.uiFont
                    font.pixelSize: 13
                    renderType: Text.NativeRendering
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: sidebar.controller.setNavSection(ChatController.NavSection.Chat)
                }
            }

            Item {
                id: settingsTab
                objectName: "settingsTab"
                width: bottomNav.width / 3
                height: bottomNav.height
                readonly property bool active:
                    sidebar.controller.navSection === ChatController.NavSection.Settings

                Rectangle {
                    anchors.fill: parent
                    color: settingsTab.active ? "#e7f2f8" : "transparent"
                }

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    y: 8
                    text: "⚙"
                    color: Theme.iconInk
                    font.family: Theme.uiFont
                    font.pixelSize: 24
                }

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 10
                    text: "Settings"
                    color: settingsTab.active ? Theme.categoryText : "#6f8eac"
                    font.family: Theme.uiFont
                    font.pixelSize: 13
                    renderType: Text.NativeRendering
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: sidebar.controller.setNavSection(ChatController.NavSection.Settings)
                }
            }
        }

        Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.rule }
    }
}
