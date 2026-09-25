import QtQuick
import QtQuick.Shapes
import OpenChat
import OpenChat.Native

// The composer's "+", left of the field: a round Aero button (the call
// controls' three-stop gradient, rim and gloss) that opens the attach menu.
// While the menu is open the plus turns a quarter over into a cross and the
// rim takes the focus blue, and a click on it closes the menu again. It never
// takes the keyboard from the field; Ctrl+O there opens the same menu.
Item {
    id: button
    objectName: "attachButton"
    // The menu it opens is showing.
    property bool open: false
    // A click: `wasOpen` says whether the menu was showing when the press
    // began, so a click on the cross closes it even when the press itself
    // already took it down.
    signal activated(bool wasOpen)

    property bool openAtPress: false
    readonly property bool hovered: mouse.containsMouse && enabled
    readonly property bool down: mouse.pressed && mouse.containsMouse

    implicitWidth: 40
    implicitHeight: 40
    opacity: enabled ? 1 : 0.45
    Accessible.role: Accessible.Button
    Accessible.name: "Attach files"
    Accessible.description: "Send a photo, a video, a sound or any file"
    Accessible.onPressAction: if (button.enabled) button.activated(button.open)

    // The face: the call buttons' gradient, turned over while pressed.
    Rectangle {
        anchors.fill: parent
        radius: width / 2
        antialiasing: true
        border.width: 1
        border.color: button.open ? Theme.focusBorder : Theme.buttonBorder
        gradient: Gradient {
            GradientStop { position: 0; color: button.down ? Theme.buttonBottom : Theme.buttonTop }
            GradientStop { position: 0.5; color: Theme.buttonMid }
            GradientStop { position: 1; color: button.down ? Theme.buttonTop : Theme.buttonBottom }
        }
    }
    // The gloss over the top half, brighter under the pointer.
    Rectangle {
        x: 4
        y: 2
        width: parent.width - 8
        height: parent.height * 0.46
        radius: height / 2
        antialiasing: true
        opacity: button.down ? 0.16 : button.hovered || button.open ? 0.42 : 0.28
        gradient: Gradient {
            GradientStop { position: 0; color: Theme.glossStrong }
            GradientStop { position: 1; color: "#00ffffff" }
        }
    }

    // Two rounded strokes; an eighth of a turn makes them the cross.
    Shape {
        objectName: "attachButtonGlyph"
        preferredRendererType: Shape.CurveRenderer // smooth on the GPU too (see ProfileGlyph)
        anchors.centerIn: parent
        width: 14
        height: 14
        rotation: button.open ? 45 : 0
        antialiasing: true
        Behavior on rotation {
            NumberAnimation {
                duration: ProfileRenderPolicy.animationsAllowed ? 160 : 0
                easing.type: Easing.OutCubic
            }
        }
        ShapePath {
            fillColor: "transparent"
            strokeColor: button.hovered || button.open ? Theme.focusBorder : Theme.iconInk
            strokeWidth: 2
            capStyle: ShapePath.RoundCap
            PathSvg { path: "M 7 1 V 13 M 1 7 H 13" }
        }
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: button.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
        onPressed: button.openAtPress = button.open
        onClicked: button.activated(button.openAtPress)
    }
}
