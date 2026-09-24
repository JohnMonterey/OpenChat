import QtQuick
import QtQuick.Shapes
import QtQuick.Window
import OpenChat
import OpenChat.Native

// The 12 px arc spinner beside "Getting Michael's page…". It turns on the
// shared profile ticker at 15 fps, only while it is on screen and animation is
// allowed; otherwise it holds one still frame (Low memory mode, a hidden or
// minimised window).
Item {
    id: spinner
    property color ink: Theme.categoryText
    property bool running: true
    readonly property bool turning: clock.active

    implicitWidth: 12
    implicitHeight: 12
    Accessible.ignored: true

    ProfileTickerClient {
        id: clock
        fps: 15
        active: spinner.running && spinner.visible && ProfileRenderPolicy.animationsAllowed
                && spinner.Window.window !== null && spinner.Window.window.visibility !== Window.Hidden
                && spinner.Window.window.visibility !== Window.Minimized
    }

    Shape {
        anchors.fill: parent
        rotation: (clock.frame * 30) % 360
        ShapePath {
            strokeColor: spinner.ink
            strokeWidth: Math.max(1.5, spinner.width / 8)
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            PathAngleArc {
                centerX: spinner.width / 2
                centerY: spinner.height / 2
                radiusX: spinner.width / 2 - 1.5
                radiusY: spinner.height / 2 - 1.5
                startAngle: 0
                sweepAngle: 270
            }
        }
    }
}
