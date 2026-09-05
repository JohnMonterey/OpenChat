import QtQuick
import QtQuick.Shapes
import QtQuick.Window
import OpenChat

Item {
    id: bubble
    required property Item target
    required property string statusText
    property bool shown: false
    property real noseY: height / 2

    // Float above the sidebar and conversation without changing the row layout
    // or intercepting its clicks. Ownership remains with the contact delegate.
    parent: target.Window.contentItem
    z: 30
    width: Math.min(280, Math.max(86, naturalText.implicitWidth + 36),
                    parent ? parent.width - 24 : 280)
    height: caption.implicitHeight + 24
    visible: opacity > 0
    opacity: shown ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 120 } }

    function reposition() {
        if (!parent || !target)
            return;
        const anchor = target.mapToItem(parent, target.width + 7, target.height / 2);
        x = Math.min(anchor.x, parent.width - width - 12);
        y = Math.max(12, Math.min(anchor.y - height / 2, parent.height - height - 12));
        noseY = Math.max(14, Math.min(anchor.y - y, height - 14));
    }
    onShownChanged: if (shown) reposition()
    onWidthChanged: reposition()
    onHeightChanged: reposition()

    // Ancestor layout changes can move the avatar without changing its local
    // coordinates. Track it only for the brief time the bubble is open.
    Timer {
        interval: 50
        repeat: true
        running: bubble.shown
        onTriggered: bubble.reposition()
    }

    readonly property string outline: {
        const l = 8.5, r = width - 0.5, t = 0.5, b = height - 0.5, c = 7;
        const n = noseY;
        return "M " + (l + c) + " " + t
            + " H " + (r - c) + " Q " + r + " " + t + " " + r + " " + (t + c)
            + " V " + (b - c) + " Q " + r + " " + b + " " + (r - c) + " " + b
            + " H " + (l + c) + " Q " + l + " " + b + " " + l + " " + (b - c)
            + " V " + (n + 6) + " Q " + l + " " + (n + 5) + " " + (l - 2) + " " + (n + 4)
            + " L 1.5 " + (n + 1) + " Q 0 " + n + " 1.5 " + (n - 1)
            + " L " + (l - 2) + " " + (n - 4)
            + " Q " + l + " " + (n - 5) + " " + l + " " + (n - 6)
            + " V " + (t + c) + " Q " + l + " " + t + " " + (l + c) + " " + t + " Z";
    }

    Shape {
        x: 0; y: 2
        width: parent.width; height: parent.height
        ShapePath {
            strokeWidth: 3
            strokeColor: Theme.tooltipShadowStroke
            fillColor: Theme.tooltipShadowFill
            PathSvg { path: bubble.outline }
        }
    }
    Shape {
        objectName: "statusBubbleSurface"
        anchors.fill: parent
        ShapePath {
            strokeWidth: 1
            strokeColor: Theme.tooltipBorder
            fillGradient: LinearGradient {
                x1: 0; y1: 0; x2: 0; y2: bubble.height
                GradientStop { position: 0; color: Theme.tooltipTop }
                GradientStop { position: 0.48; color: Theme.tooltipMid }
                GradientStop { position: 1; color: Theme.tooltipBottom }
            }
            PathSvg { path: bubble.outline }
        }
    }
    Rectangle {
        x: 16; y: 1.5
        width: parent.width - 24; height: 1
        color: Theme.tooltipHighlight
    }
    Text {
        id: naturalText
        visible: false
        text: bubble.statusText
        textFormat: Text.PlainText
        font.family: Theme.uiFont
        font.pixelSize: 13
    }
    Text {
        id: caption
        objectName: "contactStatusBubbleText"
        x: 22; y: 12
        width: parent.width - 36
        text: bubble.statusText
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: Theme.textPrimary
        font.family: Theme.uiFont
        font.pixelSize: 13
        renderType: Text.NativeRendering
    }
}
