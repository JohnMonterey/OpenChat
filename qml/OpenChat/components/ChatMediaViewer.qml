import QtQuick
import OpenChat
import OpenChat.Native

// A photo or a video from the chat, large over the whole window. It grows out
// of its bubble and shrinks back into it (MediaZoomOverlay's way), over a
// darkened window; nothing under it takes a click, and a click outside the
// picture, Esc or the cross closes it. A photo is fitted whole, with Save and
// Copy image; a video plays at once, with play and pause, a seek bar and the
// time, its poster showing until the first frame is decoded.
//
// It holds its own media (the whole photo, the video's segments) through its
// own handle, so the bubble it came from can scroll away. It lets go of all of
// it on closing, and closes by itself when the chat changes, when the chat's
// messages are hidden (a lock, a device change) and when a profile covers the
// window; a call pauses the video.
Item {
    id: viewer
    objectName: "chatMediaViewer"
    property var controller: null
    // A call rings or runs: the video holds still.
    property bool callActive: false
    // What is open: the message and its kind (1 photo, 2 video), with what
    // its descriptor says about it (caption, shape, length).
    property string stableId: ""
    property int kind: 0
    property string caption: ""
    property real mediaAspect: 4 / 3
    property real durationMs: 0
    // The bubble's picture it grew from, while that still exists.
    property Item sourceItem: null
    // True from the moment it is asked to grow until it is asked to shrink.
    property bool expanded: false
    readonly property bool open: stableId.length > 0
    readonly property int duration: ProfileRenderPolicy.animationsAllowed ? 220 : 0
    signal saveRequested(string stableId)
    // It let go of what it showed; the keyboard can go back where it was.
    signal closed

    // The rectangle the picture grows into: as much of the window as its
    // shape allows, with room above for the buttons and, for a video, below
    // for its controls and a caption.
    readonly property real aspect: picture.sourceAspect > 0 ? picture.sourceAspect
        : poster.sourceAspect > 0 ? poster.sourceAspect : mediaAspect
    readonly property real topRoom: 64
    readonly property real bottomRoom: (kind === 2 ? 64 : 24) + (captionText.visible ? captionText.height + 12 : 0)
    // Whole, whatever its shape: a narrow full-page screenshot is as tall as
    // the room and as narrow as it is, never cut off above and below.
    readonly property real roomWidth: Math.max(1, width - 56)
    readonly property real roomHeight: Math.max(1, height - topRoom - bottomRoom)
    readonly property real fitHeight: Math.max(1, Math.min(roomHeight, roomWidth / aspect))
    readonly property real fitWidth: Math.max(1, fitHeight * aspect) // a pixel at least, to draw
    property rect tileRect: Qt.rect(0, 0, 0, 0)

    anchors.fill: parent
    visible: open
    enabled: open
    Accessible.role: Accessible.Dialog
    Accessible.name: (kind === 2 ? "Video" : "Photo") + (caption.length > 0 ? ": " + caption : "")

    // Opens `info` (stableId, attachmentKind, mediaWidth, mediaHeight,
    // durationMs, caption) grown from `source`.
    function show(info, source) {
        if (!info || !(info.attachmentKind === 1 || info.attachmentKind === 2))
            return;
        settle.stop();
        controls.active = false;
        viewer.kind = info.attachmentKind;
        viewer.caption = info.caption || "";
        viewer.mediaAspect = info.mediaWidth > 0 && info.mediaHeight > 0 ? info.mediaWidth / info.mediaHeight : 4 / 3;
        viewer.durationMs = info.durationMs || 0;
        viewer.sourceItem = source || null;
        viewer.stableId = info.stableId;
        viewer.refreshTileRect();
        frame.animate = false;
        viewer.expanded = false;
        frame.animate = true;
        viewer.expanded = true;
        // A video plays as soon as it is open: the bubble's button was Play.
        if (viewer.kind === 2)
            controls.active = true;
        viewer.forceActiveFocus();
    }

    // Shrinks back onto the bubble (or just fades, when the bubble is gone)
    // and lets go. `instant` skips the animation.
    function close(instant) {
        if (!viewer.open)
            return;
        controls.active = false;
        if (instant || viewer.duration === 0 || !viewer.visible) {
            settle.stop();
            viewer.expanded = false;
            viewer.release();
            return;
        }
        viewer.refreshTileRect();
        viewer.expanded = false;
        settle.restart();
    }

    function release() {
        const wasFocused = viewer.activeFocus;
        viewer.stableId = "";
        viewer.sourceItem = null;
        viewer.kind = 0;
        if (wasFocused)
            viewer.closed();
    }

    function refreshTileRect() {
        if (viewer.sourceItem && viewer.sourceItem.visible) {
            const origin = viewer.sourceItem.mapToItem(viewer, 0, 0);
            viewer.tileRect = Qt.rect(origin.x, origin.y, viewer.sourceItem.width, viewer.sourceItem.height);
        } else {
            // Nothing to go back to: grow from, and shrink to, the middle.
            viewer.tileRect = Qt.rect(viewer.width / 2 - 40, viewer.height / 2 - 30, 80, 60);
        }
    }

    function clock(ms) {
        const seconds = Math.max(0, Math.round(ms / 1000));
        return Math.floor(seconds / 60) + ":" + ("0" + seconds % 60).slice(-2);
    }

    Timer {
        id: settle
        interval: viewer.duration + 30
        onTriggered: {
            if (!viewer.expanded)
                viewer.release();
        }
    }

    readonly property string chatId: controller ? controller.currentContactId : ""
    readonly property bool plaintextVisible: controller ? controller.plaintextVisible === true : false
    onChatIdChanged: viewer.close(true)
    onPlaintextVisibleChanged: {
        if (!viewer.plaintextVisible)
            viewer.close(true);
    }
    onCallActiveChanged: {
        if (viewer.callActive && clip.item)
            clip.item.paused = true;
    }

    Shortcut {
        sequences: ["Escape"]
        enabled: viewer.expanded
        onActivated: viewer.close(false)
    }
    Keys.onSpacePressed: viewer.togglePlay()

    // The whole photo, or the video's segments, for as long as it is open.
    ChatAttachmentMedia {
        id: media
        objectName: "chatMediaViewerMedia"
        controller: viewer.controller
        stableId: viewer.stableId
        transferState: viewer.open ? 1 : 0
        wantFull: viewer.kind === 1
        wantSegments: viewer.kind === 2 && controls.active
    }

    Rectangle {
        id: scrim
        objectName: "chatMediaViewerScrim"
        anchors.fill: parent
        color: Theme.zoomScrim
        opacity: viewer.expanded ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: viewer.duration } }

        // Every click that is not on the picture or its controls closes it,
        // and none of them reach whatever is underneath.
        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.AllButtons
            onClicked: viewer.close(false)
            onWheel: wheel => wheel.accepted = true
        }
    }

    Item {
        id: frame
        objectName: "chatMediaViewerFrame"
        property bool animate: true
        x: viewer.expanded ? Math.round((viewer.width - viewer.fitWidth) / 2) : viewer.tileRect.x
        y: viewer.expanded ? Math.round(viewer.topRoom + (viewer.height - viewer.topRoom - viewer.bottomRoom
                                                          - viewer.fitHeight) / 2)
                           : viewer.tileRect.y
        width: viewer.expanded ? viewer.fitWidth : viewer.tileRect.width
        height: viewer.expanded ? viewer.fitHeight : viewer.tileRect.height

        Behavior on x {
            enabled: frame.animate
            NumberAnimation { duration: viewer.duration; easing.type: Easing.OutCubic }
        }
        Behavior on y {
            enabled: frame.animate
            NumberAnimation { duration: viewer.duration; easing.type: Easing.OutCubic }
        }
        Behavior on width {
            enabled: frame.animate
            NumberAnimation { duration: viewer.duration; easing.type: Easing.OutCubic }
        }
        Behavior on height {
            enabled: frame.animate
            NumberAnimation { duration: viewer.duration; easing.type: Easing.OutCubic }
        }

        // Clicks on the picture stay on the picture (a video's toggle it).
        MouseArea {
            anchors.fill: parent
            cursorShape: viewer.kind === 2 ? Qt.PointingHandCursor : Qt.ArrowCursor
            onClicked: viewer.togglePlay()
        }

        Rectangle {
            anchors.fill: parent
            radius: 6
            color: "#1b1f27"
            visible: viewer.kind === 2 || !picture.ready
        }
        // The photo: whole once it is here, its preview until then. Cropped
        // to the frame, whose shape is the photo's own once it is decoded, so
        // it grows out of the bubble's crop without a jump.
        ProfilePanelImage {
            id: preview
            anchors.fill: parent
            visible: viewer.kind === 1 && !picture.ready
            mediaKey: viewer.kind === 1 ? media.previewKey : ""
            crop: true
            radius: 6
        }
        ProfilePanelImage {
            id: picture
            objectName: "chatMediaViewerImage"
            anchors.fill: parent
            visible: viewer.kind === 1
            mediaKey: viewer.kind === 1 ? media.imageKey : ""
            crop: true
            radius: 6
        }
        // The video: its poster until the first frame is on screen.
        ProfilePanelImage {
            id: poster
            anchors.fill: parent
            visible: viewer.kind === 2 && !(clip.item && clip.item.hasPicture)
            mediaKey: viewer.kind === 2 ? media.previewKey : ""
            crop: true
            radius: 6
        }
        Loader {
            id: clip
            anchors.fill: parent
            active: viewer.kind === 2 && controls.active && media.segmentKeys.length > 0
            sourceComponent: ProfileClipPlayer {
                objectName: "chatMediaViewerPlayer"
                segmentKeys: media.segmentKeys
                longForm: true
                radius: 6
                playing: true
                onFinished: controls.active = false
            }
        }
        ProfileSpinner {
            anchors.centerIn: parent
            width: 24
            height: 24
            ink: "#ffffff"
            visible: viewer.kind === 2 && controls.active && !(clip.item && clip.item.hasPicture)
                     && viewer.expanded
        }
    }

    // The video's controls, under it: play or pause, where it is (a click or
    // a drag on the bar moves there) and the time.
    Item {
        id: controls
        objectName: "chatMediaViewerControls"
        // Playing or paused, as opposed to stopped at the start.
        property bool active: false
        readonly property var clipItem: clip.item
        readonly property bool playing: active && clipItem !== null && !clipItem.paused
        readonly property real length: clipItem && clipItem.durationMs > 0 ? clipItem.durationMs : viewer.durationMs
        readonly property real position: clipItem ? clipItem.positionMs : 0
        visible: viewer.kind === 2 && viewer.expanded
        x: Math.round(frame.x + (frame.width - width) / 2)
        y: Math.round(frame.y + frame.height + 12)
        width: Math.max(300, Math.min(frame.width, 520))
        height: 40

        Rectangle {
            anchors.fill: parent
            radius: 20
            color: "#b0101820"
            border.width: 1
            border.color: Theme.mediaChipBorder
        }
        Rectangle {
            id: playButton
            objectName: "chatMediaViewerPlay"
            x: 4
            y: 4
            width: 32
            height: 32
            radius: 16
            color: playMouse.containsMouse ? "#40ffffff" : "#26ffffff"
            border.width: 1
            border.color: "#80ffffff"
            Accessible.role: Accessible.Button
            Accessible.name: controls.playing ? "Pause" : "Play"
            Accessible.onPressAction: viewer.togglePlay()
            // Play is drawn optically centred already (see ProfileGlyph).
            ProfileGlyph {
                anchors.centerIn: parent
                width: 16
                height: 16
                kind: controls.playing ? "pause" : "play"
                ink: "#ffffff"
            }
            MouseArea {
                id: playMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: viewer.togglePlay()
            }
        }
        Text {
            id: timeText
            objectName: "chatMediaViewerTime"
            anchors.right: parent.right
            anchors.rightMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            text: viewer.clock(controls.position) + " / " + viewer.clock(controls.length)
            color: "#ffffff"
            font.family: Theme.uiFont
            font.pixelSize: 12
            renderType: Text.NativeRendering
        }
        Item {
            id: seekBar
            objectName: "chatMediaViewerSeek"
            x: playButton.x + playButton.width + 12
            width: timeText.x - x - 14
            height: parent.height
            readonly property real fraction: controls.length > 0 ? Math.min(1, controls.position / controls.length) : 0
            Accessible.role: Accessible.Slider
            Accessible.name: "Position"

            Rectangle {
                id: track
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width
                height: 4
                radius: 2
                color: "#40ffffff"
                Rectangle {
                    width: Math.max(parent.height, parent.width * seekBar.fraction)
                    height: parent.height
                    radius: parent.radius
                    color: "#ffffff"
                }
            }
            Rectangle {
                x: Math.round(track.width * seekBar.fraction - width / 2)
                anchors.verticalCenter: parent.verticalCenter
                width: 12
                height: 12
                radius: 6
                border.width: 1
                border.color: Theme.buttonBorder
                gradient: Gradient {
                    GradientStop { position: 0; color: Theme.switchKnobTop }
                    GradientStop { position: 1; color: Theme.switchKnobBottom }
                }
            }
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onPressed: mouse => viewer.seekTo(mouse.x / seekBar.width)
                onPositionChanged: mouse => {
                    if (pressed)
                        viewer.seekTo(mouse.x / seekBar.width);
                }
            }
        }
    }

    function togglePlay() {
        if (viewer.kind !== 2)
            return;
        if (!controls.active) {
            controls.active = true;
            return;
        }
        if (clip.item)
            clip.item.paused = !clip.item.paused;
    }
    function seekTo(fraction) {
        if (viewer.kind !== 2 || controls.length <= 0)
            return;
        const ms = Math.max(0, Math.min(1, fraction)) * controls.length;
        if (!controls.active)
            controls.active = true;
        if (clip.item)
            clip.item.seek(ms);
    }

    Text {
        id: captionText
        objectName: "chatMediaViewerCaption"
        visible: text.length > 0
        anchors.horizontalCenter: parent.horizontalCenter
        y: viewer.expanded ? frame.y + frame.height + (viewer.kind === 2 ? 64 : 14) : parent.height
        width: Math.min(parent.width - 80, 640)
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        maximumLineCount: 3
        elide: Text.ElideRight
        textFormat: Text.PlainText
        text: viewer.caption
        color: "#f2f2f2"
        opacity: scrim.opacity
        font.family: Theme.uiFont
        font.pixelSize: 15
        renderType: Text.NativeRendering
    }

    // Save and Copy image for a photo, and the cross, in the top corner.
    Row {
        objectName: "chatMediaViewerButtons"
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 16
        spacing: 8
        opacity: scrim.opacity

        ViewerButton {
            objectName: "chatMediaViewerSave"
            visible: viewer.kind === 1
            glyph: "download"
            label: "Save"
            onClicked: viewer.saveRequested(viewer.stableId)
        }
        ViewerButton {
            id: copyButton
            objectName: "chatMediaViewerCopy"
            property bool justCopied: false
            visible: viewer.kind === 1
            glyph: "copy"
            label: justCopied ? "Copied" : "Copy image"
            onClicked: {
                if (viewer.controller && typeof viewer.controller.copyAttachmentImage === "function"
                        && viewer.controller.copyAttachmentImage(viewer.stableId)) {
                    copyButton.justCopied = true;
                    copiedReset.restart();
                }
            }
            Timer {
                id: copiedReset
                interval: 1500
                onTriggered: copyButton.justCopied = false
            }
        }
        ViewerButton {
            objectName: "chatMediaViewerClose"
            glyph: "cross"
            label: ""
            accessibleName: "Close"
            onClicked: viewer.close(false)
        }
    }

    // A glass pill: a glyph, and a word unless the glyph says it all.
    component ViewerButton: Rectangle {
        id: button
        property string glyph: ""
        property string label: ""
        property string accessibleName: label
        signal clicked

        // Whole pixels, so the drawn pill and the glyph inside it agree.
        width: label.length > 0 ? Math.ceil(caption.implicitWidth) + 44 : 32
        height: 32
        radius: 16
        color: buttonMouse.containsMouse ? Theme.mediaChipHover : Theme.mediaChip
        border.width: 1
        border.color: Theme.mediaChipBorder
        Accessible.role: Accessible.Button
        Accessible.name: button.accessibleName
        Accessible.onPressAction: button.clicked()

        ProfileGlyph {
            x: button.label.length > 0 ? 12 : (button.width - width) / 2
            anchors.verticalCenter: parent.verticalCenter
            width: 14
            height: 14
            kind: button.glyph
            ink: Theme.mediaChipGlyph
        }
        Text {
            id: caption
            x: 32
            anchors.verticalCenter: parent.verticalCenter
            visible: button.label.length > 0
            text: button.label
            color: Theme.mediaChipGlyph
            font.family: Theme.uiFont
            font.pixelSize: 12
            renderType: Text.NativeRendering
        }
        MouseArea {
            id: buttonMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: button.clicked()
        }
    }
}
