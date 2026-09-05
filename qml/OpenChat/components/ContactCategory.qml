pragma ComponentBehavior: Bound

import QtQuick
import OpenChat

Item {
    id: category
    required property string label
    required property bool favoriteCategory
    required property var contactModel
    required property var controller
    property real rowHeight: 60
    property int visibleCount: favoriteCategory ? contactModel.favoriteCount : contactModel.regularCount

    implicitHeight: visibleCount > 0 ? 40 + visibleCount * rowHeight : 0
    visible: visibleCount > 0

    Rectangle {
        id: header
        width: parent.width
        height: 40
        color: Theme.sectionHighlight
        border.width: 0

        Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.rule }
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule }

        Text {
            x: 15
            anchors.verticalCenter: parent.verticalCenter
            anchors.verticalCenterOffset: 1
            text: category.label
            color: Theme.categoryText
            font.family: Theme.uiFont
            font.pixelSize: 17
            renderType: Text.NativeRendering
        }

        Text {
            anchors.right: parent.right
            anchors.rightMargin: 20
            anchors.verticalCenter: parent.verticalCenter
            text: "⌃"
            color: Theme.categoryChevron
            font.family: Theme.uiFont
            font.bold: true
            font.pixelSize: 15
        }
    }

    Column {
        anchors.top: header.bottom
        width: parent.width

        Repeater {
            model: category.contactModel

            ContactRow {
                id: contactRowDelegate
                objectName: "contactRow_" + contactRowDelegate.contactId
                statusBubbleEnabled: !category.favoriteCategory
                width: category.width
                height: contactRowDelegate.favorite === category.favoriteCategory ? category.rowHeight : 0
                visible: height > 0
                onActivated: contactId => category.controller.selectContact(contactId)
            }
        }
    }
}
