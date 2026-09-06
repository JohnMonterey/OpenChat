import QtQuick
import QtQuick.Shapes
import OpenChat

// A small glass chip with one glyph, for controls that sit on top of a
// picture: the zoom on a camera or screen tile, the full-screen toggle in the
// corner of the call surface. Drawn as a dark chip with a light stroke so it
// reads over any video, light or dark, and over either theme's backdrop.
Item {
    id: chip
    // "zoom" (a magnifier with a plus), "expand" (four corners pointing out)
    // or "collapse" (four corners pointing in).
    property string icon: "zoom"
    property string tooltip: ""
    // Tooltips normally open above the chip; a chip on a bottom edge can ask
    // for the other side.
    property bool tooltipAbove: true
    signal clicked

    width: 22
    height: 22
    activeFocusOnTab: true
    Accessible.role: Accessible.Button
    Accessible.name: tooltip
    Accessible.onPressAction: chip.clicked()
    Keys.onSpacePressed: chip.clicked()
    Keys.onReturnPressed: chip.clicked()

    Rectangle {
        anchors.fill: parent
        radius: 5
        color: chipMouse.containsMouse ? Theme.mediaChipHover : Theme.mediaChip
        border.width: 1
        border.color: chip.activeFocus ? Theme.focusBorder : Theme.mediaChipBorder
    }

    Shape {
        anchors.centerIn: parent
        width: 14
        height: 14
        // Text-like strokes at 14px look best without antialiasing of the
        // whole item; the path itself is antialiased by the renderer.
        ShapePath {
            fillColor: "transparent"
            strokeColor: Theme.mediaChipGlyph
            strokeWidth: 1.6
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            PathSvg {
                path: chip.icon === "expand"
                      ? "M 1 5 V 1 H 5 M 9 1 H 13 V 5 M 13 9 V 13 H 9 M 5 13 H 1 V 9"
                      : chip.icon === "collapse"
                        ? "M 5 1 V 5 H 1 M 13 5 H 9 V 1 M 9 13 V 9 H 13 M 1 9 H 5 V 13"
                        : "M 1.5 6 A 4.5 4.5 0 1 0 10.5 6 A 4.5 4.5 0 1 0 1.5 6 "
                          + "M 6 3.6 V 8.4 M 3.6 6 H 8.4 M 9.3 9.3 L 13 13"
            }
        }
    }

    MouseArea {
        id: chipMouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: chip.clicked()
    }

    Rectangle {
        objectName: "mediaChipTooltip"
        visible: chip.tooltip.length > 0 && chipMouse.containsMouse
        anchors.bottom: chip.tooltipAbove ? parent.top : undefined
        anchors.top: chip.tooltipAbove ? undefined : parent.bottom
        anchors.margins: 6
        anchors.right: parent.right
        width: tooltipText.implicitWidth + 16
        height: tooltipText.implicitHeight + 10
        radius: 3
        color: Theme.contentBackground
        border.width: 1
        border.color: Theme.inputBorder
        z: 10

        Text {
            id: tooltipText
            anchors.centerIn: parent
            text: chip.tooltip
            color: Theme.textPrimary
            font.family: Theme.uiFont
            font.pixelSize: 12
            renderType: Text.NativeRendering
        }
    }
}
