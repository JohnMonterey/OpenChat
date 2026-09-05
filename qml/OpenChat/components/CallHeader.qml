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
    property real maxVideoHeight: 210
    readonly property bool isGroupCall: controller.isGroupCall === true
    readonly property bool hasVideo: controller.cameraEnabled || controller.remoteCameraEnabled
    readonly property real videoWidth: Math.max(100,
        (width - 48 - (hasVideo ? 22 : 94)
         - (controller.cameraEnabled && controller.remoteCameraEnabled ? 0 : 132))
        / (controller.cameraEnabled && controller.remoteCameraEnabled ? 2 : 1))
    // In a group every camera shares the row, so each gets a narrower slot.
    readonly property real groupVideoWidth: Math.max(100, Math.min(260, (width - 60) / 3))
    readonly property real participantsHeight: isGroupCall ? groupParticipants.height
                                                           : participants.height
    implicitHeight: Math.max(Theme.callHeaderHeight, participantsHeight + 98)
                    + (isGroupCall ? 18 : 0)
                    + (cameraError.visible ? cameraError.implicitHeight + 8 : 0)

    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0; color: Theme.callBackdropTop }
            GradientStop { position: 1; color: Theme.callBackdropBottom }
        }
    }

    // The group's name over the people in it, group calls only.
    Text {
        id: groupTitle
        objectName: "groupCallTitle"
        anchors.top: parent.top
        anchors.topMargin: 10
        anchors.horizontalCenter: parent.horizontalCenter
        width: parent.width - 40
        horizontalAlignment: Text.AlignHCenter
        elide: Text.ElideRight
        visible: callHeader.isGroupCall
        text: callHeader.controller.groupTitle
        color: Theme.textPrimary
        font.family: Theme.uiFont
        font.pixelSize: 15
        font.bold: true
        renderType: Text.NativeRendering
    }

    // Everyone on a group call, us first, each captioned with what they are
    // doing. Wraps onto further rows when the group outgrows the width.
    Flow {
        id: groupParticipants
        objectName: "groupParticipants"
        visible: callHeader.isGroupCall
        anchors.top: parent.top
        anchors.topMargin: callHeader.isGroupCall ? 34 : 18
        anchors.horizontalCenter: parent.horizontalCenter
        width: Math.min(parent.width - 40, 5 * 132 + 4 * 14)
        spacing: 14

        CallParticipant {
            objectName: "groupLocalParticipant"
            cameraEnabled: callHeader.controller.cameraEnabled
            videoFrame: callHeader.controller.localVideoFrame
            videoAspect: callHeader.controller.localVideoAspect
            videoMaxWidth: callHeader.groupVideoWidth
            videoMaxHeight: callHeader.maxVideoHeight
            mirrored: true
            name: callHeader.controller.localName.length > 0
                  ? callHeader.controller.localName : "You"
            avatarKey: callHeader.controller.localAvatarKey
            speaking: callHeader.controller.localSpeaking
            level: callHeader.controller.localLevel
            muted: callHeader.controller.muted
        }

        Repeater {
            model: callHeader.isGroupCall ? callHeader.controller.participants : null

            CallParticipant {
                id: member
                // The model's rows fill the participant's own properties (name,
                // picture, speaking ring, camera) by role name; the rest are the
                // group-only roles declared here.
                required property string deviceId
                required property string stateText
                required property bool joined
                required property bool ringing
                required speaking
                required level
                required videoFrame
                required cameraEnabled
                required videoAspect
                objectName: "groupParticipant_" + deviceId
                videoMaxWidth: callHeader.groupVideoWidth
                videoMaxHeight: callHeader.maxVideoHeight
                caption: stateText
                dimmed: !joined && !ringing
            }
        }
    }

    // Each picture grows only when that participant shares their camera.
    Row {
        id: participants
        objectName: "callParticipants"
        visible: !callHeader.isGroupCall
        height: Math.max(localParticipant.height, remoteParticipant.height)
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: 18
        spacing: callHeader.hasVideo ? 22 : 34

        CallParticipant {
            id: localParticipant
            objectName: "localParticipant"
            cameraEnabled: callHeader.controller.cameraEnabled
            videoFrame: callHeader.controller.localVideoFrame
            videoAspect: callHeader.controller.localVideoAspect
            videoMaxWidth: callHeader.videoWidth
            videoMaxHeight: callHeader.maxVideoHeight
            mirrored: true
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
            visible: !callHeader.hasVideo
            width: 26
            height: Math.max(localParticipant.height, remoteParticipant.height)

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
            id: remoteParticipant
            objectName: "remoteParticipant"
            cameraEnabled: callHeader.controller.remoteCameraEnabled
            videoFrame: callHeader.controller.remoteVideoFrame
            videoAspect: callHeader.controller.remoteVideoAspect
            videoMaxWidth: callHeader.videoWidth
            videoMaxHeight: callHeader.maxVideoHeight
            name: callHeader.controller.peerName
            avatarKey: callHeader.controller.peerAvatarKey
            speaking: callHeader.controller.remoteSpeaking
            level: callHeader.controller.remoteLevel
        }
    }

    Text {
        id: statusLine
        objectName: "callStatusText"
        anchors.top: callHeader.isGroupCall ? groupParticipants.bottom : participants.bottom
        anchors.topMargin: 8
        anchors.horizontalCenter: parent.horizontalCenter
        text: callHeader.controller.isActive ? callHeader.controller.durationText
                                            : callHeader.controller.statusText
        color: Theme.textSecondary
        font.family: Theme.uiFont
        font.pixelSize: 14
        renderType: Text.NativeRendering
    }

    // Answer / decline while an incoming call is ringing; mute / hang up once it
    // is ours to control. The two sets never appear together, so the row below
    // always means exactly one thing.
    Row {
        id: actions
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
            objectName: "cameraCallButton"
            visible: !callHeader.controller.isRinging && !callHeader.controller.callEnded
            label: callHeader.controller.cameraEnabled ? "Camera off" : "Camera on"
            cameraIcon: true
            checked: callHeader.controller.cameraEnabled
            onClicked: callHeader.controller.toggleCamera()
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

    Text {
        id: cameraError
        objectName: "cameraErrorText"
        anchors.top: actions.bottom
        anchors.topMargin: 8
        anchors.horizontalCenter: parent.horizontalCenter
        width: parent.width - 40
        visible: text.length > 0
        text: callHeader.controller.cameraError
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        color: Theme.declineBottom
        font.family: Theme.uiFont
        font.pixelSize: 12
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: Theme.rule
    }
}
