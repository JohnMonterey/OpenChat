import QtQuick
import OpenChat
import OpenChat.Native

Window {
    id: sheet
    visible: true
    width: content.width + 40
    height: content.height + 36
    title: "Profile scenes"

    readonly property var rows: [{ id: "", name: "No scene", description: "The stock sidebar header." }]
                                .concat(Cosmetics.items("scene"))
    // A matching loadout for each scene, shown beside it.
    readonly property var loadouts: ({
        "": ["", "", ""],
        "scene.aurora": ["frame.frost", "bead.gem", "flair.aero"],
        "scene.meadow": ["frame.aero", "bead.heart", "flair.gold"],
        "scene.aqua": ["frame.aero", "bead.orb", "flair.chrome"],
        "scene.synthwave": ["frame.neon", "bead.planet", "flair.neon"],
        "scene.sakura": ["frame.gilded", "bead.heart", "flair.holo"]
    })

    SheetBackground {}

    Column {
        id: content
        x: 20
        y: 16
        spacing: 8

        SheetTitle {
            title: "Profile scenes"
            subtitle: "The real ContactSidebar header with the search field. Left: the scene alone. Right: with a matching loadout."
        }

        Repeater {
            model: sheet.rows
            Row {
                id: sceneRow
                required property var modelData
                spacing: 18
                height: 112

                ItemLabel {
                    width: 190
                    anchors.verticalCenter: parent.verticalCenter
                    name: sceneRow.modelData.name
                    itemId: sceneRow.modelData.id
                    description: sceneRow.modelData.description
                }
                Repeater {
                    model: 2
                    Rectangle {
                        required property int index
                        width: Theme.sidebarWidth
                        height: 112
                        clip: true
                        color: "transparent"
                        border.width: 1
                        border.color: Theme.rule
                        ContactSidebar {
                            width: Theme.sidebarWidth
                            height: 400
                            controller: galleryShortName
                            profileScene: sceneRow.modelData.id
                            avatarFrame: parent.index === 1 ? sheet.loadouts[sceneRow.modelData.id][0] : ""
                            presenceBead: parent.index === 1 ? sheet.loadouts[sceneRow.modelData.id][1] : ""
                            nameFlair: parent.index === 1 ? sheet.loadouts[sceneRow.modelData.id][2] : ""
                        }
                    }
                }
            }
        }
    }
}
