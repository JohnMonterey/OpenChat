import QtQuick
import OpenChat

// The list the "+" next to a chat's name opens: every contact who can be
// added. Picking one starts a group (from a one-to-one chat) or adds them to
// the open group. Lives on the window's content item so it floats over the
// conversation, and closes on any click elsewhere.
Item {
    id: picker
    objectName: "groupMemberPicker"
    required property var controller
    // Where the picker's top-left goes, in window coordinates.
    property real anchorX: 0
    property real anchorY: 0
    readonly property var candidates: controller.groupCandidates
    visible: false
    z: 20
    x: anchorX
    y: anchorY
    width: 236
    height: column.height + 12

    function toggle() {
        visible = !visible;
    }

    // Click anywhere else to close.
    MouseArea {
        parent: picker.parent
        anchors.fill: parent
        visible: picker.visible
        z: 19
        onClicked: picker.visible = false
    }

    Rectangle {
        anchors.fill: parent
        radius: 5
        color: Theme.contentBackground
        border.width: 1
        border.color: Theme.inputBorder
    }
    Rectangle { x: 6; y: 1; width: parent.width - 12; height: 1; color: Theme.gloss }

    Column {
        id: column
        x: 6
        y: 6
        width: parent.width - 12

        Text {
            width: parent.width
            height: 26
            leftPadding: 6
            verticalAlignment: Text.AlignVCenter
            text: picker.controller.currentIsGroup ? "Add to this group" : "Start a group with…"
            color: Theme.categoryText
            font.family: Theme.uiFont
            font.pixelSize: 13
            font.bold: true
            renderType: Text.NativeRendering
        }

        Text {
            objectName: "groupPickerEmpty"
            width: parent.width
            height: 30
            leftPadding: 6
            verticalAlignment: Text.AlignVCenter
            visible: picker.candidates.length === 0
            text: "No one else to add yet"
            color: Theme.textSecondary
            font.family: Theme.uiFont
            font.pixelSize: 13
            renderType: Text.NativeRendering
        }

        Repeater {
            model: picker.candidates

            Item {
                id: candidateRow
                required property var modelData
                objectName: "groupCandidate_" + modelData.contactId
                width: column.width
                height: 34

                Rectangle {
                    anchors.fill: parent
                    radius: 3
                    color: candidateMouse.containsMouse ? Theme.selectedTop : "transparent"
                }
                Avatar {
                    x: 6
                    anchors.verticalCenter: parent.verticalCenter
                    width: 24
                    height: 24
                    cornerRadius: 4
                    avatarKey: candidateRow.modelData.avatarKey
                }
                Text {
                    x: 38
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - x - 8
                    elide: Text.ElideRight
                    text: candidateRow.modelData.name
                    color: Theme.textPrimary
                    font.family: Theme.uiFont
                    font.pixelSize: 14
                    renderType: Text.NativeRendering
                }
                MouseArea {
                    id: candidateMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        picker.visible = false;
                        picker.controller.addToGroup(candidateRow.modelData.contactId);
                    }
                }
            }
        }
    }
}
