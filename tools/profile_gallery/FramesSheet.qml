import QtQuick
import OpenChat
import OpenChat.Native

Window {
    id: sheet
    visible: true
    width: content.width + 40
    height: content.height + 36
    title: "Avatar frames"

    readonly property var sizes: [32, 44, 74, 110]
    readonly property var keys: ["alex", "michael", "jessica", "ryan"]
    readonly property var captions: ["32 px", "44 px · sidebar", "74 px · call", "110 px"]
    readonly property var rows: [{ id: "", name: "None", description: "The stock picture with its 1 px border." }]
                                .concat(Cosmetics.items("frame"))

    SheetBackground {}

    Column {
        id: content
        x: 20
        y: 16
        spacing: 6

        SheetTitle {
            title: "Avatar frames"
            subtitle: "Rendered by the real Avatar component (and CallParticipant, speaking) at the sizes the app uses."
        }

        Row {
            spacing: 0
            Item { width: 200; height: 18 }
            Repeater {
                model: sheet.sizes.length
                Text {
                    width: sheet.sizes[index] + 44
                    horizontalAlignment: Text.AlignHCenter
                    text: sheet.captions[index]
                    color: Theme.textSecondary
                    font.family: Theme.uiFont
                    font.pixelSize: 11
                }
            }
            Text {
                width: 150
                horizontalAlignment: Text.AlignHCenter
                text: "call tile · speaking"
                color: Theme.textSecondary
                font.family: Theme.uiFont
                font.pixelSize: 11
            }
        }

        Repeater {
            model: sheet.rows
            Row {
                required property var modelData
                height: 170
                spacing: 0
                Rectangle { width: 0; height: 1; color: "transparent" }
                ItemLabel {
                    width: 190
                    anchors.verticalCenter: parent.verticalCenter
                    name: modelData.name
                    itemId: modelData.id
                    description: modelData.description
                }
                Item { width: 10; height: 1 }
                Repeater {
                    model: sheet.sizes.length
                    Item {
                        required property int index
                        width: sheet.sizes[index] + 44
                        height: 170
                        Avatar {
                            anchors.centerIn: parent
                            anchors.verticalCenterOffset: 4
                            width: sheet.sizes[index]
                            height: width
                            cornerRadius: sheet.sizes[index] >= 70 ? 6 : 5
                            avatarKey: sheet.keys[index]
                            frameId: modelData.id
                            frameAnimated: false
                        }
                    }
                }
                Item {
                    width: 150
                    height: 170
                    CallParticipant {
                        anchors.centerIn: parent
                        anchors.verticalCenterOffset: 6
                        name: "Daniel"
                        avatarKey: "userpfp_none"
                        speaking: true
                        level: 0.3
                        frameId: modelData.id
                    }
                }
            }
        }
    }
}
