import QtQuick
import OpenChat

// A box's header strip (SPEC §4.2): the renderer's recipe for the strip
// style, drawn from scene-graph primitives. A four-stop gradient (gloss has
// its hard glass edge at 0.49/0.50), a highlight line under the top edge, a
// darker last row, and the title engraved Windows 7 style (Text.Raised), with
// an optional suffix such as "(Top 8)".
//
// The top corners are rounded by clipping a taller rounded rectangle, so the
// bottom stays square where the strip meets the body. The Boxes tab reuses
// this for its style tiles, so every input is a plain value.
Item {
    id: strip
    // ProfileRenderStyle's stripStops / stripPositions / stripHighlight /
    // stripBottomLine / stripDark (or the alt family's).
    property var stops: ["#c4e2f7", "#b2d7f3", "#9fcdef", "#96c1e1"]
    property var positions: [0, 0.49, 0.5, 1]
    property color highlight: "transparent"
    property color bottomLine: "transparent"
    property bool dark: false
    // Top corner radius: the box's radius less its border, never negative.
    property real radius: 0
    // Where the highlight line starts and ends: max(2, box radius − 1).
    property real highlightInset: 2
    property string title: ""
    property string suffix: ""
    property color titleColor: "#133a61"
    property string titleFamily: ""
    property int titlePixelSize: 14
    property bool titleBold: false
    // Script faces sit a pixel high; the title moves by this (negative: up).
    property int titleLift: 0
    // The last-resort readability halo (ProfileRenderStyle.textHalo).
    property bool halo: false
    property color haloColor: "transparent"
    property int pad: 12
    property string titleObjectName: "profileBoxTitle"
    readonly property real extra: radius + 2

    implicitHeight: 30
    clip: true
    Accessible.ignored: true

    function stopAt(index) {
        const stops = strip.stops || [];
        return stops.length > index ? stops[index] : (stops.length > 0 ? stops[stops.length - 1] : "transparent");
    }
    // A stop's position on the strip, squeezed into the part of the taller
    // rectangle that shows.
    function positionAt(index) {
        const positions = strip.positions || [0, 0.49, 0.5, 1];
        const p = positions.length > index ? positions[index] : 1;
        return p * strip.height / (strip.height + strip.extra);
    }

    Rectangle {
        width: parent.width
        height: parent.height + strip.extra
        radius: strip.radius
        gradient: Gradient {
            GradientStop { position: strip.positionAt(0); color: strip.stopAt(0) }
            GradientStop { position: strip.positionAt(1); color: strip.stopAt(1) }
            GradientStop { position: strip.positionAt(2); color: strip.stopAt(2) }
            GradientStop { position: strip.positionAt(3); color: strip.stopAt(3) }
            GradientStop { position: 1; color: strip.stopAt(3) }
        }
    }
    Rectangle {
        visible: strip.highlight.a > 0
        x: strip.highlightInset
        y: 1
        width: parent.width - 2 * x
        height: 1
        color: strip.highlight
    }
    Rectangle {
        y: parent.height - 1
        width: parent.width
        height: 1
        color: strip.bottomLine
    }

    Row {
        x: strip.pad
        width: parent.width - 2 * strip.pad
        anchors.verticalCenter: parent.verticalCenter
        anchors.verticalCenterOffset: strip.titleLift
        spacing: 5
        Text {
            id: titleText
            objectName: strip.titleObjectName
            anchors.verticalCenter: parent.verticalCenter
            width: Math.min(implicitWidth, parent.width - (suffixText.visible ? suffixText.implicitWidth + 5 : 0))
            elide: Text.ElideRight
            text: strip.title
            textFormat: Text.PlainText
            color: strip.titleColor
            style: strip.halo ? Text.Outline : Text.Raised
            styleColor: strip.halo ? strip.haloColor : strip.dark ? "#50000000" : "#80ffffff"
            font.family: strip.titleFamily.length > 0 ? strip.titleFamily : Theme.uiFont
            font.pixelSize: strip.titlePixelSize
            font.bold: strip.titleBold
            renderType: Text.NativeRendering
        }
        Text {
            id: suffixText
            visible: strip.suffix.length > 0
            anchors.verticalCenter: parent.verticalCenter
            anchors.verticalCenterOffset: 1
            text: strip.suffix
            textFormat: Text.PlainText
            color: strip.titleColor
            opacity: 0.85
            font.family: Theme.uiFont
            font.pixelSize: 12
            renderType: Text.NativeRendering
        }
    }
}
