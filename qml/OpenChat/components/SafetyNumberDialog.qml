import QtQuick
import OpenChat
import OpenChat.Native

// The safety-number dialog: a centered card floating over a click-catching scrim,
// modelled on AddContactDialog. It binds entirely to the ContactController handed in
// as a property and reads its frozen surface (safetyNumberOpen / safetyNumber /
// safetyNumberVerified / safetyNumberContact); it mutates nothing except through the
// controller's invokables. When no controller is attached, or its safety-number
// surface is closed, the whole overlay is invisible so the default and capture paths
// render unchanged.
Item {
    id: root
    objectName: "safetyNumberDialog"
    property var contactController: null
    anchors.fill: parent
    visible: root.contactController && root.contactController.safetyNumberOpen

    readonly property string number:
        root.contactController ? root.contactController.safetyNumber : ""
    readonly property bool verified:
        root.contactController && root.contactController.safetyNumberVerified

    // Scrim: dims the surface and swallows clicks; a click outside the card dismisses
    // the dialog through the controller.
    Rectangle {
        anchors.fill: parent
        color: "#66223247"

        MouseArea {
            anchors.fill: parent
            onClicked: {
                if (root.contactController)
                    root.contactController.closeSafetyNumber();
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
        Rectangle { x: 6; y: 1; width: parent.width - 12; height: 1; color: "#24526878" }
        Rectangle { x: 6; y: parent.height - 2; width: parent.width - 12; height: 1; color: "#70ffffff" }

        // Close affordance: a small "×" drawn from two crossed strokes.
        Item {
            objectName: "safetyNumberClose"
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
                        root.contactController.closeSafetyNumber();
                }
            }
        }

        Column {
            id: cardColumn
            x: 24
            y: 22
            width: parent.width - 48
            spacing: 16

            Text {
                text: "Verify safety number"
                color: Theme.textPrimary
                font.family: Theme.uiFont
                font.pixelSize: 22
                renderType: Text.NativeRendering
            }

            Text {
                objectName: "safetyNumberContactLabel"
                width: parent.width
                text: root.contactController ? root.contactController.safetyNumberContact : ""
                color: Theme.textSecondary
                elide: Text.ElideRight
                font.family: Theme.uiFont
                font.pixelSize: 14
                renderType: Text.NativeRendering
            }

            // The number box: a read-only, selectable monospace grid the user reads
            // aloud to compare. Replaced by an "unavailable" line when the peer key
            // isn't known yet.
            Rectangle {
                width: parent.width
                height: numberContent.height + 24
                radius: 6
                color: "#eef6fb"
                border.width: 1
                border.color: Theme.inputBorder

                Item {
                    id: numberContent
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    height: root.number.length > 0 ? safetyNumberText.height
                                                   : safetyNumberUnavailable.height

                    TextEdit {
                        id: safetyNumberText
                        objectName: "safetyNumberText"
                        anchors.left: parent.left
                        anchors.right: parent.right
                        visible: root.number.length > 0
                        readOnly: true
                        selectByMouse: true
                        text: root.number
                        color: "#2b3b53"
                        font.family: "Courier New"
                        font.pixelSize: 17
                        // WordWrap, not WrapAnywhere: the number arrives as
                        // space-joined 5-digit groups, and breaking mid-group makes
                        // it far easier to misread aloud when comparing with a
                        // contact. Every group is short enough to always fit.
                        wrapMode: TextEdit.WordWrap
                        renderType: Text.NativeRendering
                    }

                    Text {
                        id: safetyNumberUnavailable
                        objectName: "safetyNumberUnavailable"
                        anchors.left: parent.left
                        anchors.right: parent.right
                        visible: root.number.length === 0
                        text: "Safety number not available yet."
                        color: Theme.textSecondary
                        wrapMode: Text.WordWrap
                        font.family: Theme.uiFont
                        font.pixelSize: 14
                        renderType: Text.NativeRendering
                    }
                }
            }

            // Verify action + verified badge share a row: the button while unverified,
            // the green pill once the user has asserted a match.
            Item {
                width: parent.width
                height: 40

                AeroButton {
                    objectName: "markVerifiedButton"
                    anchors.left: parent.left
                    width: 150
                    height: parent.height
                    label: "Mark as verified"
                    fontPixelSize: 15
                    enabled: root.contactController && !root.verified && root.number.length > 0
                    onClicked: {
                        if (root.contactController)
                            root.contactController.markVerified();
                    }
                }

                Rectangle {
                    objectName: "safetyNumberVerifiedBadge"
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    visible: root.verified
                    width: verifiedLabel.width + 26
                    height: 26
                    radius: 13
                    color: "#2e7d4f"

                    Text {
                        id: verifiedLabel
                        anchors.centerIn: parent
                        text: "Verified ✓"
                        color: "#ffffff"
                        font.family: Theme.uiFont
                        font.pixelSize: 13
                        renderType: Text.NativeRendering
                    }
                }
            }

            // A short explanatory line: comparing the number out of band rules out an
            // eavesdropper.
            Text {
                width: parent.width
                text: "Compare this number with your contact in person or over another "
                      + "channel. If it matches on both sides, no one is intercepting "
                      + "your messages."
                color: Theme.textSecondary
                wrapMode: Text.WordWrap
                font.family: Theme.uiFont
                font.pixelSize: 13
                renderType: Text.NativeRendering
            }
        }
    }
}
