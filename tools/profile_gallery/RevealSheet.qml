import QtQuick
import OpenChat
import OpenChat.Native

// The daily-case reveal frozen mid-burst for three tiers, so the tier colour,
// the heavier ring and the top tiers' flash and echo can be judged still.
Window {
    id: sheet
    visible: true
    width: content.width + 40
    height: content.height + 36
    title: "Case reveals"

    readonly property var winners: ["frame.gilded", "scene.aurora", "bubble.magma"]

    SheetBackground {}

    Column {
        id: content
        x: 20
        y: 16
        spacing: 14

        SheetTitle {
            title: "Case reveals"
            subtitle: "The real CaseReel, settled on a winner and frozen part-way through the reveal burst. Legendary and Exotic add a flash and an echo ring."
        }

        Repeater {
            model: sheet.winners
            Column {
                id: entry
                required property string modelData
                readonly property var reward: Cosmetics.item(modelData)
                spacing: 4
                Text {
                    text: entry.reward.name + " · " + entry.reward.rarityName
                    color: Theme.textPrimary
                    font.family: Theme.uiFont
                    font.pixelSize: 13
                }
                Rectangle {
                    width: 680
                    height: 198
                    color: Theme.contentBackground
                    CaseReel {
                        anchors.fill: parent
                        impact: 0.3
                        echo: 0.25
                        // Stands in for DailyCaseController: settled on the winner.
                        controller: QtObject {
                            property var reward: entry.reward
                            property var fillers: Cosmetics.items("")
                            property int winnerIndex: 14
                            property real position: 14
                            property int state: DailyCaseController.OpenedToday
                            property int tileCount: 29
                            property bool reducedMotion: false
                            signal revealed()
                        }
                    }
                }
            }
        }
    }
}
