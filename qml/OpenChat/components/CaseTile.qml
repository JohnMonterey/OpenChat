import QtQuick
import OpenChat

Rectangle {
    id: root
    property bool selected: false
    property bool settled: false
    property bool animateReveal: true
    radius: 6
    color: selected && settled ? Theme.selectedTop : Theme.buttonBackground
    border.color: selected && settled ? Theme.focusBorder : Theme.inputBorder
    border.width: 1
    opacity: settled && !selected ? 0.38 : 1
    scale: settled && selected ? 1.035 : 1
    y: settled && selected ? -3 : 0
    Behavior on scale { enabled: root.animateReveal; NumberAnimation { duration: 380; easing.type: Easing.OutBack } }
    Behavior on y { enabled: root.animateReveal; NumberAnimation { duration: 380; easing.type: Easing.OutCubic } }
    Behavior on opacity { enabled: root.animateReveal; NumberAnimation { duration: 300 } }
    Rectangle { x: 7; y: 1; width: parent.width - 14; height: 1; color: Theme.glossStrong }
    Rectangle {
        anchors.centerIn: parent
        width: 35; height: 35; radius: 7; rotation: 45
        color: root.selected && root.settled ? Theme.selectedBottom : Theme.fieldBackground
        border.color: Theme.rule
        Text {
            anchors.centerIn: parent
            rotation: -45
            text: "?"
            font.family: Theme.uiFont
            font.pixelSize: 23
            color: root.selected && root.settled ? Theme.focusBorder : Theme.textSecondary
        }
    }
    Rectangle {
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 6
        anchors.horizontalCenter: parent.horizontalCenter
        width: 20; height: 2; radius: 1
        color: root.selected && root.settled ? Theme.focusBorder : Theme.rule
    }
}
