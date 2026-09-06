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
    // The share stage gets its own cap rather than a multiple of the camera's:
    // a desktop wants more height than a face, but not so much that the
    // conversation underneath disappears at the minimum window size.
    property real maxShareHeight: 300
    // The window-level enlarge overlay, handed down to the share stage so an
    // enlarged share is encoded at the size it is drawn.
    property Item zoom: null
    // True while this surface is the whole window rather than the top of the
    // conversation. Its height is then the window's, not its own, so the
    // content centres itself and the pictures grow to use the room.
    property bool fullscreen: false
    // The height the surface has when it fills the window. Handed in rather
    // than read from `height`: the slot sizes itself from this surface's
    // implicit height, and the caps below feed that, so reading our own height
    // here would be a loop the moment the two modes swap.
    property real availableHeight: height
    // Raised by the corner chip; the window decides what to collapse.
    signal fullscreenToggled
    // Raised by any zoom chip on any picture here, with the video view to copy.
    signal enlargeRequested(Item videoItem)
    readonly property bool isGroupCall: controller.isGroupCall === true
    readonly property bool hasVideo: controller.cameraEnabled || controller.remoteCameraEnabled
    readonly property real videoWidth: Math.max(100,
        (width - 48 - (hasVideo ? 22 : 94)
         - (controller.cameraEnabled && controller.remoteCameraEnabled ? 0 : 132))
        / (controller.cameraEnabled && controller.remoteCameraEnabled ? 2 : 1))
    // In a group every camera shares the row, so each gets a narrower slot.
    readonly property real groupVideoWidth: Math.max(100, Math.min(fullscreen ? 480 : 260,
                                                                   (width - 60) / 3))
    readonly property real participantsHeight: isGroupCall ? groupParticipants.height
                                                           : participants.height
    // Everything below the pictures — status, actions, errors, and the air
    // around them — which is what a full-window layout has to leave room for.
    readonly property real chromeHeight: 98 + (isGroupCall ? 18 : 0)
                                         + (mediaError.visible ? mediaError.implicitHeight + 8 : 0)
    // The caps the pictures are actually laid out with. Filling the window,
    // a lone camera row takes what the chrome leaves; with a share on stage
    // the cameras stay modest and the share gets the rest.
    readonly property real videoHeightCap: !fullscreen ? maxVideoHeight
        : (screenStage.visible ? Math.min(240, availableHeight * 0.28)
                               : Math.max(120, availableHeight - chromeHeight - 100))
    readonly property real shareHeightCap: !fullscreen ? Math.max(140, maxShareHeight)
        : Math.max(140, availableHeight - participantsHeight - chromeHeight - 70)
    // The height this surface asks for when it is only the top of the pane.
    readonly property real naturalHeight: Math.max(Theme.callHeaderHeight, participantsHeight + 98)
                    + (isGroupCall ? 18 : 0)
                    + (screenStage.visible ? screenStage.implicitHeight + 10 : 0)
                    + (mediaError.visible ? mediaError.implicitHeight + 8 : 0)
    // Where the content starts: at the top, or — given more height than it
    // needs — centred in it.
    readonly property real contentTop: fullscreen
        ? Math.max(18, 18 + (availableHeight - naturalHeight) / 2) : 18
    implicitHeight: naturalHeight

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
        anchors.topMargin: callHeader.contentTop - 8
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
        anchors.topMargin: callHeader.contentTop + (callHeader.isGroupCall ? 16 : 0)
        anchors.horizontalCenter: parent.horizontalCenter
        width: Math.min(parent.width - 40, 5 * 132 + 4 * 14)
        spacing: 14

        CallParticipant {
            objectName: "groupLocalParticipant"
            cameraEnabled: callHeader.controller.cameraEnabled
            videoFrame: callHeader.controller.localVideoFrame
            videoAspect: callHeader.controller.localVideoAspect
            videoMaxWidth: callHeader.groupVideoWidth
            videoMaxHeight: callHeader.videoHeightCap
            mirrored: true
            onEnlargeRequested: videoItem => callHeader.enlargeRequested(videoItem)
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
                videoMaxHeight: callHeader.videoHeightCap
                caption: stateText
                dimmed: !joined && !ringing
                onEnlargeRequested: videoItem => callHeader.enlargeRequested(videoItem)
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
        anchors.topMargin: callHeader.contentTop
        spacing: callHeader.hasVideo ? 22 : 34

        CallParticipant {
            id: localParticipant
            objectName: "localParticipant"
            cameraEnabled: callHeader.controller.cameraEnabled
            videoFrame: callHeader.controller.localVideoFrame
            videoAspect: callHeader.controller.localVideoAspect
            videoMaxWidth: callHeader.videoWidth
            videoMaxHeight: callHeader.videoHeightCap
            mirrored: true
            onEnlargeRequested: videoItem => callHeader.enlargeRequested(videoItem)
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
            videoMaxHeight: callHeader.videoHeightCap
            name: callHeader.controller.peerName
            avatarKey: callHeader.controller.peerAvatarKey
            speaking: callHeader.controller.remoteSpeaking
            level: callHeader.controller.remoteLevel
            onEnlargeRequested: videoItem => callHeader.enlargeRequested(videoItem)
        }
    }

    // Appears by itself when a share starts — ours or theirs — and is gone the
    // moment it stops, exactly as an incoming camera tile does above it.
    ScreenShareStage {
        id: screenStage
        controller: callHeader.controller
        anchors.top: callHeader.isGroupCall ? groupParticipants.bottom : participants.bottom
        anchors.topMargin: visible ? 10 : 0
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: 24
        anchors.rightMargin: 24
        height: implicitHeight
        maxHeight: callHeader.shareHeightCap
        zoom: callHeader.zoom
        onEnlargeRequested: videoItem => callHeader.enlargeRequested(videoItem)
    }

    Text {
        id: statusLine
        objectName: "callStatusText"
        anchors.top: screenStage.visible
                     ? screenStage.bottom
                     : (callHeader.isGroupCall ? groupParticipants.bottom : participants.bottom)
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
        // The room the full-screen chip needs at the right edge: its width,
        // its margin, and a gap.
        readonly property real chipReserve: fullscreenChip.width + 8 + 6
        // The row's width with its usual spacing, from the buttons' own widths
        // so it can be known before the spacing is chosen from it.
        readonly property real naturalWidth: {
            let total = 0;
            let count = 0;
            for (const child of actions.children) {
                if (!child.visible)
                    continue;
                total += child.width;
                ++count;
            }
            return total + Math.max(0, count - 1) * 12;
        }
        // At the minimum window width the live-call row only just fits the
        // pane; rather than run under the chip it closes up and leans left.
        readonly property bool tight: naturalWidth + 2 * chipReserve > callHeader.width
        anchors.top: statusLine.bottom
        anchors.topMargin: 12
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.horizontalCenterOffset: Math.max(
            -Math.max(0, (callHeader.width - actions.width) / 2 - 4),
            Math.min(0, (callHeader.width - actions.width) / 2 - chipReserve))
        spacing: tight ? 6 : 12

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
            objectName: "screenShareButton"
            visible: !callHeader.controller.isRinging && !callHeader.controller.callEnded
            label: callHeader.controller.screenShareEnabled ? "Stop sharing" : "Share screen"
            screenIcon: true
            checked: callHeader.controller.screenShareEnabled === true
            disabled: callHeader.controller.screenShareAvailable !== true
            tooltip: callHeader.controller.screenShareAvailable !== true
                     ? "Screen sharing is not available on this system"
                     : (callHeader.controller.screenShareEnabled
                        ? "Stop sharing your screen"
                        : "Share a screen or a window")
            onClicked: callHeader.controller.toggleScreenShare()
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

    // A camera failure and a screen failure are different facts, and either can
    // be true while the other is not — a machine with no webcam can share its
    // screen perfectly well. They get a line each, so a camera error left over
    // from earlier in the call cannot hide what just happened to a share.
    Column {
        id: mediaError
        objectName: "callMediaErrors"
        anchors.top: actions.bottom
        anchors.topMargin: 8
        anchors.horizontalCenter: parent.horizontalCenter
        width: parent.width - 40
        spacing: 3
        visible: cameraErrorText.visible || screenShareErrorText.visible

        Text {
            id: cameraErrorText
            objectName: "cameraErrorText"
            width: parent.width
            visible: text.length > 0
            text: callHeader.controller.cameraError
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            color: Theme.declineBottom
            font.family: Theme.uiFont
            font.pixelSize: 12
        }

        Text {
            id: screenShareErrorText
            objectName: "screenShareErrorText"
            width: parent.width
            visible: text.length > 0
            text: callHeader.controller.screenShareError !== undefined
                  ? callHeader.controller.screenShareError : ""
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            color: Theme.declineBottom
            font.family: Theme.uiFont
            font.pixelSize: 12
        }
    }

    // The corner of the call surface: fill the window with it, or give the
    // sidebar and the conversation back.
    MediaIconButton {
        id: fullscreenChip
        objectName: "callFullscreenButton"
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 8
        icon: callHeader.fullscreen ? "collapse" : "expand"
        tooltip: callHeader.fullscreen ? "Exit full screen" : "Full screen"
        onClicked: callHeader.fullscreenToggled()
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: Theme.rule
        visible: !callHeader.fullscreen
    }
}
