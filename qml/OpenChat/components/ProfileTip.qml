import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Shapes
import OpenChat
import OpenChat.Native

// The Aero tooltip every profile tip uses (ContactStatusBubble's chrome): the
// three-stop tooltip gradient, a 1 px tooltipBorder, a highlight line under
// the top edge and a shadow 2 px below, with an optional bold title and an
// optional nose pointing at what it explains. Plain text only.
//
// Place it like any ToolTip: parent it to the thing it explains and show it
// on hover; x/y (in the parent's coordinates) move it, noseX points it.
ToolTip {
    id: tip
    objectName: "profileTip"
    property string title: ""
    // Where the nose points, in the tip's own x; negative: no nose.
    property real noseX: -1
    property int maxWidth: 300
    readonly property int noseHeight: noseX >= 0 ? 7 : 0

    delay: 400
    timeout: -1
    margins: 6
    leftPadding: 12
    rightPadding: 12
    topPadding: 8 + noseHeight
    bottomPadding: 10
    // Wide enough for the longest line, never wider than maxWidth.
    width: Math.min(maxWidth, Math.ceil(Math.max(titleMetrics.advanceWidth, bodyMetrics.advanceWidth)) + leftPadding
                    + rightPadding + 2)

    TextMetrics { id: titleMetrics; font: titleText.font; text: tip.title }
    TextMetrics { id: bodyMetrics; font: bodyText.font; text: tip.text }

    enter: Transition {
        NumberAnimation {
            property: "opacity"; from: 0; to: 1
            duration: ProfileRenderPolicy.animationsAllowed ? 120 : 0
        }
    }
    exit: Transition {
        NumberAnimation {
            property: "opacity"; from: 1; to: 0
            duration: ProfileRenderPolicy.animationsAllowed ? 120 : 0
        }
    }

    contentItem: Column {
        spacing: 3
        Text {
            id: titleText
            visible: tip.title.length > 0
            width: tip.availableWidth
            wrapMode: Text.Wrap
            text: tip.title
            textFormat: Text.PlainText
            color: Theme.textPrimary
            font.family: Theme.uiFont
            font.pixelSize: 13
            font.bold: true
            renderType: Text.NativeRendering
        }
        Text {
            id: bodyText
            visible: tip.text.length > 0
            width: tip.availableWidth
            wrapMode: Text.Wrap
            text: tip.text
            textFormat: Text.PlainText
            color: Theme.textPrimary
            font.family: Theme.uiFont
            font.pixelSize: 12
            lineHeight: 1.1
            renderType: Text.NativeRendering
        }
    }

    background: Item {
        id: chrome
        // The outline, nose included, offset by `dy` (the shadow is the same
        // outline two pixels lower).
        function outline(dy) {
            var w = width, h = height - 2, t = tip.noseHeight + 0.5, n = tip.noseX;
            function p(x, y) { return x.toFixed(1) + " " + (y + dy).toFixed(1); }
            var d = "M " + p(5.5, t);
            if (n >= 0)
                d += " L " + p(Math.max(6, n - 7), t) + " L " + p(n, 0.5) + " L " + p(Math.min(w - 7, n + 7), t);
            d += " L " + p(w - 6, t) + " Q " + p(w - 0.5, t) + " " + p(w - 0.5, t + 5)
               + " L " + p(w - 0.5, h - 5) + " Q " + p(w - 0.5, h - 0.5) + " " + p(w - 6, h - 0.5)
               + " L " + p(5.5, h - 0.5) + " Q " + p(0.5, h - 0.5) + " " + p(0.5, h - 5)
               + " L " + p(0.5, t + 5) + " Q " + p(0.5, t) + " " + p(5.5, t) + " Z";
            return d;
        }

        Shape {
            anchors.fill: parent
            ShapePath {
                strokeColor: "transparent"
                fillColor: Theme.tooltipShadowFill
                PathSvg { path: chrome.outline(2) }
            }
            ShapePath {
                strokeColor: Theme.tooltipBorder
                strokeWidth: 1
                fillGradient: LinearGradient {
                    x1: 0; y1: tip.noseHeight
                    x2: 0; y2: chrome.height
                    GradientStop { position: 0; color: Theme.tooltipTop }
                    GradientStop { position: 0.48; color: Theme.tooltipMid }
                    GradientStop { position: 1; color: Theme.tooltipBottom }
                }
                PathSvg { path: chrome.outline(0) }
            }
        }
        Rectangle {
            x: 6
            y: tip.noseHeight + 1.5
            width: parent.width - 13
            height: 1
            color: Theme.tooltipHighlight
        }
    }
}
