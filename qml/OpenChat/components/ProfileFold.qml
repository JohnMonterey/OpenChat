import QtQuick
import OpenChat

// A fold in the About me tab (SPEC §14.11): a 34 px button row, "Interests"
// with "4 of 6 filled" and a chevron, over its fields, which show only while
// the fold is open. The row is one Tab stop; Enter or Space opens and closes it.
Item {
    id: fold

    property string label: ""
    property string note: ""
    property bool expanded: false
    default property alias content: body.data

    implicitWidth: 240
    implicitHeight: header.height + (expanded ? body.implicitHeight + 8 : 0)
    height: implicitHeight

    function toggle() {
        expanded = !expanded;
    }

    Rectangle {
        id: header
        width: parent.width
        height: 34
        radius: 5
        color: headerMouse.containsMouse ? Theme.buttonHover : Theme.buttonBackground
        border.width: headerFocus.activeFocus ? 2 : 1
        border.color: headerFocus.activeFocus ? Theme.focusBorder : Theme.buttonBorder
        Accessible.role: Accessible.Button
        Accessible.name: fold.label + ", " + fold.note + (fold.expanded ? ", expanded" : ", collapsed")

        Rectangle { x: 4; y: 1; width: parent.width - 8; height: 1; color: Theme.gloss }
        Text {
            x: 12
            anchors.verticalCenter: parent.verticalCenter
            text: fold.label
            color: Theme.textPrimary
            font.family: Theme.uiFont
            font.pixelSize: 13
            renderType: Text.NativeRendering
        }
        Row {
            anchors.right: parent.right
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            spacing: 6
            Text {
                objectName: "profileFoldNote"
                anchors.verticalCenter: parent.verticalCenter
                text: fold.note
                color: Theme.textSecondaryStrong
                font.family: Theme.uiFont
                font.pixelSize: 11
                renderType: Text.NativeRendering
            }
            ProfileEditorRail.Glyph {
                anchors.verticalCenter: parent.verticalCenter
                width: 14
                height: 14
                kind: fold.expanded ? "up" : "down"
                ink: Theme.categoryChevron
                stroke: 1.6
            }
        }
        Item {
            id: headerFocus
            anchors.fill: parent
            activeFocusOnTab: true
            Keys.onReturnPressed: fold.toggle()
            Keys.onEnterPressed: fold.toggle()
            Keys.onSpacePressed: fold.toggle()
        }
        MouseArea {
            id: headerMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: fold.toggle()
        }
    }

    Column {
        id: body
        visible: fold.expanded
        y: header.height + 8
        width: parent.width
        spacing: 8
    }
}
