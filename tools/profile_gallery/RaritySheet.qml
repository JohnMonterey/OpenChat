import QtQuick
import OpenChat
import OpenChat.Native

// Every collectible on its rung of the rarity ladder, drawn by the real
// case tile, with each tier's odds and each item's own chance.
Window {
    id: sheet
    visible: true
    width: content.width + 40
    height: content.height + 36
    title: "Rarity ladder"

    readonly property var tiers: Cosmetics.tiers()
    readonly property var everything: Cosmetics.items("")
    function membersOf(tierId) {
        return everything.filter(item => item.rarity === tierId);
    }
    function ink(color) { return Theme.darkMode ? Qt.lighter(color, 1.3) : Qt.darker(color, 1.3) }

    SheetBackground {}

    Column {
        id: content
        x: 20
        y: 16
        spacing: 14

        SheetTitle {
            title: "Rarity ladder"
            subtitle: "All " + sheet.everything.length + " collectibles as the hourly case shows them. A draw picks a tier by its odds, then one of its items evenly."
        }

        Repeater {
            model: sheet.tiers
            Row {
                id: rung
                required property var modelData
                readonly property var members: sheet.membersOf(modelData.id)
                spacing: 14

                Column {
                    width: 150
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 3
                    Row {
                        spacing: 7
                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 11; height: 11; radius: 5.5
                            color: rung.modelData.color
                        }
                        Text {
                            text: rung.modelData.name
                            color: sheet.ink(rung.modelData.color)
                            font.family: Theme.uiFont
                            font.pixelSize: 18
                            font.bold: true
                        }
                    }
                    Text {
                        text: rung.modelData.percent + "% of draws"
                        color: Theme.textPrimary
                        font.family: Theme.uiFont
                        font.pixelSize: 12
                    }
                    Text {
                        text: rung.members.length + " items · "
                              + (rung.modelData.percent / rung.members.length).toFixed(2) + "% each"
                        color: Theme.textSecondary
                        font.family: Theme.uiFont
                        font.pixelSize: 12
                    }
                }

                Row {
                    spacing: 10
                    Repeater {
                        model: rung.members
                        CaseTile {
                            required property var modelData
                            width: 104
                            height: 128
                            item: modelData
                        }
                    }
                }
            }
        }
    }
}
