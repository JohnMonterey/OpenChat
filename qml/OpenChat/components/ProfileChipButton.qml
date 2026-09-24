import QtQuick
import OpenChat

// The 30-tall Aero chip of the profile chrome (Back, Copy, Undo/Redo, the
// Preview toggle): buttonBackground glass with a 1 px buttonBorder and a
// gloss line, an optional leading glyph and a label. Hover lifts the fill and
// rims it in focusBorder; keyboard focus is a 2 px focusBorder rim.
//
// A checkable chip reports the flip through toggled(!checked) and leaves
// `checked` to whoever owns the state (as AeroSwitch does), so a binding on
// it survives the click.
Item {
    id: chip
    property string label: ""
    property string glyph: ""
    property int glyphSize: 15
    property int fontPixelSize: 14
    property bool checkable: false
    property bool checked: false
    // Glyph only (the Preview toggle below 900 px): 32 wide, the label moves
    // to the accessible name.
    property bool compact: false
    property string accessibleName: label
    // Shown on hover; a disabled chip explains itself here.
    property string tooltip: ""
    // Right-click, Shift+F10 and the Menu key ask for a context menu.
    property bool contextMenu: false
    readonly property bool hovered: area.containsMouse
    readonly property bool pressed: area.pressed
    readonly property bool showsLabel: label.length > 0 && !compact
    signal clicked()
    signal toggled(bool checked)
    signal contextMenuRequested()

    function activate() {
        if (!chip.enabled)
            return;
        if (chip.checkable)
            chip.toggled(!chip.checked);
        chip.clicked();
    }

    implicitWidth: compact && glyph.length > 0 ? 32 : content.implicitWidth + (showsLabel ? 24 : 14)
    implicitHeight: 30
    width: implicitWidth
    height: implicitHeight
    opacity: enabled ? 1 : 0.55
    activeFocusOnTab: true

    Accessible.role: Accessible.Button
    Accessible.name: chip.accessibleName
    Accessible.description: chip.tooltip
    Accessible.checkable: chip.checkable
    Accessible.checked: chip.checked
    Accessible.onPressAction: chip.activate()

    Keys.onPressed: event => {
        if (event.key === Qt.Key_Space || event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            chip.activate();
            event.accepted = true;
        } else if (chip.contextMenu && (event.key === Qt.Key_Menu
                   || (event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier)))) {
            chip.contextMenuRequested();
            event.accepted = true;
        }
    }

    Rectangle {
        anchors.fill: parent
        radius: 4
        color: chip.checked ? Theme.navSelected : chip.hovered ? Theme.buttonHover : Theme.buttonBackground
        border.width: chip.activeFocus ? 2 : 1
        border.color: chip.activeFocus || chip.checked || (chip.hovered && chip.enabled) ? Theme.focusBorder
                                                                                         : Theme.buttonBorder
        Rectangle {
            x: 4
            y: 1
            width: parent.width - 8
            height: 1
            color: Theme.gloss
        }
    }

    Row {
        id: content
        anchors.centerIn: parent
        spacing: 6
        ProfileGlyph {
            visible: chip.glyph.length > 0
            anchors.verticalCenter: parent.verticalCenter
            width: chip.glyphSize
            height: chip.glyphSize
            kind: chip.glyph
            ink: Theme.iconInk
            stroke: 1.5
        }
        Text {
            visible: chip.showsLabel
            anchors.verticalCenter: parent.verticalCenter
            text: chip.label
            textFormat: Text.PlainText
            color: Theme.buttonText
            font.family: Theme.uiFont
            font.pixelSize: chip.fontPixelSize
            renderType: Text.NativeRendering
        }
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: chip.contextMenu ? Qt.LeftButton | Qt.RightButton : Qt.LeftButton
        cursorShape: chip.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
        onClicked: mouse => {
            if (mouse.button === Qt.RightButton) {
                chip.contextMenuRequested();
                return;
            }
            chip.activate();
        }
    }

    ProfileTip {
        parent: chip
        text: chip.tooltip
        visible: chip.tooltip.length > 0 && chip.hovered
        y: chip.height + 4
    }
}
