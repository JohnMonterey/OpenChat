import QtQuick
import OpenChat

Column {
    property string title
    property string subtitle
    spacing: 2
    Text {
        text: parent.title + "  ·  " + (Theme.darkMode ? "dark" : "light")
        color: Theme.textPrimary
        font.family: Theme.uiFont
        font.pixelSize: 20
        font.bold: true
    }
    Text {
        text: parent.subtitle
        color: Theme.textSecondary
        font.family: Theme.uiFont
        font.pixelSize: 12
    }
}
