import QtQuick
import OpenChat

Item {
    id: category
    property string label
    property bool favoriteCategory
    property var contactModel
    property int visibleCount: favoriteCategory ? contactModel.favoriteCount : contactModel.regularCount

    implicitHeight: visibleCount > 0 ? 40 + visibleCount * 60 : 0
    visible: visibleCount > 0

    Rectangle {
        id: header
        width: parent.width
        height: 40
        color: "#27ffffff"
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
            color: "#557ca1"
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
                width: category.width
                height: model.favorite === category.favoriteCategory ? 60 : 0
                visible: height > 0
                contactId: model.contactId
                contactName: model.name
                statusText: model.statusText
                presence: model.presence
                selected: model.selected
                avatarKey: model.avatarKey
                onActivated: contactId => chatController.selectContact(contactId)
            }
        }
    }
}
