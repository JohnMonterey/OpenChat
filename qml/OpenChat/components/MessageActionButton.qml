import QtQuick
import QtQuick.Shapes
import OpenChat

// One bare glyph in the row under a hovered message: copy, edit, reply or
// save. The glyph is drawn small so the row fits in the gap every message
// already leaves below it; the whole slot around it, wider and taller than
// the glyph, takes the click. The row names the hovered action beside itself.
Item {
    id: action
    // "copy", "edit", "reply" or "save".
    property string icon: "copy"
    // What the action does, for the row's caption and for screen readers.
    property string label: ""
    readonly property bool hovered: actionMouse.containsMouse
    // Where the glyph sits in the slot. Near the top, it reads as belonging
    // to the message above and leaves the next one room.
    readonly property real glyphTop: 3
    readonly property real glyphSize: 11
    signal clicked

    width: 22
    height: 20
    Accessible.role: Accessible.Button
    Accessible.name: label
    Accessible.onPressAction: action.clicked()

    Shape {
        anchors.horizontalCenter: parent.horizontalCenter
        y: action.glyphTop
        width: action.glyphSize
        height: action.glyphSize
        ShapePath {
            fillColor: "transparent"
            strokeColor: action.hovered ? Theme.textPrimary : Theme.timestampText
            strokeWidth: 1.25
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            PathSvg {
                path: action.icon === "edit"
                      // A pencil, point down to the left.
                      ? "M 1 10 L 1.6 7.6 L 7.8 1.4 L 9.6 3.2 L 3.4 9.4 Z M 6.6 2.6 L 8.4 4.4"
                      : action.icon === "reply"
                        // An arrow turning back to the left.
                        ? "M 4.2 1.8 L 1 5 L 4.2 8.2 M 1 5 H 6.5 A 3.5 3.5 0 0 1 10 8.5 V 10"
                        : action.icon === "save"
                          // An arrow down into a tray.
                          ? "M 5.5 1 V 7 M 2.8 4.4 L 5.5 7 L 8.2 4.4 M 1 7.4 V 10 H 10 V 7.4"
                          // Two sheets, one behind the other.
                          : "M 1 3.6 H 7.4 V 10 H 1 Z M 3.6 3.6 V 1 H 10 V 7.4 H 7.4"
            }
        }
    }

    MouseArea {
        id: actionMouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: action.clicked()
    }
}
