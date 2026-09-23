import QtQuick
import OpenChat
import OpenChat.Native

// A contact sheet of every chat-bubble skin, drawn with the real
// MessageDelegate on the conversation background: for each skin, an incoming
// classic bubble for context and two of the local user's own bubbles wearing it.
Rectangle {
    id: gallery

    required property var skins
    readonly property int columns: 3
    readonly property int cellWidth: 400
    readonly property int spacing: 18

    width: columns * cellWidth + (columns - 1) * spacing + 48
    height: sheet.implicitHeight + 44
    gradient: Gradient {
        GradientStop { position: 0.0; color: Theme.contentBackground }
        GradientStop { position: 1.0; color: Theme.contentBottom }
    }

    Column {
        id: sheet
        x: 24
        y: 20
        spacing: 16

        Text {
            text: "Chat bubble skins"
            color: Theme.textPrimary
            font.family: Theme.uiFont
            font.pixelSize: 20
            renderType: Text.NativeRendering
        }

        Grid {
            columns: gallery.columns
            columnSpacing: gallery.spacing
            rowSpacing: gallery.spacing

            Repeater {
                model: gallery.skins

                delegate: Rectangle {
                    id: card
                    required property var modelData
                    width: gallery.cellWidth
                    height: cardColumn.implicitHeight + 14
                    radius: 8
                    color: "transparent"
                    border.color: Theme.softRule

                    Column {
                        id: cardColumn
                        y: 12
                        width: parent.width

                        Text {
                            x: 16
                            width: parent.width - 32
                            text: card.modelData.name
                            color: Theme.textPrimary
                            font.family: Theme.uiFont
                            font.pixelSize: 16
                            font.bold: true
                            renderType: Text.NativeRendering
                        }
                        Text {
                            x: 16
                            width: parent.width - 32
                            text: (card.modelData.id.length > 0 ? card.modelData.id : "classic")
                                  + "  ·  " + card.modelData.description
                            color: Theme.textSecondary
                            font.family: Theme.uiFont
                            font.pixelSize: 12
                            wrapMode: Text.Wrap
                            renderType: Text.NativeRendering
                        }
                        Item { width: 1; height: 8 }

                        MessageDelegate {
                            width: parent.width
                            deliveryState: 3
                            direction: 0
                            body: "Did you open today's case yet?"
                            timestamp: "10:15 AM"
                            kind: 0
                            dateLabel: ""
                            showDateDivider: false
                            senderName: ""
                        }
                        MessageDelegate {
                            width: parent.width
                            deliveryState: 3
                            direction: 1
                            body: "Just did! Check out my new bubble, the detail up close is unreal."
                            timestamp: "10:16 AM"
                            kind: 0
                            dateLabel: ""
                            showDateDivider: false
                            senderName: ""
                            bubbleSkin: card.modelData.id
                        }
                        MessageDelegate {
                            width: parent.width
                            deliveryState: 3
                            direction: 1
                            body: "What do you think?"
                            timestamp: "10:16 AM"
                            kind: 0
                            dateLabel: ""
                            showDateDivider: false
                            senderName: ""
                            bubbleSkin: card.modelData.id
                        }
                    }
                }
            }
        }
    }
}
