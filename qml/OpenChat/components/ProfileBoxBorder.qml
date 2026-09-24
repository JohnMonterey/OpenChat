import QtQuick
import QtQuick.Shapes
import OpenChat.Native

// A box's theme border (SPEC §4.1 layer 6), drawn last so it lies over the
// strip. Solid is a plain rectangle stroke. Dashed (dash 4 px, gap 2.5 px,
// butt caps) and dotted (round caps, dash 0.01 px, gap 2.2 px) stroke a
// rounded-rectangle path whose line runs through the middle of the border.
// Double is two 1 px lines, one at the edge and one bw − 1 inside; below
// 3 px it has no room and draws solid.
Item {
    id: frame
    property int borderWidth: 1
    property int borderStyle: Profile.SolidBorder
    property color color: "#8dbbe0"
    property real radius: 0
    readonly property bool dashed: borderStyle === Profile.DashedBorder
    readonly property bool dotted: borderStyle === Profile.DottedBorder
    readonly property bool doubled: borderStyle === Profile.DoubleBorder && borderWidth >= 3
    readonly property bool solid: !dashed && !dotted && !doubled

    visible: borderWidth > 0
    Accessible.ignored: true

    // The stroke's centre line: the box outline inset by half the border.
    function outline() {
        const o = frame.borderWidth / 2;
        const w = frame.width - frame.borderWidth, h = frame.height - frame.borderWidth;
        const r = Math.max(0, Math.min(frame.radius, w / 2, h / 2));
        function p(x, y) { return (o + x).toFixed(2) + " " + (o + y).toFixed(2); }
        if (r <= 0)
            return "M " + p(0, 0) + " L " + p(w, 0) + " L " + p(w, h) + " L " + p(0, h) + " Z";
        const a = " A " + r + " " + r + " 0 0 1 ";
        return "M " + p(r, 0) + " L " + p(w - r, 0) + a + p(w, r) + " L " + p(w, h - r) + a + p(w - r, h)
             + " L " + p(r, h) + a + p(0, h - r) + " L " + p(0, r) + a + p(r, 0) + " Z";
    }

    Rectangle {
        visible: frame.solid
        anchors.fill: parent
        radius: frame.radius
        color: "transparent"
        border.width: frame.borderWidth
        border.color: frame.color
    }

    Item {
        visible: frame.doubled
        anchors.fill: parent
        Rectangle {
            anchors.fill: parent
            radius: frame.radius
            color: "transparent"
            border.width: 1
            border.color: frame.color
        }
        Rectangle {
            anchors.fill: parent
            anchors.margins: frame.borderWidth - 1
            radius: Math.max(0, frame.radius - frame.borderWidth + 1)
            color: "transparent"
            border.width: 1
            border.color: frame.color
        }
    }

    Shape {
        visible: frame.dashed || frame.dotted
        anchors.fill: parent
        ShapePath {
            strokeColor: frame.color
            strokeWidth: frame.borderWidth
            fillColor: "transparent"
            strokeStyle: ShapePath.DashLine
            // Qt measures dashes in line widths; the recipe is in pixels.
            dashPattern: frame.dotted ? [0.01 / frame.borderWidth, 2.2 / frame.borderWidth]
                                       : [4 / frame.borderWidth, 2.5 / frame.borderWidth]
            capStyle: frame.dotted ? ShapePath.RoundCap : ShapePath.FlatCap
            joinStyle: ShapePath.MiterJoin
            PathSvg { path: frame.outline() }
        }
    }
}
