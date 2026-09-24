import QtQuick
import OpenChat

// The Song tab's trimmer (SPEC §14.11): the file's loudness as bars
// (`peaks`, 0…255 each, spread over the whole file) in a sunken well, the
// chosen window tinted with the selection colour and its bars in focusBorder,
// and a knob-styled handle at each end. Dragging the window, or ←/→ (Shift:
// ten seconds), moves it; `windowMoved` reports each new start and the
// controller re-encodes once the handle rests.
Item {
    id: wave

    property var peaks: []
    property real durationMs: 0
    property real windowStartMs: 0
    property real windowMs: 0
    property bool interactive: true
    property string accessibleName: "Which part of the song"
    signal windowMoved(real startMs)

    readonly property real maxStartMs: Math.max(0, durationMs - windowMs)
    readonly property real innerWidth: width - 8
    readonly property real windowX: durationMs > 0 ? 4 + windowStartMs / durationMs * innerWidth : 4
    readonly property real windowWidth: durationMs > 0 ? Math.min(innerWidth, windowMs / durationMs * innerWidth) : 0

    function moveTo(startMs) {
        const clamped = Math.max(0, Math.min(maxStartMs, Math.round(startMs)));
        if (clamped !== Math.round(windowStartMs))
            windowMoved(clamped);
    }

    implicitWidth: 268
    implicitHeight: 64
    activeFocusOnTab: interactive
    opacity: interactive ? 1 : 0.7
    Accessible.role: Accessible.Slider
    Accessible.name: accessibleName
    Keys.onLeftPressed: event => moveTo(windowStartMs - (event.modifiers & Qt.ShiftModifier ? 10000 : 1000))
    Keys.onRightPressed: event => moveTo(windowStartMs + (event.modifiers & Qt.ShiftModifier ? 10000 : 1000))
    Keys.onPressed: event => {
        if (event.key === Qt.Key_Home) {
            moveTo(0);
            event.accepted = true;
        } else if (event.key === Qt.Key_End) {
            moveTo(maxStartMs);
            event.accepted = true;
        }
    }

    Rectangle {
        anchors.fill: parent
        radius: 5
        color: Theme.fieldBackground
        border.width: wave.activeFocus ? 2 : 1
        border.color: wave.activeFocus ? Theme.focusBorder : Theme.inputBorder
        Rectangle { x: 5; y: 1; width: parent.width - 10; height: 1; color: Theme.insetTop }
    }
    Rectangle {
        objectName: "profileWaveformWindow"
        x: wave.windowX
        y: 1
        width: wave.windowWidth
        height: parent.height - 2
        color: Theme.selectionBackground
        opacity: 0.55
    }

    // 2 px bars on a 3 px pitch; each shows the loudest peak it covers.
    readonly property var bins: {
        const count = Math.max(0, Math.floor(innerWidth / 3));
        const result = [];
        if (peaks.length === 0 || count === 0)
            return result;
        for (let bar = 0; bar < count; ++bar) {
            const first = Math.floor(bar * peaks.length / count);
            const last = Math.max(first + 1, Math.floor((bar + 1) * peaks.length / count));
            let loudest = 0;
            for (let peak = first; peak < last && peak < peaks.length; ++peak)
                loudest = Math.max(loudest, Number(peaks[peak]));
            result.push(loudest);
        }
        return result;
    }

    Repeater {
        model: wave.bins
        Rectangle {
            required property int index
            required property var modelData
            readonly property real centre: x + 1
            x: 4 + index * 3
            height: Math.max(2, (wave.height - 12) * Number(modelData) / 255)
            y: (wave.height - height) / 2
            width: 2
            color: centre >= wave.windowX && centre <= wave.windowX + wave.windowWidth
                   ? Theme.focusBorder : Theme.buttonBorder
        }
    }

    Repeater {
        model: 2
        Rectangle {
            required property int index
            x: (index === 0 ? wave.windowX : wave.windowX + wave.windowWidth) - 4
            y: -3
            width: 8
            height: wave.height + 6
            radius: 3
            border.width: 1
            border.color: Theme.focusBorder
            gradient: Gradient {
                GradientStop { position: 0; color: Theme.switchKnobTop }
                GradientStop { position: 1; color: Theme.switchKnobBottom }
            }
            Rectangle {
                anchors.centerIn: parent
                width: 2
                height: 14
                radius: 1
                color: Theme.focusBorder
                opacity: 0.6
            }
        }
    }

    MouseArea {
        id: drag
        anchors.fill: parent
        anchors.margins: -3
        enabled: wave.interactive && wave.durationMs > 0
        cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
        property real grabOffsetMs: 0
        function msAt(x) {
            return (x - 3 - 4) / wave.innerWidth * wave.durationMs;
        }
        onPressed: mouse => {
            const at = msAt(mouse.x);
            // Grabbing the window keeps the point under the pointer; a press
            // outside it centres the window there.
            const inside = at >= wave.windowStartMs && at <= wave.windowStartMs + wave.windowMs;
            grabOffsetMs = inside ? at - wave.windowStartMs : wave.windowMs / 2;
            wave.moveTo(at - grabOffsetMs);
        }
        onPositionChanged: mouse => {
            if (pressed)
                wave.moveTo(msAt(mouse.x) - grabOffsetMs);
        }
    }
}
