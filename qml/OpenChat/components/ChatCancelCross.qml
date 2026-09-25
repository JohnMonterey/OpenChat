import QtQuick
import QtQuick.Shapes
import OpenChat

// The small cross that stops one's own attachment on its way, at the end of
// a sound's or a file's row: bare, like the compose bar's.
Item {
    id: cross
    objectName: "cancelAttachment"
    property color ink: Theme.timestampText
    signal clicked

    width: 18
    height: 18
    Accessible.role: Accessible.Button
    Accessible.name: "Stop sending"
    Accessible.onPressAction: cross.clicked()

    Shape {
        anchors.centerIn: parent
        // Even, like the 18 px slot, so centring lands on whole pixels.
        width: 8
        height: 8
        opacity: crossMouse.containsMouse ? 1 : 0.7
        ShapePath {
            fillColor: "transparent"
            strokeColor: cross.ink
            strokeWidth: 1.4
            capStyle: ShapePath.RoundCap
            PathSvg { path: "M 0 0 L 8 8 M 8 0 L 0 8" }
        }
    }
    MouseArea {
        id: crossMouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: cross.clicked()
    }
}
