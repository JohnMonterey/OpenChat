pragma ComponentBehavior: Bound

import QtQuick
import OpenChat
import OpenChat.Native

// What goes with the next message, above the field: a card for each staged
// attachment in the order it was picked, scrolling sideways once they outgrow
// the row. Cards pop in and shrink away; the rest slide over. While Enter is
// waiting for one still being prepared, the row says so after the last card.
Item {
    id: tray
    objectName: "stagedAttachments"
    property var controller: null
    readonly property var model: controller !== null && controller.stagedAttachments ? controller.stagedAttachments : null
    readonly property bool shown: controller !== null && controller.hasStagedAttachments === true
    readonly property bool waiting: controller !== null && controller.sendWhenReady === true
    readonly property real motion: ProfileRenderPolicy.animationsAllowed ? 1 : 0

    visible: shown
    implicitHeight: 60

    ListView {
        id: cards
        objectName: "stagedAttachmentList"
        pixelAligned: true // see MessageHistory
        anchors.fill: parent
        orientation: ListView.Horizontal
        spacing: 8
        model: tray.model
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        interactive: contentWidth > width
        Accessible.role: Accessible.List
        Accessible.name: "Attachments to send"

        delegate: StagedAttachmentCard {
            onRemoveRequested: stagedId => {
                if (typeof tray.controller.removeStagedAttachment === "function")
                    tray.controller.removeStagedAttachment(stagedId);
            }
        }

        footer: Item {
            width: waitingText.visible ? waitingText.implicitWidth + 20 : 0
            height: cards.height
            Text {
                id: waitingText
                objectName: "stagedAttachmentsWaiting"
                visible: tray.waiting
                x: 12
                anchors.verticalCenter: parent.verticalCenter
                text: "Sends when ready…"
                color: Theme.textSecondary
                font.family: Theme.uiFont
                font.pixelSize: 12
                font.italic: true
                renderType: Text.NativeRendering
            }
        }

        add: Transition {
            ParallelAnimation {
                NumberAnimation {
                    property: "scale"
                    from: 0.8
                    to: 1
                    duration: 140 * tray.motion
                    easing.type: Easing.OutCubic
                }
                NumberAnimation {
                    property: "opacity"
                    from: 0
                    to: 1
                    duration: 140 * tray.motion
                    easing.type: Easing.OutCubic
                }
            }
        }
        remove: Transition {
            ParallelAnimation {
                NumberAnimation {
                    property: "scale"
                    to: 0.8
                    duration: 140 * tray.motion
                    easing.type: Easing.InCubic
                }
                NumberAnimation {
                    property: "opacity"
                    to: 0
                    duration: 140 * tray.motion
                    easing.type: Easing.InCubic
                }
            }
        }
        displaced: Transition {
            NumberAnimation {
                properties: "x"
                duration: 140 * tray.motion
                easing.type: Easing.OutCubic
            }
        }
    }
}
