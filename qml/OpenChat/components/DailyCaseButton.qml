import QtQuick
import QtQuick.Controls
import OpenChat
import OpenChat.Native

AbstractButton {
    id: root
    objectName: "dailyCaseButton"
    required property var controller
    width: 30
    height: 28
    Accessible.name: "Daily case"
    ToolTip.visible: hovered
    ToolTip.text: !controller ? "Hourly case"
        : controller.state === DailyCaseController.Available ? "Your case is ready"
        : controller.state === DailyCaseController.Opened && !isNaN(controller.nextAvailableAt)
            ? "Next case at " + controller.nextAvailableAt.toLocaleTimeString(Qt.locale(), Locale.ShortFormat)
        : "Hourly case"
    background: Rectangle {
        radius: 4
        color: root.hovered ? Theme.buttonHover : "transparent"
        border.color: root.activeFocus ? Theme.focusBorder : "transparent"
    }
    contentItem: Item {
        Rectangle {
            anchors.centerIn: parent
            width: 17; height: 13; radius: 2
            color: "transparent"
            border.color: Theme.iconInk
            Rectangle { x: -1; y: 3; width: 19; height: 1; color: Theme.iconInk }
            Rectangle { x: 7; y: 3; width: 3; height: 5; radius: 1; color: Theme.iconInk }
            Rectangle { x: 5; y: -4; width: 7; height: 4; radius: 1; color: "transparent"; border.color: Theme.iconInk }
        }
        Rectangle {
            x: parent.width - 6; y: 1; width: 5; height: 5; radius: 3
            color: Theme.accentBlue
            visible: root.controller !== null && root.controller.state === DailyCaseController.Available
        }
    }
}
