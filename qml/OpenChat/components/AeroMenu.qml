import QtQuick
import QtQuick.Controls.Basic
import OpenChat

Menu {
    id: menu
    width: 260
    padding: 5
    margins: 8
    overlap: 1
    cascade: true
    delegate: AeroMenuItem {}

    background: Rectangle {
        radius: 5
        color: Theme.contentBackground
        border.color: Theme.inputBorder
        Rectangle {
            x: 1; y: 1
            width: parent.width - 2
            height: 16
            radius: 4
            gradient: Gradient {
                GradientStop { position: 0; color: Theme.glossStrong }
                GradientStop { position: 1; color: "transparent" }
            }
        }
    }

    contentItem: ListView {
        implicitHeight: contentHeight
        model: menu.contentModel
        currentIndex: menu.currentIndex
        clip: true
        interactive: contentHeight > height
        boundsBehavior: Flickable.StopAtBounds
        ScrollIndicator.vertical: ScrollIndicator {}
    }
}
