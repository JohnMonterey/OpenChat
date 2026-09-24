import QtQuick
import QtQuick.Controls
import OpenChat

// The top bar's Back chip (SPEC §1.2, §2): labelled with where it goes
// ("‹ Chat", "‹ Michael"). A click pops one entry through the page. Right
// click, Shift+F10 or the Menu key opens the history, newest first, each row
// with a 20 px picture; choosing one pops straight back to it.
ProfileChipButton {
    id: back
    objectName: "profileBackButton"
    property var profiles: null
    readonly property bool menuOpen: history.visible
    // The page's handleBack(): the chip never pops by itself, so the page
    // can fade the body through the move.
    signal backRequested()
    signal historyChosen(int index)

    function openHistory() {
        if (!back.profiles || back.profiles.history.length === 0)
            return;
        history.popup(back, 0, back.height + 2);
    }

    glyph: "back"
    glyphSize: 14
    label: profiles ? profiles.backLabel : ""
    accessibleName: "Back to " + label
    contextMenu: true
    onClicked: back.backRequested()
    onContextMenuRequested: back.openHistory()

    AeroMenu {
        id: history
        objectName: "profileHistoryMenu"
        width: 240

        Instantiator {
            model: back.profiles ? back.profiles.history : []
            delegate: MenuItem {
                id: entry
                required property var modelData
                objectName: "profileHistoryItem"
                implicitWidth: 230
                implicitHeight: 32
                leftPadding: 8
                rightPadding: 12
                hoverEnabled: true
                text: modelData.name
                onTriggered: back.historyChosen(entry.modelData.index)

                contentItem: Row {
                    spacing: 8
                    Avatar {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 20
                        height: 20
                        cornerRadius: 3
                        avatarKey: entry.modelData.avatarKey || "userpfp_none"
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        width: entry.availableWidth - 28
                        elide: Text.ElideRight
                        text: entry.text
                        textFormat: Text.PlainText
                        color: Theme.textPrimary
                        font.family: Theme.uiFont
                        font.pixelSize: 13
                        renderType: Text.NativeRendering
                    }
                }
                background: Rectangle {
                    radius: 3
                    color: entry.highlighted ? Theme.navSelected : "transparent"
                    border.width: entry.highlighted ? 1 : 0
                    border.color: Theme.focusBorder
                }
            }
            onObjectAdded: (index, object) => history.insertItem(index, object)
            onObjectRemoved: (index, object) => history.removeItem(object)
        }
    }
}
