import QtQuick
import OpenChat
import OpenChat.Native

Window {
    id: sheet
    visible: true
    width: content.width + 40
    height: content.height + 36
    title: "Name flair"

    readonly property var rows: [{ id: "", name: "Plain name", description: "The stock 17 px name." }]
                                .concat(Cosmetics.items("flair"))

    SheetBackground {}

    Column {
        id: content
        x: 20
        y: 16
        spacing: 6

        SheetTitle {
            title: "Name flair"
            subtitle: "The real ContactSidebar header (clipped), a short name and one that must elide before the bead."
        }

        Repeater {
            model: sheet.rows
            Row {
                id: flairRow
                required property var modelData
                spacing: 18
                height: 70

                ItemLabel {
                    width: 190
                    anchors.verticalCenter: parent.verticalCenter
                    name: flairRow.modelData.name
                    itemId: flairRow.modelData.id
                    description: flairRow.modelData.description
                }
                Repeater {
                    model: [galleryShortName, galleryLongName]
                    Item {
                        required property var modelData
                        width: Theme.sidebarWidth
                        height: 66
                        clip: true
                        ContactSidebar {
                            width: Theme.sidebarWidth
                            height: 400
                            controller: parent.modelData
                            avatarFrame: ""
                            presenceBead: ""
                            profileScene: ""
                            nameFlair: flairRow.modelData.id
                        }
                    }
                }
            }
        }
    }
}
