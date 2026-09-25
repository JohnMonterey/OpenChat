import QtQuick
import OpenChat
import OpenChat.Native

// A panel's video: its poster frame (or a dark frame) with a play button and
// its length until it is played, then the clip itself (ProfileClipPlayer),
// with its sound, looping if its owner asked. Nothing plays by itself.
Item {
    id: video
    // ProfilePageObject.block() of a video block.
    property var entry: ({})
    property Item panelBlock: null
    readonly property bool present: entry.present === true
    readonly property real aspect: entry.width > 0 && entry.height > 0 ? entry.width / entry.height : 16 / 9
    readonly property bool preview: panelBlock !== null && panelBlock.preview
    property bool playing: false

    implicitHeight: Math.round(width / Math.max(0.5, Math.min(2.4, aspect)))
    height: implicitHeight
    activeFocusOnTab: !preview
    Accessible.role: Accessible.Button
    Accessible.name: playing ? "Stop video" : "Play video"
    Keys.onSpacePressed: toggle()
    Keys.onReturnPressed: toggle()

    // A call ringing or starting silences the page, as it does its song.
    Connections {
        target: video.panelBlock && video.panelBlock.view ? video.panelBlock.view.profiles : null
        ignoreUnknownSignals: true
        function onViewerChanged() {
            if (target.callActive)
                video.playing = false;
        }
    }

    function toggle() {
        if (video.preview) {
            if (video.panelBlock && video.panelBlock.view)
                video.panelBlock.view.editRequested("panel:" + video.entry.panelId);
            return;
        }
        if (video.present)
            video.playing = !video.playing;
    }
    function formatDuration(ms) {
        const seconds = Math.max(0, Math.round(ms / 1000));
        return Math.floor(seconds / 60) + ":" + ("0" + seconds % 60).slice(-2);
    }

    Rectangle {
        anchors.fill: parent
        radius: 6
        color: "#1b1f27"
        clip: true

        ProfilePanelImage {
            anchors.fill: parent
            visible: !video.playing
            mediaKey: video.entry.posterKey || ""
            crop: false
            radius: 6
        }
        Loader {
            id: player
            anchors.fill: parent
            active: video.playing
            sourceComponent: ProfileClipPlayer {
                segmentKeys: video.entry.segmentKeys || []
                loop: video.entry.loop === true
                playing: true
                onFinished: video.playing = false
            }
        }

        // The play button and the length, over the poster.
        Rectangle {
            visible: !video.playing
            anchors.centerIn: parent
            width: 54
            height: 54
            radius: 27
            color: "#a0000000"
            border.width: 2
            border.color: "#e6ffffff"
            ProfileGlyph {
                anchors.centerIn: parent
                anchors.horizontalCenterOffset: 2
                width: 26
                height: 26
                kind: video.present || video.preview ? "play" : "film"
                ink: "white"
            }
        }
        Rectangle {
            visible: !video.playing && (video.entry.durationMs || 0) > 0
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 8
            width: lengthText.implicitWidth + 10
            height: lengthText.implicitHeight + 4
            radius: 3
            color: "#b0000000"
            Text {
                id: lengthText
                anchors.centerIn: parent
                text: video.present || video.preview ? video.formatDuration(video.entry.durationMs || 0)
                                                     : "Loading…"
                color: "white"
                font.family: Theme.uiFont
                font.pixelSize: 11
                renderType: Text.NativeRendering
            }
        }
        MouseArea {
            anchors.fill: parent
            cursorShape: video.present || video.preview ? Qt.PointingHandCursor : Qt.ArrowCursor
            onClicked: video.toggle()
        }
    }
    ProfileFocusRing {
        anchors.fill: parent
        radius: 6
        shown: video.activeFocus
    }
}
