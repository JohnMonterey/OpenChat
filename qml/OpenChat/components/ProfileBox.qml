import QtQuick
import OpenChat
import OpenChat.Native

// One module box (SPEC §4), built from scene-graph primitives only: the ring
// shadow (a 1 px ring and a line under the bottom edge, never a fill, so a
// see-through box never darkens and the contrast maths stays exact) or the
// neon edge, the fill at the resolved opacity, the header strip or the
// strip-less "none" title, the body, the Aero bevel lines and the theme
// border on top. The page's theme supplies colours and enums through
// `render` (a ProfileRenderStyle); everything else is the renderer's.
Item {
    id: box
    property var render: null
    // The wide column's family: the alt strip, border and monogram inks.
    property bool alt: false
    property string title: ""
    property string suffix: ""
    property string titleObjectName: "profileBoxTitle"
    property int pad: 12
    default property alias content: body.data

    readonly property bool showHeader: title.length > 0
    readonly property int bw: render ? render.borderWidth : 0
    readonly property int radius: render ? render.radiusPx : 0
    readonly property bool darkBox: render ? render.boxDark : false
    readonly property bool hasStrip: showHeader && render !== null && render.headerStyle !== Profile.NoHeader
    readonly property color borderColor: render ? (alt ? render.altBorderColor : render.borderColor) : "transparent"
    // Style "none": the title in the heading face at (title + 2) px.
    readonly property int plainTitleSize: render ? Math.round(render.titlePixelSize + 2 * render.headingFactor) : 16
    readonly property int plainTitleHeight: Math.round(plainTitleSize * 1.35)
    readonly property int headerBlock: !showHeader ? 0 : hasStrip ? render.stripHeight : plainTitleHeight + 14
    readonly property real innerWidth: Math.max(0, width - 2 * (bw + pad))
    // The strip (or the strip-less title), for the editor's click targets.
    readonly property Item headerItem: hasStrip ? stripItem : plainTitle

    implicitHeight: headerBlock + body.height + 2 * pad + 2 * bw
    Accessible.role: Accessible.Grouping
    Accessible.name: box.title

    // Ring shadow (layer 1).
    Rectangle {
        visible: box.render !== null && !box.render.boxGlow
        anchors.fill: parent
        anchors.margins: -1
        radius: box.radius + 1
        color: "transparent"
        border.width: 1
        border.color: box.darkBox ? "#66000000" : "#1c1b3a58"
    }
    Rectangle {
        visible: box.render !== null && !box.render.boxGlow
        x: Math.max(2, box.radius)
        y: parent.height + 1
        width: parent.width - 2 * x
        height: 1
        color: box.darkBox ? "#44000000" : "#141b3a58"
    }
    // Neon edge (layer 1′): four rings in the border colour, fading outwards.
    Repeater {
        model: box.render !== null && box.render.boxGlow ? 4 : 0
        Rectangle {
            required property int index
            anchors.fill: parent
            anchors.margins: -(index + 1)
            radius: box.radius + index + 1
            color: "transparent"
            border.width: 1
            border.color: Qt.rgba(box.borderColor.r, box.borderColor.g, box.borderColor.b,
                                  [0.34, 0.22, 0.13, 0.06][index])
        }
    }

    // Fill (layer 2): the alpha is the resolved opacity.
    Rectangle {
        anchors.fill: parent
        radius: box.radius
        color: box.render ? box.render.boxFill : "transparent"
    }

    // Header strip (layer 3), inside the border and rounded at the top only.
    ProfileStrip {
        id: stripItem
        visible: box.hasStrip
        x: box.bw
        y: box.bw
        width: parent.width - 2 * box.bw
        height: box.render ? box.render.stripHeight : 30
        radius: Math.max(0, box.radius - box.bw)
        highlightInset: Math.max(2, box.radius - 1)
        stops: box.render ? (box.alt ? box.render.altStripStops : box.render.stripStops) : []
        positions: box.render ? (box.alt ? box.render.altStripPositions : box.render.stripPositions) : []
        highlight: box.render ? (box.alt ? box.render.altStripHighlight : box.render.stripHighlight) : "transparent"
        bottomLine: box.render ? (box.alt ? box.render.altStripBottomLine : box.render.stripBottomLine) : "transparent"
        dark: box.render ? (box.alt ? box.render.altStripDark : box.render.stripDark) : false
        title: box.title
        suffix: box.suffix
        titleObjectName: box.hasStrip ? box.titleObjectName : ""
        titleColor: box.render ? (box.alt ? box.render.altHeaderText : box.render.headerText) : "black"
        titleFamily: box.render ? box.render.headingFamily : ""
        titlePixelSize: box.render ? box.render.titlePixelSize : 14
        titleBold: box.render ? box.render.headingBold : false
        titleLift: box.render ? box.render.headingLift : 0
        halo: box.render ? box.render.textHalo : false
        haloColor: box.render ? box.render.haloColor : "transparent"
        pad: box.pad
    }

    // Style "none": the title on the box over a rule in the border colour.
    Item {
        id: plainTitle
        visible: box.showHeader && !box.hasStrip
        x: box.bw + box.pad
        y: box.bw + 9
        width: parent.width - 2 * (box.bw + box.pad)
        height: box.plainTitleHeight
        Row {
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width
            spacing: 6
            Text {
                objectName: plainTitle.visible ? box.titleObjectName : ""
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(implicitWidth, parent.width - (plainSuffix.visible ? plainSuffix.implicitWidth + 6 : 0))
                elide: Text.ElideRight
                text: box.title
                textFormat: Text.PlainText
                color: box.render ? (box.alt ? box.render.altHeaderText : box.render.headerText) : "black"
                style: box.render && box.render.textHalo ? Text.Outline : Text.Normal
                styleColor: box.render ? box.render.haloColor : "transparent"
                font.family: box.render && box.render.headingFamily.length > 0 ? box.render.headingFamily : Theme.uiFont
                font.pixelSize: box.plainTitleSize
                font.bold: box.render ? box.render.headingBold : false
                renderType: Text.NativeRendering
            }
            Text {
                id: plainSuffix
                visible: box.suffix.length > 0
                anchors.verticalCenter: parent.verticalCenter
                text: box.suffix
                textFormat: Text.PlainText
                color: box.render ? box.render.mutedColor : "gray"
                font.family: Theme.uiFont
                font.pixelSize: 12
                renderType: Text.NativeRendering
            }
        }
        Rectangle {
            y: parent.height + 3
            width: parent.width
            height: 1
            color: Qt.rgba(box.borderColor.r, box.borderColor.g, box.borderColor.b, 0.9)
        }
    }

    // Body (layer 4).
    Item {
        id: body
        x: box.bw + box.pad
        y: box.bw + box.headerBlock + box.pad
        width: box.innerWidth
        height: childrenRect.height
    }

    // Aero bevel (layer 5): a light line above the bottom edge, and a top
    // highlight on boxes without a strip.
    Rectangle {
        x: Math.max(3, box.radius)
        y: parent.height - box.bw - 1
        width: parent.width - 2 * x
        height: 1
        color: box.darkBox ? "#12ffffff" : "#a0ffffff"
    }
    Rectangle {
        visible: !box.hasStrip
        x: Math.max(3, box.radius)
        y: box.bw
        width: parent.width - 2 * x
        height: 1
        color: box.darkBox ? "#16ffffff" : "#c0ffffff"
    }

    // Border (layer 6), last, over the strip.
    ProfileBoxBorder {
        anchors.fill: parent
        borderWidth: box.bw
        borderStyle: box.render ? box.render.borderStyle : Profile.SolidBorder
        color: box.borderColor
        radius: box.radius
    }
}
