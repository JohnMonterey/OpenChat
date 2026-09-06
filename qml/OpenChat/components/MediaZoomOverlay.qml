import QtQuick
import OpenChat
import OpenChat.Native

// One picture, enlarged: a camera or a shared screen grown from its tile to
// fill the window, over a darkened copy of everything else.
//
// The enlarged picture is a SECOND view of the same source, not the tile
// moved: it follows the tile's frame or canvas (in C++, through the view's
// `source`, so frames never pass through a binding), so a camera keeps playing
// and a share keeps filling in while it is up, and nothing about the call
// surface underneath has to be re-parented or re-laid-out to make room. The
// tile it came from is paused while the copy is open — it sits under the
// scrim, where repainting it would be work nobody could see.
//
// It grows from exactly where the tile is and shrinks back to exactly there,
// so the eye follows one picture rather than watching one vanish and another
// appear. While it is up nothing behind it takes a click: the scrim swallows
// them, and any click outside the picture closes it.
Item {
    id: zoom
    objectName: "mediaZoomOverlay"

    // The view this is a copy of. Set while open or closing.
    property CallVideoItem sourceItem: null
    // True from the moment the picture is asked to grow until it is asked to
    // shrink. The picture is on screen for a little longer than this, on its
    // way back to the tile.
    property bool expanded: false
    readonly property bool open: sourceItem !== null
    readonly property int duration: 220
    readonly property real margin: 28

    // The rectangle the picture grows into: as much of the window as its own
    // aspect allows, centred, with a little air around it.
    readonly property real aspect: sourceItem ? Math.max(0.2, sourceItem.sourceAspect) : 1
    readonly property real fitWidth: Math.max(80, Math.min(width - 2 * margin,
                                                           (height - 2 * margin) * aspect))
    readonly property real fitHeight: fitWidth / aspect

    // Where the tile is, in this overlay's coordinates: the picture starts and
    // ends there. Refreshed whenever the copy is opened or told to close, which
    // is the only time it is read.
    property rect tileRect: Qt.rect(0, 0, 0, 0)

    anchors.fill: parent
    visible: open
    enabled: open

    function refreshTileRect() {
        if (!sourceItem)
            return;
        const origin = sourceItem.mapToItem(zoom, 0, 0);
        tileRect = Qt.rect(origin.x, origin.y, sourceItem.width, sourceItem.height);
    }

    // Grow `item` (a CallVideoItem) to fill the window.
    function enlarge(item) {
        if (!item)
            return;
        if (sourceItem === item && expanded)
            return;
        if (sourceItem && sourceItem !== item)
            sourceItem.paused = false;
        settle.stop();
        sourceItem = item;
        refreshTileRect();
        // Place the picture on the tile first, without animating, so the
        // growth starts from there and not from wherever it was last.
        picture.animate = false;
        expanded = false;
        picture.animate = true;
        expanded = true;
        sourceItem.paused = true;
    }

    // Shrink back onto the tile and go away. `instant` skips the animation,
    // for when the tile itself is gone and there is nothing to shrink to.
    function dismiss(instant) {
        if (!sourceItem)
            return;
        sourceItem.paused = false;
        if (instant || !visible) {
            settle.stop();
            expanded = false;
            sourceItem = null;
            return;
        }
        refreshTileRect();
        expanded = false;
        settle.restart();
    }

    // Lets go of the source once the shrink has landed on the tile.
    Timer {
        id: settle
        interval: zoom.duration + 30
        onTriggered: {
            if (!zoom.expanded)
                zoom.sourceItem = null;
        }
    }

    // A tile whose camera turns off, or whose share stops, disappears; the
    // copy of it has nothing left to show and closes on the spot.
    Connections {
        target: zoom.sourceItem
        ignoreUnknownSignals: true
        function onVisibleChanged() {
            if (zoom.sourceItem && !zoom.sourceItem.visible)
                zoom.dismiss(true);
        }
    }

    // Escape closes it too, from wherever the keyboard focus happens to be.
    Shortcut {
        sequences: ["Escape"]
        enabled: zoom.expanded
        onActivated: zoom.dismiss(false)
    }

    Rectangle {
        id: scrim
        objectName: "mediaZoomScrim"
        anchors.fill: parent
        color: Theme.zoomScrim
        opacity: zoom.expanded ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: zoom.duration } }

        // Every click that is not on the picture closes it, and none of them
        // reach whatever is underneath.
        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            onClicked: zoom.dismiss(false)
        }
    }

    Rectangle {
        id: picture
        objectName: "mediaZoomPicture"
        property bool animate: true
        x: zoom.expanded ? Math.round((zoom.width - zoom.fitWidth) / 2) : zoom.tileRect.x
        y: zoom.expanded ? Math.round((zoom.height - zoom.fitHeight) / 2) : zoom.tileRect.y
        width: zoom.expanded ? zoom.fitWidth : zoom.tileRect.width
        height: zoom.expanded ? zoom.fitHeight : zoom.tileRect.height
        radius: 8
        color: Theme.callBackdropBottom
        border.width: 1
        border.color: Theme.idleRing

        Behavior on x {
            enabled: picture.animate
            NumberAnimation { duration: zoom.duration; easing.type: Easing.OutCubic }
        }
        Behavior on y {
            enabled: picture.animate
            NumberAnimation { duration: zoom.duration; easing.type: Easing.OutCubic }
        }
        Behavior on width {
            enabled: picture.animate
            NumberAnimation { duration: zoom.duration; easing.type: Easing.OutCubic }
        }
        Behavior on height {
            enabled: picture.animate
            NumberAnimation { duration: zoom.duration; easing.type: Easing.OutCubic }
        }

        // Clicks on the picture stay on the picture.
        MouseArea { anchors.fill: parent }

        // Shows nothing, and holds nothing, while there is no source.
        CallVideoItem {
            objectName: "mediaZoomVideo"
            anchors.fill: parent
            anchors.margins: 1
            source: zoom.sourceItem
        }
    }
}
