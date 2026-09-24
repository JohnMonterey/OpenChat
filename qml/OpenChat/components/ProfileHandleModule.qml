import QtQuick
import OpenChat

// "OpenChat handle:" in MySpace's "MySpace URL:" slot (SPEC §5.3): the
// relay-confirmed handle (never the page's claim), selectable, with a Copy
// chip that reads "✓ Copied" for 1.5 s. Hidden while the handle is unknown.
// The words and the chip are the app's, in its own face.
ProfileBox {
    id: module
    objectName: "profileHandleBox"
    property var view: null
    readonly property var profiles: view ? view.profiles : null
    readonly property string handle: profiles ? profiles.personHandle : ""
    readonly property bool inert: view ? view.preview : false
    readonly property color ink: render ? render.linkColor : "#1f6fa3"
    readonly property int bodySize: render ? render.bodyPixelSize : 13
    property bool copied: false

    render: view ? view.render : null
    pad: 10
    // Hidden while the handle is unknown (the view reads this).
    readonly property bool present: handle.length > 0

    function copy() {
        if (module.inert || !module.profiles)
            return;
        module.profiles.copyHandle();
        module.copied = true;
        copiedTimer.restart();
    }

    Timer {
        id: copiedTimer
        interval: 1500
        onTriggered: module.copied = false
    }

    Item {
        width: parent.width
        height: 36

        Column {
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - chip.width - 8
            spacing: 2
            Text {
                text: "OpenChat handle:"
                textFormat: Text.PlainText
                color: module.render ? module.render.labelColor : "black"
                style: module.render && module.render.textHalo ? Text.Outline : Text.Normal
                styleColor: module.render ? module.render.haloColor : "transparent"
                font.family: Theme.uiFont
                font.pixelSize: module.bodySize - 1
                font.bold: true
                renderType: Text.NativeRendering
            }
            TextEdit {
                objectName: "profileHandleText"
                width: Math.min(implicitWidth, parent.width)
                readOnly: true
                selectByMouse: !module.inert
                activeFocusOnPress: !module.inert
                text: "@" + module.handle
                textFormat: TextEdit.PlainText
                color: module.ink
                selectionColor: Theme.selectionBackground
                selectedTextColor: Theme.selectionText
                font.family: Theme.uiFont
                font.pixelSize: module.bodySize + 1
                renderType: Text.NativeRendering
                Accessible.name: "OpenChat handle @" + module.handle
            }
        }

        Item {
            id: chip
            objectName: "profileCopyHandleButton"
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            width: chipRow.implicitWidth + 16
            height: 26
            activeFocusOnTab: !module.inert

            Accessible.role: Accessible.Button
            Accessible.name: module.copied ? "Copied @" + module.handle : "Copy @" + module.handle
            Accessible.onPressAction: module.copy()
            Keys.onPressed: event => {
                if (event.key === Qt.Key_Space || event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                    module.copy();
                    event.accepted = true;
                }
            }

            Rectangle {
                anchors.fill: parent
                radius: 4
                color: Qt.rgba(module.ink.r, module.ink.g, module.ink.b,
                               chipArea.pressed ? 0.22 : chipArea.containsMouse ? 0.16 : 0.10)
                border.width: 1
                border.color: Qt.rgba(module.ink.r, module.ink.g, module.ink.b, 0.45)
            }
            Row {
                id: chipRow
                anchors.centerIn: parent
                spacing: 5
                ProfileGlyph {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 13
                    height: 13
                    kind: module.copied ? "check" : "copy"
                    ink: module.ink
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: module.copied ? "Copied" : "Copy"
                    textFormat: Text.PlainText
                    color: module.ink
                    font.family: Theme.uiFont
                    font.pixelSize: 12
                    renderType: Text.NativeRendering
                }
            }
            ProfileFocusRing {
                anchors.fill: parent
                shown: chip.activeFocus
                radius: 4
                onDark: module.render ? module.render.boxDark : false
            }
            MouseArea {
                id: chipArea
                anchors.fill: parent
                enabled: !module.inert
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: module.copy()
            }
        }
    }
}
