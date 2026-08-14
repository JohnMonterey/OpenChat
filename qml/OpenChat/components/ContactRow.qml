import QtQuick
import OpenChat

Item {
    id: row
    property string contactId
    property string contactName
    property string statusText
    property int presence
    property bool selected
    property string avatarKey
    signal activated(string contactId)

    implicitHeight: 60

    Rectangle {
        anchors.fill: parent
        visible: row.selected
        border.width: 0
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0; color: Theme.selectedTop }
            GradientStop { position: 0.82; color: Theme.selectedBottom }
            GradientStop { position: 1; color: "#00ffffff" }
        }
    }

    Avatar {
        id: avatarImage
        x: 13
        anchors.verticalCenter: parent.verticalCenter
        width: 44
        height: 44
        avatarKey: row.avatarKey
    }

    PresenceBead {
        x: avatarImage.x + avatarImage.width + 10
        y: avatarImage.y + 20
        beadSize: 12
        presence: row.presence
    }

    Text {
        x: 84
        y: 11
        text: row.contactName
        color: Theme.textPrimary
        font.family: Theme.uiFont
        font.pixelSize: 16
        renderType: Text.NativeRendering
    }

    Text {
        x: 84
        y: 34
        text: row.statusText
        color: Theme.textSecondary
        font.family: Theme.uiFont
        font.pixelSize: 14
        renderType: Text.NativeRendering
    }

    MouseArea {
        anchors.fill: parent
        cursorShape: Qt.PointingHandCursor
        onClicked: row.activated(row.contactId)
    }
}
