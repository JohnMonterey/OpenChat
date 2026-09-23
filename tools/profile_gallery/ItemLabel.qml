import QtQuick
import OpenChat

// Name, id and one line of description for a gallery row.
Column {
    property string name
    property string itemId
    property string description
    width: 190
    spacing: 1
    Text {
        width: parent.width
        text: parent.name
        color: Theme.textPrimary
        font.family: Theme.uiFont
        font.pixelSize: 14
        font.bold: true
        elide: Text.ElideRight
    }
    Text {
        width: parent.width
        text: parent.itemId.length > 0 ? parent.itemId : "(nothing equipped)"
        color: Theme.categoryText
        font.family: "monospace"
        font.pixelSize: 11
    }
    Text {
        width: parent.width
        text: parent.description
        color: Theme.textSecondary
        font.family: Theme.uiFont
        font.pixelSize: 11
        wrapMode: Text.WordWrap
        maximumLineCount: 3
    }
}
