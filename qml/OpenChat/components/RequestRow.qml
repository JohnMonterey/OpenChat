pragma ComponentBehavior: Bound

import QtQuick
import OpenChat

// One inbound contact request: the sender's avatar and identity over a compact row
// of accept / decline / block actions. Bound to the ContactController handed in as
// `controller`; each button forwards the row's requestId to the matching invokable.
Item {
    id: row
    required property string requestId
    required property string accountId
    required property string displayName
    required property string subtitle
    required property string handle
    property var controller

    implicitHeight: 78

    Avatar {
        id: avatarImage
        x: 13
        y: 9
        width: 40
        height: 40
        avatarKey: "userpfp_none"
    }

    Text {
        anchors.left: avatarImage.right
        anchors.leftMargin: 10
        anchors.right: parent.right
        anchors.rightMargin: 13
        y: 10
        text: row.displayName
        color: Theme.textPrimary
        font.family: Theme.uiFont
        font.pixelSize: 16
        elide: Text.ElideRight
        renderType: Text.NativeRendering
    }

    Text {
        anchors.left: avatarImage.right
        anchors.leftMargin: 10
        anchors.right: parent.right
        anchors.rightMargin: 13
        y: 32
        text: row.subtitle
        color: Theme.textSecondary
        font.family: Theme.uiFont
        font.pixelSize: 13
        elide: Text.ElideRight
        renderType: Text.NativeRendering
    }

    Row {
        id: actions
        anchors.left: parent.left
        anchors.leftMargin: 13
        anchors.right: parent.right
        anchors.rightMargin: 13
        y: 46
        spacing: 6
        readonly property real buttonWidth: (width - spacing * 2) / 3

        AeroButton {
            objectName: "requestAccept_" + row.requestId
            width: actions.buttonWidth
            height: 26
            fontPixelSize: 13
            label: "Accept"
            onClicked: {
                if (row.controller)
                    row.controller.accept(row.requestId);
            }
        }
        AeroButton {
            objectName: "requestDecline_" + row.requestId
            width: actions.buttonWidth
            height: 26
            fontPixelSize: 13
            label: "Decline"
            onClicked: {
                if (row.controller)
                    row.controller.decline(row.requestId);
            }
        }
        AeroButton {
            objectName: "requestBlock_" + row.requestId
            width: actions.buttonWidth
            height: 26
            fontPixelSize: 13
            label: "Block"
            onClicked: {
                if (row.controller)
                    row.controller.block(row.requestId);
            }
        }
    }
}
