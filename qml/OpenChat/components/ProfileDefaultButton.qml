import QtQuick
import OpenChat

// The Windows 7 default button (SPEC §18.1 defaultButton* tokens): pale blue
// glass split at the middle, a focusBorder rim (2 px with keyboard focus), an
// inner light ring and a bold dark label. Save, Edit profile, Keep editing
// and Send contact request wear it, so the one action that moves you on is
// always the same object.
Item {
    id: button
    property string label: ""
    property string glyph: ""
    property int glyphSize: 15
    property int fontPixelSize: 14
    property string accessibleName: label
    readonly property bool hovered: area.containsMouse
    readonly property bool pressed: area.pressed
    signal clicked()

    function activate() {
        if (button.enabled)
            button.clicked();
    }

    implicitWidth: content.implicitWidth + 24
    implicitHeight: 30
    width: implicitWidth
    height: implicitHeight
    opacity: enabled ? 1 : 0.55
    activeFocusOnTab: true

    Accessible.role: Accessible.Button
    Accessible.name: button.accessibleName
    Accessible.onPressAction: button.activate()

    Keys.onPressed: event => {
        if (event.key === Qt.Key_Space || event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            button.activate();
            event.accepted = true;
        }
    }

    Rectangle {
        anchors.fill: parent
        radius: 4
        border.width: button.activeFocus ? 2 : 1
        border.color: Theme.focusBorder
        gradient: Gradient {
            GradientStop {
                position: 0
                color: button.hovered && button.enabled ? Qt.lighter(Theme.defaultButtonTop, 1.04)
                                                        : Theme.defaultButtonTop
            }
            GradientStop { position: 0.5; color: Qt.darker(Theme.defaultButtonTop, 1.02) }
            GradientStop { position: 0.51; color: Qt.lighter(Theme.defaultButtonBottom, 1.03) }
            GradientStop {
                position: 1
                color: button.pressed ? Qt.darker(Theme.defaultButtonBottom, 1.04) : Theme.defaultButtonBottom
            }
        }
        Rectangle {
            anchors.fill: parent
            anchors.margins: button.activeFocus ? 2 : 1
            radius: 3
            color: "transparent"
            border.width: 1
            border.color: Theme.defaultButtonInner
        }
    }

    Row {
        id: content
        anchors.centerIn: parent
        spacing: 6
        ProfileGlyph {
            visible: button.glyph.length > 0
            anchors.verticalCenter: parent.verticalCenter
            width: button.glyphSize
            height: button.glyphSize
            kind: button.glyph
            ink: Theme.defaultButtonText
            stroke: 1.5
        }
        Text {
            visible: button.label.length > 0
            anchors.verticalCenter: parent.verticalCenter
            text: button.label
            textFormat: Text.PlainText
            color: Theme.defaultButtonText
            font.family: Theme.uiFont
            font.pixelSize: button.fontPixelSize
            font.bold: true
            renderType: Text.NativeRendering
        }
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: button.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
        onClicked: button.activate()
    }
}
