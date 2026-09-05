import QtQuick
import OpenChat
import OpenChat.Native

// The add-contact dialog: a centered card floating over a click-catching scrim. It
// binds entirely to the ContactController handed in as a property and reads its
// frozen surface (dialogOpen / status / myInvite …); it mutates nothing except
// through the controller's invokables. When no controller is attached, or its
// dialog is closed, the whole overlay is invisible so the default and capture paths
// render unchanged.
Item {
    id: root
    objectName: "addContactDialog"
    property var contactController: null
    anchors.fill: parent
    visible: root.contactController && root.contactController.dialogOpen

    readonly property bool working:
        root.contactController
        && root.contactController.status === ContactController.Status.Working

    // Scrim: dims the surface and swallows clicks; a click outside the card dismisses
    // the dialog through the controller.
    Rectangle {
        anchors.fill: parent
        color: Theme.dialogScrim

        MouseArea {
            anchors.fill: parent
            onClicked: {
                if (root.contactController)
                    root.contactController.closeDialog();
            }
        }
    }

    // The card. Its own MouseArea swallows presses so a click inside never reaches
    // the scrim's dismiss handler.
    Rectangle {
        id: card
        anchors.centerIn: parent
        width: 420
        height: cardColumn.y + cardColumn.height + 24
        radius: 6
        color: Theme.contentBackground
        border.width: 1
        border.color: Theme.inputBorder

        MouseArea { anchors.fill: parent }

        // Aero inset along the top and bottom edges, matching the app's framed surfaces.
        Rectangle { x: 6; y: 1; width: parent.width - 12; height: 1; color: Theme.insetTop }
        Rectangle { x: 6; y: parent.height - 2; width: parent.width - 12; height: 1; color: Theme.gloss }

        // Close affordance: a small "×" drawn from two crossed strokes.
        Item {
            objectName: "addContactClose"
            anchors.right: parent.right
            anchors.rightMargin: 15
            anchors.top: parent.top
            anchors.topMargin: 15
            width: 20
            height: 20

            Rectangle {
                anchors.centerIn: parent
                width: 15
                height: 2
                radius: 1
                rotation: 45
                color: Theme.textSecondary
            }
            Rectangle {
                anchors.centerIn: parent
                width: 15
                height: 2
                radius: 1
                rotation: -45
                color: Theme.textSecondary
            }
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    if (root.contactController)
                        root.contactController.closeDialog();
                }
            }
        }

        Column {
            id: cardColumn
            x: 24
            y: 22
            width: parent.width - 48
            spacing: 18

            Text {
                text: "Add a contact"
                color: Theme.textPrimary
                font.family: Theme.uiFont
                font.pixelSize: 22
                renderType: Text.NativeRendering
            }

            // By handle.
            Column {
                width: parent.width
                spacing: 7

                Text {
                    text: "Add by handle"
                    color: Theme.textPrimary
                    font.family: Theme.uiFont
                    font.pixelSize: 14
                    renderType: Text.NativeRendering
                }

                Item {
                    width: parent.width
                    height: 38

                    AeroTextField {
                        id: handleField
                        objectName: "addHandleField"
                        anchors.left: parent.left
                        anchors.right: addHandleButton.left
                        anchors.rightMargin: 8
                        height: parent.height
                        prefix: "@"
                        placeholder: "handle"
                    }
                    AeroButton {
                        id: addHandleButton
                        objectName: "addHandleButton"
                        anchors.right: parent.right
                        width: 96
                        height: parent.height
                        label: root.working ? "Working…" : "Add"
                        enabled: !root.working
                        onClicked: {
                            if (root.contactController)
                                root.contactController.addByHandle(handleField.text);
                        }
                    }
                }
            }

            // By invite code.
            Column {
                width: parent.width
                spacing: 7

                Text {
                    text: "Redeem an invite"
                    color: Theme.textPrimary
                    font.family: Theme.uiFont
                    font.pixelSize: 14
                    renderType: Text.NativeRendering
                }

                Item {
                    width: parent.width
                    height: 38

                    AeroTextField {
                        id: inviteField
                        objectName: "redeemInviteField"
                        anchors.left: parent.left
                        anchors.right: redeemInviteButton.left
                        anchors.rightMargin: 8
                        height: parent.height
                        placeholder: "Paste an invite code"
                    }
                    AeroButton {
                        id: redeemInviteButton
                        objectName: "redeemInviteButton"
                        anchors.right: parent.right
                        width: 96
                        height: parent.height
                        label: root.working ? "Working…" : "Redeem"
                        enabled: !root.working
                        onClicked: {
                            if (root.contactController)
                                root.contactController.addByInvite(inviteField.text);
                        }
                    }
                }
            }

            // Share your own invite.
            Column {
                width: parent.width
                spacing: 8

                Text {
                    text: "Share your invite"
                    color: Theme.textPrimary
                    font.family: Theme.uiFont
                    font.pixelSize: 14
                    renderType: Text.NativeRendering
                }

                AeroButton {
                    objectName: "createInviteButton"
                    width: parent.width
                    height: 40
                    label: root.working ? "Working…" : "Create my invite"
                    enabled: !root.working
                    onClicked: {
                        if (root.contactController)
                            root.contactController.createMyInvite();
                    }
                }

                // Invite box, revealed once an invite is ready to share.
                Column {
                    width: parent.width
                    spacing: 8
                    visible: root.contactController && root.contactController.inviteReady

                    Rectangle {
                        width: parent.width
                        height: myInviteText.height + 24
                        radius: 6
                        color: Theme.panelBackground
                        border.width: 1
                        border.color: Theme.inputBorder

                        TextEdit {
                            id: myInviteText
                            objectName: "myInviteText"
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.leftMargin: 12
                            anchors.rightMargin: 12
                            anchors.verticalCenter: parent.verticalCenter
                            readOnly: true
                            selectByMouse: true
                            selectionColor: Theme.selectionBackground
                            selectedTextColor: Theme.selectionText
                            text: root.contactController ? root.contactController.myInvite : ""
                            color: Theme.textPrimary
                            font.family: "Courier New"
                            font.pixelSize: 15
                            wrapMode: TextEdit.WrapAnywhere
                            renderType: Text.NativeRendering
                        }
                    }

                    Item {
                        width: parent.width
                        height: 34

                        AeroButton {
                            id: copyInviteButton
                            objectName: "copyInviteButton"
                            anchors.left: parent.left
                            width: 96
                            height: parent.height
                            label: "Copy"
                            onClicked: {
                                myInviteText.selectAll();
                                myInviteText.copy();
                                myInviteText.deselect();
                            }
                        }

                        Text {
                            anchors.left: copyInviteButton.right
                            anchors.leftMargin: 12
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            text: "Share this code as text — QR scanning isn't in this build."
                            color: Theme.textSecondary
                            wrapMode: Text.WordWrap
                            font.family: Theme.uiFont
                            font.pixelSize: 13
                            renderType: Text.NativeRendering
                        }
                    }
                }
            }

            // Feedback: the controller's latest status message, colored by outcome.
            Text {
                objectName: "addContactStatus"
                width: parent.width
                visible: root.contactController && root.contactController.statusMessage.length > 0
                text: root.contactController ? root.contactController.statusMessage : ""
                color: {
                    if (root.contactController
                            && root.contactController.status === ContactController.Status.Error)
                        return Theme.errorText;
                    if (root.contactController
                            && root.contactController.status === ContactController.Status.Success)
                        return Theme.successText;
                    return Theme.textSecondary;
                }
                wrapMode: Text.WordWrap
                font.family: Theme.uiFont
                font.pixelSize: 13
                renderType: Text.NativeRendering
            }
        }
    }
}
