import QtQuick
import QtQuick.Shapes
import OpenChat

// The in-call surface. It occupies the top of the conversation pane in place of
// the conversation header, so while a call is running the contact's name,
// picture and call buttons are gone and the two people on the call are shown
// together instead — one panel, both faces, the talker ringed in green.
//
// It deliberately replaces rather than overlays the header: a call is the whole
// point of the surface while it lasts, and leaving the old header underneath
// would mean two competing pictures of the same contact.
Item {
    id: callHeader
    objectName: "callHeader"
    required property var controller
    implicitHeight: Theme.callHeaderHeight

    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0; color: Theme.callBackdropTop }
            GradientStop { position: 1; color: Theme.callBackdropBottom }
        }
    }

    // The two callers, side by side and equally weighted: a call is between
    // peers, so neither picture is larger or more central than the other.
    Row {
        id: participants
        objectName: "callParticipants"
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: 18
        spacing: 34

        CallParticipant {
            objectName: "localParticipant"
            name: callHeader.controller.localName.length > 0
                  ? callHeader.controller.localName : "You"
            avatarKey: callHeader.controller.localAvatarKey
            speaking: callHeader.controller.localSpeaking
            level: callHeader.controller.localLevel
            muted: callHeader.controller.muted
        }

        // A quiet reminder of who is connected to whom, sitting between the two
        // pictures where a link belongs.
        Item {
            width: 26
            height: participants.height

            Text {
                anchors.centerIn: parent
                anchors.verticalCenterOffset: -18
                text: "↔"
                color: Theme.textSecondary
                font.family: Theme.uiFont
                font.pixelSize: 18
                renderType: Text.NativeRendering
            }
        }

        CallParticipant {
            objectName: "remoteParticipant"
            name: callHeader.controller.peerName
            avatarKey: callHeader.controller.peerAvatarKey
            speaking: callHeader.controller.remoteSpeaking
            level: callHeader.controller.remoteLevel
        }
    }

    Text {
        id: statusLine
        objectName: "callStatusText"
        anchors.top: participants.bottom
        anchors.topMargin: 8
        anchors.horizontalCenter: parent.horizontalCenter
        text: callHeader.controller.statusText
        color: Theme.textSecondary
        font.family: Theme.uiFont
        font.pixelSize: 14
        renderType: Text.NativeRendering
    }

    // Answer / decline while an incoming call is ringing; mute / hang up once it
    // is ours to control. The two sets never appear together, so the row below
    // always means exactly one thing.
    Row {
        objectName: "callActions"
        anchors.top: statusLine.bottom
        anchors.topMargin: 12
        anchors.horizontalCenter: parent.horizontalCenter
        spacing: 12

        CallActionButton {
            objectName: "acceptCallButton"
            visible: callHeader.controller.isRinging
            label: "Answer"
            accent: "accept"
            onClicked: callHeader.controller.acceptCall()
        }
        CallActionButton {
            objectName: "declineCallButton"
            visible: callHeader.controller.isRinging
            label: "Decline"
            accent: "end"
            onClicked: callHeader.controller.declineCall()
        }
        CallActionButton {
            objectName: "muteCallButton"
            visible: !callHeader.controller.isRinging && !callHeader.controller.callEnded
            label: callHeader.controller.muted ? "Unmute" : "Mute"
            accent: "neutral"
            onClicked: callHeader.controller.toggleMute()
        }
        CallActionButton {
            objectName: "endCallButton"
            visible: !callHeader.controller.isRinging && !callHeader.controller.callEnded
            label: "End call"
            accent: "end"
            onClicked: callHeader.controller.hangUp()
        }
        CallActionButton {
            objectName: "dismissCallButton"
            visible: callHeader.controller.callEnded
            label: "Back to chat"
            accent: "neutral"
            onClicked: callHeader.controller.dismissCall()
        }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: Theme.rule
    }
}
