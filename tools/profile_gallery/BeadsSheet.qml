import QtQuick
import OpenChat
import OpenChat.Native

Window {
    id: sheet
    visible: true
    width: content.width + 40
    height: content.height + 36
    title: "Presence beads"

    readonly property var states: [0, 1, 3, 2]
    readonly property var stateNames: ["Available", "Away", "Busy", "Offline"]
    readonly property var rows: [{ id: "", name: "Stock bead", description: "The glossy Aero bead the app ships with." }]
                                .concat(Cosmetics.items("bead"))

    SheetBackground {}

    Column {
        id: content
        x: 20
        y: 16
        spacing: 4

        SheetTitle {
            title: "Presence beads"
            subtitle: "The real PresenceBead component. Left: 11 px after a 16 px name, as in the lists. Middle: 12 px. Right: 36 px, then the real 11 px pixels magnified 5x (nearest-neighbour)."
        }

        Row {
            Item { width: 200; height: 16 }
            Repeater {
                model: 4
                Text {
                    width: 96
                    text: sheet.stateNames[index]
                    color: Theme.textSecondary
                    font.family: Theme.uiFont
                    font.pixelSize: 11
                }
            }
        }

        Repeater {
            model: sheet.rows
            Row {
                id: beadRow
                required property var modelData
                spacing: 0
                height: 80

                ItemLabel {
                    width: 190
                    anchors.verticalCenter: parent.verticalCenter
                    name: beadRow.modelData.name
                    itemId: beadRow.modelData.id
                    description: beadRow.modelData.description
                }
                Item { width: 10; height: 1 }

                // In context: a contact-row name with the bead after it.
                Repeater {
                    model: sheet.states
                    Item {
                        required property int modelData
                        width: 96
                        height: 80
                        Text {
                            id: sample
                            y: 22
                            text: "Sarah"
                            color: Theme.textPrimary
                            font.family: Theme.uiFont
                            font.pixelSize: 16
                            renderType: Text.NativeRendering
                        }
                        PresenceBead {
                            x: sample.width + 8
                            anchors.verticalCenter: sample.verticalCenter
                            anchors.verticalCenterOffset: 1
                            beadSize: 11
                            presence: parent.modelData
                            styleId: beadRow.modelData.id
                        }
                        Text {
                            y: 46
                            text: sheet.stateNames[sheet.states.indexOf(parent.modelData)]
                            color: Theme.textSecondary
                            font.family: Theme.uiFont
                            font.pixelSize: 13
                            renderType: Text.NativeRendering
                        }
                    }
                }
                Item { width: 16; height: 1 }

                Row {
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 10
                    Repeater {
                        model: sheet.states
                        PresenceBead {
                            required property int modelData
                            beadSize: 12
                            presence: modelData
                            styleId: beadRow.modelData.id
                        }
                    }
                }
                Item { width: 26; height: 1 }

                Row {
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 14
                    Repeater {
                        model: sheet.states
                        PresenceBead {
                            required property int modelData
                            beadSize: 36
                            presence: modelData
                            styleId: beadRow.modelData.id
                        }
                    }
                }
                Item { width: 26; height: 1 }

                // The real 11 px rendering, magnified so every pixel shows.
                Row {
                    objectName: "zoomSource:" + beadRow.modelData.id
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 2
                    Repeater {
                        model: sheet.states
                        Item {
                            required property int modelData
                            width: 17
                            height: 17
                            PresenceBead {
                                anchors.centerIn: parent
                                beadSize: 11
                                presence: parent.modelData
                                styleId: beadRow.modelData.id
                            }
                        }
                    }
                }
                Item { width: 12; height: 1 }
                Item {
                    objectName: "zoomTarget:" + beadRow.modelData.id
                    anchors.verticalCenter: parent.verticalCenter
                    width: (17 * 4 + 2 * 3) * 5
                    height: 17 * 5
                }
            }
        }
    }
}
