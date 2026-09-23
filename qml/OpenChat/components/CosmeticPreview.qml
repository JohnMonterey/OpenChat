import QtQuick
import OpenChat
import OpenChat.Native

// A still preview of one collectible, drawn by the same component that wears
// it in the app, centred and scaled down to fit. `item` is a catalogue map
// (Cosmetics.item(id)); an unknown or empty item draws nothing.
Item {
    id: root
    property var item: ({})
    readonly property string itemId: item && item.id ? item.id : ""
    readonly property string category: item && item.category ? item.category : ""
    implicitWidth: 84
    implicitHeight: 56

    Loader {
        id: loader
        anchors.centerIn: parent
        scale: loader.item ? Math.min(1, root.width / loader.item.width, root.height / loader.item.height) : 1
        sourceComponent: root.category === "bubble" ? bubble
            : root.category === "frame" ? frame
            : root.category === "bead" ? bead
            : root.category === "flair" ? flair
            : root.category === "scene" ? scene : null
    }

    Component {
        id: bubble
        Item {
            width: 72
            height: 36
            BubbleBackground {
                id: skinned
                anchors.fill: parent
                outgoing: true
                radius: 6
                tailWidth: 8
                tailHeight: 11
                skin: root.itemId
                fillTop: Theme.outgoingTop
                fillBottom: Theme.outgoingBottom
                strokeColor: Theme.outgoingBorder
            }
            Text {
                x: 10
                anchors.verticalCenter: parent.verticalCenter
                text: "Hey!"
                color: skinned.skinned ? skinned.skinTextColor : Theme.textPrimary
                style: skinned.skinned ? Text.Raised : Text.Normal
                styleColor: skinned.skinned ? skinned.skinTextShadowColor : "transparent"
                font.family: Theme.uiFont
                font.pixelSize: 13
            }
        }
    }

    Component {
        id: frame
        // Room for the frame reaching past the picture on every side.
        Item {
            width: 56
            height: 56
            Avatar {
                anchors.centerIn: parent
                width: 38
                height: 38
                avatarKey: "userpfp_none"
                frameId: root.itemId
                frameAnimated: false
            }
        }
    }

    Component {
        id: bead
        Item {
            width: 44
            height: 44
            PresenceBead {
                anchors.centerIn: parent
                presence: 0
                beadSize: 32
                styleId: root.itemId
            }
        }
    }

    Component {
        id: flair
        Item {
            width: label.width + 16
            height: 44
            // Measures the name; the flair draws it.
            Text {
                id: label
                anchors.centerIn: parent
                text: "Name"
                opacity: 0
                font.family: Theme.uiFont
                font.pixelSize: 21
            }
            NameFlair {
                flairId: root.itemId
                text: label.text
                font: label.font
                textColor: Theme.textPrimary
                darkMode: Theme.darkMode
                textWidth: label.width
                elided: false
                x: label.x - padding
                y: label.y - padding
                width: label.width + 2 * padding
                height: label.height + 2 * padding
            }
        }
    }

    Component {
        id: scene
        // A window onto the header backdrop at its real proportions.
        Rectangle {
            width: 84
            height: 46
            radius: 4
            clip: true
            color: Theme.contentBackground
            border.color: Theme.rule
            ProfileScene {
                x: 1
                y: 1
                width: parent.width - 2
                height: parent.height - 2
                sceneId: root.itemId
                darkMode: Theme.darkMode
                fadeHeight: 0
            }
        }
    }
}
