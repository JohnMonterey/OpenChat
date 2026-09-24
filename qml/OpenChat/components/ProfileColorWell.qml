import QtQuick
import OpenChat

// A colour well (SPEC §14.3): a sunken 30 px field with a 34×22 swatch and the
// hex value, under an optional 12 px label. Text colours (`inkRole` ≥ 0, a
// Profile.InkRole) carry a readability badge: a green check while the page
// shows the colour as picked, an amber sparkle once the renderer shows it
// adjusted (`render.inkAdjusted`, the same truth the preview paints with).
// Clicking, Enter or Space opens the editor's colour picker at the well;
// the picker reports each colour through `picked`.
Item {
    id: well

    property string label: ""
    property color color: "#ffffff"
    property int inkRole: -1
    property var page: null     // the draft (its render style names adjusted inks)
    property Item editor: null  // the ProfileEditor, which owns the picker
    property string pickerTitle: label
    signal picked(color color)

    readonly property bool isOpen: editor !== null && editor.colorPickerWell === well
    readonly property bool adjusted: inkRole >= 0 && page !== null && page.render !== null
                                     && page.render.inkAdjusted[String(inkRole)] === true

    function openPicker() {
        if (editor && enabled)
            editor.openColorPicker(well);
    }

    implicitWidth: 132
    implicitHeight: label.length > 0 ? 50 : 30
    height: implicitHeight
    opacity: enabled ? 1 : 0.55
    activeFocusOnTab: true
    Accessible.role: Accessible.Button
    Accessible.name: (label.length > 0 ? label : pickerTitle) + ", " + String(color).toUpperCase()
                     + (inkRole >= 0 ? (adjusted ? ", adjusted for readability" : ", easy to read") : "")
    Accessible.onPressAction: openPicker()
    Keys.onReturnPressed: openPicker()
    Keys.onEnterPressed: openPicker()
    Keys.onSpacePressed: openPicker()

    Text {
        visible: well.label.length > 0
        width: parent.width
        elide: Text.ElideRight
        text: well.label
        color: Theme.textSecondaryStrong
        font.family: Theme.uiFont
        font.pixelSize: 12
        renderType: Text.NativeRendering
    }

    Rectangle {
        id: frame
        objectName: "profileColorWellFrame"
        y: well.label.length > 0 ? 20 : 0
        width: parent.width
        height: 30
        radius: 4
        color: Theme.fieldBackground
        border.width: well.isOpen || well.activeFocus ? 2 : 1
        border.color: well.isOpen || well.activeFocus ? Theme.focusBorder : Theme.inputBorder
        Rectangle { x: 4; y: 1; width: parent.width - 8; height: 1; color: Theme.insetTop }

        Rectangle {
            id: swatch
            x: 4
            y: 4
            width: Math.min(34, parent.width - 8)
            height: 22
            radius: 3
            color: well.color
            border.width: 1
            border.color: "#40000000"
            Rectangle { x: 2; y: 1; width: parent.width - 4; height: 1; color: "#50ffffff" }
        }
        Text {
            visible: well.width >= 110
            x: swatch.x + swatch.width + 7
            anchors.verticalCenter: parent.verticalCenter
            text: String(well.color).toUpperCase()
            color: Theme.textPrimary
            font.family: Theme.uiFont
            font.pixelSize: 12
            renderType: Text.NativeRendering
        }
        Rectangle {
            objectName: "profileColorWellBadge"
            readonly property bool warning: well.adjusted
            visible: well.inkRole >= 0
            anchors.right: parent.right
            anchors.rightMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            width: 20
            height: 20
            radius: 10
            color: warning ? Theme.warningBackground : Theme.successFill
            border.width: 1
            border.color: warning ? Theme.warningBorder : Qt.darker(Theme.successFill, 1.2)
            ProfileEditorRail.Glyph {
                anchors.centerIn: parent
                width: 12
                height: 12
                kind: parent.warning ? "sparkle" : "check"
                ink: parent.warning ? Theme.warningText : "#ffffff"
            }
        }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: well.openPicker()
        }
    }
}
