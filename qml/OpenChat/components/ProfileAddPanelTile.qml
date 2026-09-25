import QtQuick
import OpenChat
import OpenChat.Native
import QtQuick.Shapes

// The editor preview's last box in the wide column: a dashed "+ Add new
// panel" tile. Clicking it (through the preview's decor) opens the Panels
// tab with its template choices. Viewers never see it.
Item {
    id: tile
    objectName: "profileAddPanelTile"
    property var view: null
    readonly property var render: view ? view.render : null
    readonly property color ink: render ? render.linkColor : Theme.accentBlue
    readonly property int radius: render ? Math.max(6, render.radiusPx) : 6

    implicitHeight: 76
    Accessible.role: Accessible.Button
    Accessible.name: "Add new panel"

    Rectangle {
        anchors.fill: parent
        radius: tile.radius
        color: tile.render ? Qt.rgba(tile.render.boxFill.r, tile.render.boxFill.g, tile.render.boxFill.b, 0.55)
                           : "#80ffffff"
    }
    Shape {
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            strokeColor: Qt.rgba(tile.ink.r, tile.ink.g, tile.ink.b, 0.6)
            strokeWidth: 1.5
            strokeStyle: ShapePath.DashLine
            dashPattern: [4, 3]
            fillColor: "transparent"
            PathRectangle {
                x: 1
                y: 1
                width: tile.width - 2
                height: tile.height - 2
                radius: tile.radius
            }
        }
    }
    Row {
        anchors.centerIn: parent
        spacing: 10
        Rectangle {
            width: 30
            height: 30
            radius: 15
            color: Qt.rgba(tile.ink.r, tile.ink.g, tile.ink.b, 0.14)
            border.width: 1
            border.color: Qt.rgba(tile.ink.r, tile.ink.g, tile.ink.b, 0.5)
            anchors.verticalCenter: parent.verticalCenter
            ProfileGlyph {
                anchors.centerIn: parent
                width: 16
                height: 16
                kind: "plus"
                ink: tile.ink
            }
        }
        Column {
            anchors.verticalCenter: parent.verticalCenter
            Text {
                text: "Add new panel"
                color: tile.ink
                font.family: Theme.uiFont
                font.pixelSize: 15
                font.bold: true
                renderType: Text.NativeRendering
            }
            Text {
                text: "Text, pictures, videos, favorite games and more"
                color: tile.render ? tile.render.mutedColor : "gray"
                font.family: Theme.uiFont
                font.pixelSize: 12
                renderType: Text.NativeRendering
            }
        }
    }
}
