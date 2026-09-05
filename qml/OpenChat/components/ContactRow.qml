import QtQuick
import QtQuick.Window
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
    required property bool isGroup
    readonly property bool compact: height < 55
    property bool statusBubbleEnabled: true
    property bool statusBubbleReady: false
    property bool statusBubbleDismissed: false
    readonly property bool avatarHovered: statusBubbleEnabled && visible && rowMouse.containsMouse
        && rowMouse.mouseX >= avatarImage.x && rowMouse.mouseX < avatarImage.x + avatarImage.width
        && rowMouse.mouseY >= avatarImage.y && rowMouse.mouseY < avatarImage.y + avatarImage.height
        && statusText.trim().length > 0
    onAvatarHoveredChanged: {
        statusBubbleReady = false;
        if (!avatarHovered)
            statusBubbleDismissed = false;
    }
    // The text block starts one avatar-margin right of the avatar; the bead
    // follows the name on its own line so the status line gets the full width.
    readonly property int textLeft: avatarImage.x + avatarImage.width + 14
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
        objectName: "contactAvatar"
        x: 13
        anchors.verticalCenter: parent.verticalCenter
        width: Math.min(44, row.height - 6)
        height: width
        avatarKey: row.avatarKey
    }

    Text {
        id: nameText
        x: row.textLeft
        y: row.compact ? 3 : 11
        width: Math.min(implicitWidth, row.width - x - presenceBead.width - 8 - 12)
        elide: Text.ElideRight
        text: row.name
        color: Theme.textPrimary
        font.family: Theme.uiFont
        font.pixelSize: 16
        renderType: Text.NativeRendering
    }

    // A group has no presence of its own, so no bead follows its name.
    PresenceBead {
        id: presenceBead
        x: nameText.x + nameText.width + 8
        anchors.verticalCenter: nameText.verticalCenter
        anchors.verticalCenterOffset: 1
        beadSize: 11
        presence: row.presence
        visible: !row.isGroup
    }

    Text {
        x: row.textLeft
        y: row.compact ? 26 : 34
        width: row.width - x - 12
        elide: Text.ElideRight
        text: row.statusText
        color: Theme.textSecondary
        font.family: Theme.uiFont
        font.pixelSize: 14
        renderType: Text.NativeRendering
    }

    MouseArea {
        id: rowMouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: {
            row.statusBubbleDismissed = true;
            row.activated(row.contactId);
        }
    }

    Timer {
        interval: 320
        running: row.avatarHovered && !row.statusBubbleDismissed
        onTriggered: row.statusBubbleReady = true
    }
    ContactStatusBubble {
        objectName: "contactStatusBubble_" + row.contactId
        target: avatarImage
        statusText: row.statusText
        shown: row.avatarHovered && row.statusBubbleReady && !row.statusBubbleDismissed
    }
}
