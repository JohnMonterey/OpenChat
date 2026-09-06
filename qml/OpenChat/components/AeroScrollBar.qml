import QtQuick
import OpenChat

// A slim overlay scrollbar for the sidebar lists. It rides the left edge of its
// flickable and stays invisible until the view actually moves, so a short list
// looks exactly as it did before this existed. Dragging the handle scrolls, and
// the bar keeps itself shown while the pointer is on it or holding it.
Item {
    id: bar

    // The Flickable this bar drives. Everything below reads its contentY and
    // contentHeight; the bar anchors itself to the flickable's left edge.
    required property var flickable

    // The bar only exists while the content overflows its viewport.
    readonly property bool scrollable:
        flickable && flickable.contentHeight > flickable.height + 0.5

    // Handle geometry, clamped so a very long list still leaves something to
    // grab, with the travel range matched to the shortened handle.
    readonly property real minHandle: 32
    readonly property real handleHeight:
        scrollable ? Math.max(minHandle,
                              bar.height * (flickable.height / flickable.contentHeight))
                   : bar.height
    readonly property real travel: Math.max(0, bar.height - handleHeight)
    readonly property real maxContentY:
        scrollable ? flickable.contentHeight - flickable.height : 0

    // Shown while the view is moving, while the pointer is on the bar, and for
    // a beat afterwards so the fade does not cut off the moment a flick ends.
    property bool active: false
    readonly property bool shown:
        scrollable && (active || handleMouse.containsMouse || handleMouse.pressed
                       || flickable.moving || flickable.dragging || flickable.flicking)

    width: 10
    anchors.left: flickable ? flickable.left : undefined
    anchors.top: flickable ? flickable.top : undefined
    anchors.bottom: flickable ? flickable.bottom : undefined
    anchors.topMargin: 2
    anchors.bottomMargin: 2
    visible: opacity > 0.01
    opacity: shown ? 1 : 0

    Behavior on opacity {
        NumberAnimation { duration: 160; easing.type: Easing.OutQuad }
    }

    // Any content movement re-arms the hold timer, which is what keeps the bar
    // up briefly after a wheel scroll stops.
    Connections {
        target: bar.flickable ? bar.flickable : null
        function onContentYChanged() {
            bar.active = true;
            hideTimer.restart();
        }
    }

    Timer {
        id: hideTimer
        interval: 900
        onTriggered: bar.active = false
    }

    // Track: barely there, just enough to seat the handle against a light and a
    // dark sidebar alike.
    Rectangle {
        anchors.fill: parent
        anchors.leftMargin: 3
        anchors.rightMargin: 1
        radius: width / 2
        color: Theme.scrollTrack
    }

    // Handle: the request buttons' Aero treatment at scrollbar scale — a
    // three-stop vertical gradient under a gloss highlight, inside a darker rim.
    Rectangle {
        id: handle
        x: 3
        width: parent.width - 4
        height: bar.handleHeight
        radius: width / 2
        y: bar.travel <= 0 || bar.maxContentY <= 0
           ? 0
           : bar.travel * Math.max(0, Math.min(1, bar.flickable.contentY / bar.maxContentY))

        gradient: Gradient {
            GradientStop { position: 0; color: Theme.scrollHandleTop }
            GradientStop { position: 0.5; color: Theme.scrollHandleMid }
            GradientStop { position: 1; color: Theme.scrollHandleBottom }
        }
        border.width: 1
        border.color: Theme.scrollHandleBorder
        opacity: handleMouse.pressed ? 1 : (handleMouse.containsMouse ? 0.97 : 0.88)

        // Gloss down the upper half, the same highlight the buttons carry.
        Rectangle {
            x: 1
            y: 1
            width: parent.width - 2
            height: Math.max(2, parent.height / 2 - 1)
            radius: width / 2
            gradient: Gradient {
                GradientStop { position: 0; color: Theme.gloss }
                GradientStop { position: 1; color: "transparent" }
            }
        }
    }

    MouseArea {
        id: handleMouse
        anchors.fill: parent
        hoverEnabled: true

        // Only the handle itself drags. A press on bare track is passed through
        // rather than jumping the view, which keeps the rows underneath usable
        // while the bar is faded out.
        property real grabOffset: 0
        property bool dragging: false

        onPressed: function (mouse) {
            dragging = mouse.y >= handle.y && mouse.y <= handle.y + handle.height;
            grabOffset = mouse.y - handle.y;
            if (!dragging)
                mouse.accepted = false;
        }
        onReleased: dragging = false
        onCanceled: dragging = false
        onPositionChanged: function (mouse) {
            if (!dragging || bar.travel <= 0)
                return;
            const target = Math.max(0, Math.min(bar.travel, mouse.y - grabOffset));
            bar.flickable.contentY = (target / bar.travel) * bar.maxContentY;
        }
    }
}
