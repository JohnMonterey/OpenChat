import QtQuick
import OpenChat
import OpenChat.Native

// The Interests / Details table (SPEC §5.5): bold labels on the left, the
// owner's words on the right. Cells (the MySpace look): tinted label and value
// cells 3 px apart, radius 2, 7/5 px padding. Lines: rows padded 6 px with a
// faint rule between them. The inks and cell fills come from the renderer,
// already held at 4.5:1 against their cells.
Column {
    id: table
    property var render: null
    // [{label, value}], filled rows only.
    property var rows: []
    readonly property bool cells: render !== null && render.tableStyle === Profile.CellTable
    readonly property int labelWidth: Math.max(80, Math.min(104, Math.round(width * 0.30)))

    spacing: cells ? 3 : 0

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
                width: table.labelWidth - 12
                wrapMode: Text.Wrap
                text: row.modelData.label
                textFormat: Text.PlainText
                color: table.render ? (table.cells ? table.render.cellLabelInk : table.render.labelColor) : "black"
                font.family: table.render && table.render.labelFamily.length > 0 ? table.render.labelFamily : Theme.uiFont
                font.pixelSize: table.render ? table.render.labelPixelSize : 13
                font.bold: table.render ? table.render.labelBold : true
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
