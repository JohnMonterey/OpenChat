import QtQuick
import OpenChat
import OpenChat.Native

// "Adjusted for readability" (SPEC §9, §14.8): the renderer's corrections that
// belong to one editor tab, said out loud in the editor only. The sentences
// are the renderer's own (render.adjustments), so the notice never guesses.
// "Show me" asks the editor to point at a box that shows the change.
Rectangle {
    id: notice
    objectName: "profileReadabilityNotice"

    property var page: null // the draft
    property int editorTab: Profile.TextTab
    signal showMe(int role)

    readonly property var entries: {
        const all = page && page.render ? page.render.adjustments : [];
        return all.filter(entry => entry.tab === notice.editorTab);
    }

    visible: entries.length > 0
    implicitWidth: 268
    implicitHeight: visible ? column.implicitHeight + 16 : 0
    height: implicitHeight
    radius: 5
    color: Theme.warningBackground
    border.width: 1
    border.color: Theme.warningBorder
    Accessible.role: Accessible.AlertMessage
    Accessible.name: "Adjusted for readability. " + entries.map(entry => entry.sentence).join(" ")

    ProfileEditorRail.Glyph {
        x: 10
        y: 8
        width: 16
        height: 16
        kind: "sparkle"
        ink: Theme.warningText
    }
    Column {
        id: column
        x: 34
        y: 8
        width: parent.width - 44
        spacing: 3
        Text {
            width: parent.width
            wrapMode: Text.Wrap
            text: "Adjusted for readability"
            color: Theme.warningText
            font.family: Theme.uiFont
            font.pixelSize: 13
            font.bold: true
            renderType: Text.NativeRendering
        }
        Repeater {
            model: notice.entries
            Text {
                required property var modelData
                width: column.width
                wrapMode: Text.Wrap
                text: modelData.sentence
                color: Theme.noticeText
                font.family: Theme.uiFont
                font.pixelSize: 12
                lineHeight: 1.05
                renderType: Text.NativeRendering
            }
        }
        Text {
            objectName: "profileReadabilityShowMe"
            text: "Show me"
            color: Theme.focusBorder
            font.family: Theme.uiFont
            font.pixelSize: 12
            font.underline: true
            renderType: Text.NativeRendering
            activeFocusOnTab: true
            Accessible.role: Accessible.Link
            Accessible.name: "Show me"
            Accessible.onPressAction: notice.showMe(notice.entries[0].role)
            Keys.onReturnPressed: notice.showMe(notice.entries[0].role)
            Keys.onEnterPressed: notice.showMe(notice.entries[0].role)
            Keys.onSpacePressed: notice.showMe(notice.entries[0].role)
            Rectangle {
                visible: parent.activeFocus
                anchors.fill: parent
                anchors.margins: -2
                radius: 2
                color: "transparent"
                border.width: 2
                border.color: Theme.focusBorder
            }
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: notice.showMe(notice.entries[0].role)
            }
        }
    }
}
