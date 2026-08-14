import QtQuick
import OpenChat

Item {
    id: row
    required property string contactId
    required property string name
    required property string statusText
    required property int presence
    required property bool favorite
    required property bool selected
    required property string avatarKey
    readonly property bool compact: height < 55
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
        width: Math.min(44, row.height - 6)
        height: width
        avatarKey: row.avatarKey
    }

    PresenceBead {
        x: avatarImage.x + avatarImage.width + 10
        anchors.verticalCenter: parent.verticalCenter
        beadSize: 12
        presence: row.presence
    }

    Text {
        x: 84
        y: row.compact ? 3 : 11
        text: row.name
        color: Theme.textPrimary
        font.family: Theme.uiFont
        font.pixelSize: 16
        renderType: Text.NativeRendering
    }

    Text {
        x: 84
        y: row.compact ? 26 : 34
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
