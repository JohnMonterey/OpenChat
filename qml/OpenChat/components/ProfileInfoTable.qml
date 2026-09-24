import QtQuick
import OpenChat
import OpenChat.Native

// The Interests / Details table (SPEC §5.5): bold labels on the left, the
// owner's words on the right. Cells (the MySpace look): tinted label and value
// cells 3 px apart, radius 2, 7/5 px padding. Lines: rows padded 6 px with a
// faint rule between them. The inks and cell fills come from the renderer,
// already held at 4.5:1 against their cells.
//
// The labels share one size and one line. At the narrowest columns (the
// label column's 80 px floor) the whole table's labels step down together,
// by as much as the widest of them needs and never below 11 px; a label
// still too wide after that elides.
Column {
    id: table
    property var render: null
    // [{label, value}], filled rows only.
    property var rows: []
    readonly property bool cells: render !== null && render.tableStyle === Profile.CellTable
    readonly property int labelWidth: Math.max(80, Math.min(104, Math.round(width * 0.30)))
    // The label's room: 7 px of cell padding each side, or (Lines) the 6 px
    // before the value.
    readonly property int labelRoom: cells ? labelWidth - 14 : labelWidth - 6
    readonly property int basePixelSize: render ? render.labelPixelSize : 13
    // The widest label at the base size (reading the metrics' font makes
    // this follow the face, size and weight).
    readonly property real widestLabel: {
        let widest = 0;
        const font = labelMetrics.font;
        for (let i = 0; i < rows.length; ++i)
            widest = Math.max(widest, labelMetrics.advanceWidth(rows[i].label));
        return font ? Math.ceil(widest) + 1 : 0;
    }
    readonly property int labelPixelSize: widestLabel <= labelRoom
                                          ? basePixelSize
                                          : Math.max(Math.min(11, basePixelSize),
                                                     Math.floor(basePixelSize * labelRoom / widestLabel))

    spacing: cells ? 3 : 0

    FontMetrics {
        id: labelMetrics
        font.family: table.render && table.render.labelFamily.length > 0 ? table.render.labelFamily : Theme.uiFont
        font.pixelSize: table.basePixelSize
        font.bold: table.render ? table.render.labelBold : true
    }

    Repeater {
        model: table.rows
        Item {
            id: row
            required property var modelData
            required property int index
            width: table.width
            height: Math.max(label.implicitHeight, value.implicitHeight) + (table.cells ? 10 : 12)

            Rectangle {
                visible: table.cells
                width: table.labelWidth
                height: parent.height
                radius: 2
                color: table.render ? table.render.cellLabelFill : "transparent"
            }
            Rectangle {
                visible: table.cells
                x: table.labelWidth + 3
                width: parent.width - x
                height: parent.height
                radius: 2
                color: table.render ? table.render.cellValueFill : "transparent"
            }
            Rectangle {
                visible: !table.cells && row.index > 0
                width: parent.width
                height: 1
                color: table.render ? table.render.tableRuleColor : "transparent"
            }
            Text {
                id: label
                objectName: "profileTableLabel"
                x: table.cells ? 7 : 0
                y: table.cells ? 5 : 6
                width: table.labelRoom
                elide: Text.ElideRight
                text: row.modelData.label
                textFormat: Text.PlainText
                color: table.render ? (table.cells ? table.render.cellLabelInk : table.render.labelColor) : "black"
                font.family: labelMetrics.font.family
                font.pixelSize: table.labelPixelSize
                font.bold: labelMetrics.font.bold
                renderType: Text.NativeRendering
            }
            Text {
                id: value
                objectName: "profileTableValue"
                x: table.labelWidth + (table.cells ? 10 : 6)
                y: table.cells ? 5 : 6
                width: parent.width - x - (table.cells ? 7 : 0)
                wrapMode: Text.Wrap
                lineHeight: 1.1
                text: row.modelData.value
                textFormat: Text.PlainText
                color: table.render ? (table.cells ? table.render.cellValueInk : table.render.bodyColor) : "black"
                style: table.render && table.render.textHalo && !table.cells ? Text.Outline : Text.Normal
                styleColor: table.render ? table.render.haloColor : "transparent"
                font.family: table.render && table.render.bodyFamily.length > 0 ? table.render.bodyFamily : Theme.uiFont
                font.pixelSize: table.render ? table.render.bodyPixelSize : 13
                renderType: Text.NativeRendering
            }
        }
    }
}
