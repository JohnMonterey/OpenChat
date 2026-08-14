import QtQuick
import OpenChat

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

        PresenceBead { x: 17; y: 30; beadSize: 13; presence: 0 }
        Text {
            x: 41; y: 24
            text: "Daniel"
            color: Theme.textPrimary
            font.family: Theme.uiFont
            font.pixelSize: 17
            renderType: Text.NativeRendering
        }
        Text {
            x: 41; y: 48
            text: "Available"
            color: Theme.textSecondary
            font.family: Theme.uiFont
            font.pixelSize: 14
            renderType: Text.NativeRendering
        }
        Text {
            x: 116; y: 29
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
                    text: "Search contacts..."
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

    Column {
        anchors.top: searchArea.bottom
        width: parent.width

        ContactCategory {
            objectName: "favoritesCategory"
            width: parent.width
            label: "Favorites"
            favoriteCategory: true
            contactModel: sidebar.controller.contacts
            controller: sidebar.controller
            rowHeight: sidebar.contactRowHeight
        }

        ContactCategory {
            objectName: "contactsCategory"
            width: parent.width
            label: "Contacts"
            favoriteCategory: false
            contactModel: sidebar.controller.contacts
            controller: sidebar.controller
            rowHeight: sidebar.contactRowHeight
        }
    }

    Text {
        objectName: "noContactsFound"
        anchors.top: searchArea.bottom
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

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 45
        color: "#e7f2f8"
        border.width: 0

        Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.rule }

        Row {
            anchors.centerIn: parent
            spacing: 31

            Text {
                text: "♟  Add Contact"
                color: "#6f8eac"
                font.family: Theme.uiFont
                font.pixelSize: 13
            }
            Text {
                text: "⚙  Settings"
                color: "#6f8eac"
                font.family: Theme.uiFont
                font.pixelSize: 13
            }
        }
    }
}
