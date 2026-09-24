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
    required property int unreadCount
    required property bool isGroup
    // A call is running in this chat without us: a quiet green mark says so
    // from the sidebar, wherever the user happens to be looking.
    required property bool callInProgress
    readonly property real badgeSpace: (unreadBadge.visible ? unreadBadge.width + 12 : 0)
                                       + (callPill.visible ? callPill.width + 8 : 0)
    readonly property bool compact: height < 55
    property bool statusBubbleEnabled: true
    property bool statusBubbleReady: false
    property bool statusBubbleDismissed: false
    // The pointer is on the picture rather than the rest of the row: a click
    // there opens the person's profile (SPEC §12). The row's one MouseArea
    // decides, from the same hit test the status bubble uses.
    readonly property bool pointerOnAvatar: visible && rowMouse.containsMouse
        && rowMouse.mouseX >= avatarImage.x && rowMouse.mouseX < avatarImage.x + avatarImage.width
        && rowMouse.mouseY >= avatarImage.y && rowMouse.mouseY < avatarImage.y + avatarImage.height
    // The bubble carries the status line and, for a person, the way into
    // their profile, so a person without a status still gets one; a group
    // only has its status line to show.
    readonly property bool avatarHovered: statusBubbleEnabled && pointerOnAvatar
        && (!isGroup || statusText.trim().length > 0)
    // Arriving at or leaving the picture starts the bubble afresh: a click
    // dismisses it only while the pointer stays where it clicked.
    onAvatarHoveredChanged: {
        statusBubbleReady = false;
        statusBubbleDismissed = false;
    }
    // The text block starts one avatar-margin right of the avatar; the bead
    // follows the name on its own line so the status line gets the full width.
    readonly property int textLeft: avatarImage.x + avatarImage.width + 14
    signal activated(string contactId)
    signal profileRequested(string contactId)
    // A right click on a person's row: its category shows "View profile".
    signal contextMenuRequested(string contactId)

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

    // Rings and the profile badge over a person's picture while the pointer
    // is on it; the row's MouseArea handles the click.
    ProfileAvatarAffordance {
        objectName: "contactAvatarAffordance"
        target: avatarImage
        interactive: false
        visible: !row.isGroup
        hovered: row.pointerOnAvatar && !row.isGroup
        pressed: hovered && rowMouse.pressed
    }

    Text {
        id: nameText
        x: row.textLeft
        y: row.compact ? 3 : 11
        width: Math.min(implicitWidth, Math.max(0, row.width - x - presenceBead.width - 8 - 12 - row.badgeSpace))
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
        width: Math.max(0, row.width - x - 12 - row.badgeSpace)
        elide: Text.ElideRight
        text: row.statusText
        color: Theme.textSecondary
        font.family: Theme.uiFont
        font.pixelSize: 14
        renderType: Text.NativeRendering
    }

    Rectangle {
        id: callPill
        objectName: "contactCallPill"
        visible: row.callInProgress
        anchors.right: unreadBadge.visible ? unreadBadge.left : parent.right
        anchors.rightMargin: unreadBadge.visible ? 8 : 12
        anchors.verticalCenter: parent.verticalCenter
        width: callPillLabel.implicitWidth + 12
        height: 20
        radius: 10
        color: Theme.callBackdropTop
        border.width: 1
        border.color: Theme.acceptBorder
        Text {
            id: callPillLabel
            anchors.centerIn: parent
            text: "In call"
            color: Theme.textPrimary
            font.family: Theme.uiFont
            font.pixelSize: 11
            renderType: Text.NativeRendering
        }
    }

    Rectangle {
        id: unreadBadge
        objectName: "contactUnreadBadge"
        visible: row.unreadCount > 0
        anchors.right: parent.right
        anchors.rightMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        width: Math.max(22, unreadLabel.implicitWidth + 12)
        height: 22
        radius: 11
        color: Theme.unreadBadge
        Text {
            id: unreadLabel
            objectName: "contactUnreadLabel"
            anchors.centerIn: parent
            text: row.unreadCount > 99 ? "99+" : String(row.unreadCount)
            color: "white"
            font.family: Theme.uiFont
            font.pixelSize: 12
            font.bold: true
        }
    }

    MouseArea {
        id: rowMouse
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        cursorShape: Qt.PointingHandCursor
        onClicked: mouse => {
            row.statusBubbleDismissed = true;
            if (mouse.button === Qt.RightButton) {
                if (!row.isGroup)
                    row.contextMenuRequested(row.contactId);
                return;
            }
            // The picture opens the profile; the rest of the row still
            // selects the chat.
            if (!row.isGroup && row.pointerOnAvatar)
                row.profileRequested(row.contactId);
            else
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
        profileName: row.isGroup ? "" : row.name
        shown: row.avatarHovered && row.statusBubbleReady && !row.statusBubbleDismissed
    }
}
