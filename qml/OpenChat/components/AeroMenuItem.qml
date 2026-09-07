import QtQuick
import QtQuick.Controls.Basic
import OpenChat

MenuItem {
    id: item
    implicitWidth: 250
    implicitHeight: 32
    leftPadding: 28
    rightPadding: 24
    hoverEnabled: true

    contentItem: Text {
        text: item.text
        color: item.enabled ? Theme.textPrimary : Theme.textSecondary
        font.family: Theme.uiFont
        font.pixelSize: 13
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    indicator: Text {
        x: 8
        y: (item.height - height) / 2
        text: "✓"
        visible: item.checkable && item.checked
        color: Theme.textPrimary
        font.pixelSize: 14
    }
    arrow: Text {
        x: item.width - width - 10
        y: (item.height - height) / 2
        text: "›"
        visible: item.subMenu !== null
        color: Theme.textPrimary
        font.pixelSize: 20
    }
    background: Rectangle {
        radius: 3
        color: item.highlighted ? Theme.navSelected : "transparent"
        border.width: item.highlighted ? 1 : 0
        border.color: Theme.focusBorder
    }
}
