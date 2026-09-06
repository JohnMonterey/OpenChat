import QtQuick
import OpenChat
import OpenChat.Native

// The strip a shared screen appears on.
//
// It behaves exactly like the camera tiles above it — it appears by itself the
// moment a share starts and is gone the moment it stops — and it is built from
// the same CallVideoItem those tiles use. What differs is the shape: a desktop
// is wide and full of small text, so it gets the width of the panel rather than
// a square slot beside a face.
//
// The panel also tells the controller how large it is actually drawing the
// remote share, which travels back to the sender: a share in a narrow strip is
// not encoded at 1080p, and a share in a panel that is not on screen at all is
// not encoded at all.
Item {
    id: stage
    objectName: "screenShareStage"

    required property var controller
    property real maxHeight: 300

    readonly property bool remoteActive: stage.controller.remoteScreenShareActive === true
    readonly property bool localActive: stage.controller.screenShareEnabled === true
    visible: stage.remoteActive || stage.localActive

    readonly property real captionHeight: 16
    readonly property real localPaneWidth: stage.localActive ? 150 : 0
    readonly property real remoteAspect: Math.max(0.2, stage.controller.remoteScreenAspect)
    readonly property real remotePaneWidth: !stage.remoteActive ? 0
        : Math.max(160, Math.min(stage.width - stage.localPaneWidth
                                 - (stage.localActive ? 14 : 0),
                                 stage.maxHeight * stage.remoteAspect))
    readonly property real remotePaneHeight: stage.remoteActive
        ? stage.remotePaneWidth / stage.remoteAspect : 0
    readonly property real localAspect: Math.max(0.2, localScreenVideo.sourceAspect)
    readonly property real localPaneHeight: stage.localActive
        ? stage.localPaneWidth / stage.localAspect : 0

    implicitHeight: stage.visible
        ? Math.max(stage.remotePaneHeight, stage.localPaneHeight) + stage.captionHeight + 8
        : 0

    // What the sender is told about this view. Zero while the panel is not
    // being shown, which is the sender's cue to stop encoding for us entirely.
    function reportViewSize() {
        if (!stage.controller)
            return;
        const showing = stage.visible && stage.remoteActive;
        stage.controller.setRemoteScreenViewSize(showing ? Math.round(stage.remotePaneWidth) : 0,
                                                 showing ? Math.round(stage.remotePaneHeight) : 0);
    }

    onRemotePaneWidthChanged: stage.reportViewSize()
    onRemotePaneHeightChanged: stage.reportViewSize()
    onVisibleChanged: stage.reportViewSize()
    Component.onCompleted: stage.reportViewSize()
    Component.onDestruction: {
        if (stage.controller)
            stage.controller.setRemoteScreenViewSize(0, 0);
    }

    Row {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        spacing: 14

        // The far end's desktop.
        Item {
            width: stage.remotePaneWidth
            height: stage.remotePaneHeight + stage.captionHeight + 8
            visible: stage.remoteActive

            Rectangle {
                objectName: "remoteScreenFrame"
                width: stage.remotePaneWidth
                height: stage.remotePaneHeight
                radius: 8
                color: Theme.callBackdropBottom
                border.width: 1
                border.color: Theme.idleRing

                CallVideoItem {
                    id: remoteScreenVideo
                    objectName: "remoteScreenVideo"
                    anchors.fill: parent
                    anchors.margins: 1
                    canvas: stage.controller.remoteScreenCanvas
                }

                // The first sweep of a share fills the canvas in over a moment.
                // Saying so beats showing black squares as if the far end's
                // desktop had holes in it.
                Text {
                    objectName: "remoteScreenLoading"
                    anchors.centerIn: parent
                    visible: stage.remoteActive && !remoteScreenVideo.canvasComplete
                    text: "Receiving screen…"
                    color: Theme.textSecondary
                    font.family: Theme.uiFont
                    font.pixelSize: 13
                    renderType: Text.NativeRendering
                }
            }

            Text {
                objectName: "remoteScreenCaption"
                anchors.top: parent.top
                anchors.topMargin: stage.remotePaneHeight + 6
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
                text: stage.controller.remoteScreenSharerName.length > 0
                      ? stage.controller.remoteScreenSharerName + "'s screen"
                      : "Shared screen"
                color: Theme.textSecondary
                font.family: Theme.uiFont
                font.pixelSize: 12
                renderType: Text.NativeRendering
            }
        }

        // Our own share, small: enough to be sure the right window is going
        // out, nowhere near the resolution it is going out at.
        Item {
            width: stage.localPaneWidth
            height: stage.localPaneHeight + stage.captionHeight + 8
            visible: stage.localActive

            Rectangle {
                objectName: "localScreenFrame"
                width: stage.localPaneWidth
                height: stage.localPaneHeight
                radius: 8
                color: Theme.callBackdropBottom
                border.width: 1
                border.color: Theme.speakingRing

                CallVideoItem {
                    id: localScreenVideo
                    objectName: "localScreenVideo"
                    anchors.fill: parent
                    anchors.margins: 1
                    frame: stage.controller.localScreenPreview
                }
            }

            Text {
                objectName: "localScreenCaption"
                anchors.top: parent.top
                anchors.topMargin: stage.localPaneHeight + 6
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
                text: stage.controller.screenShareSourceName.length > 0
                      ? stage.controller.screenShareSourceName : "Your screen"
                color: Theme.textSecondary
                font.family: Theme.uiFont
                font.pixelSize: 12
                renderType: Text.NativeRendering
            }
        }
    }
}
