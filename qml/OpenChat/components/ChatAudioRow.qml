import QtQuick
import QtQuick.Shapes
import OpenChat

// A sound in its bubble: a round Aero play button, the sound's loudness as
// bars (the played part in the accent, or the skin's ink) and where it is,
// "0:42 / 3:10". It plays in the chat's one player (MessageHistory's), so
// scrolling it away or a new message never stops it; a click on the bars
// plays from there. While it travels a ring round the button fills and the
// line under the bars says how far it has come.
Item {
    id: audio
    objectName: "chatAudioRow"
    required property MessageDelegate row
    readonly property var player: row.chatAudio
    readonly property bool complete: row.transferState === 1
    readonly property bool mine: player !== null && player.activeId === row.stableId
    readonly property bool playing: mine && player.playing
    readonly property bool loading: mine && player.loading
    readonly property real durationMs: mine && player.durationMs > 0 ? player.durationMs : row.durationMs
    readonly property real positionMs: mine ? Math.min(player.positionMs, durationMs) : 0
    readonly property real played: durationMs > 0 ? positionMs / durationMs : 0
    readonly property bool skinned: row.bubbleItem.skinned
    readonly property color ink: skinned ? row.bubbleItem.skinTextColor : Theme.waveformPlayed
    readonly property color idleInk: skinned ? row.bubbleItem.skinSecondaryTextColor : Theme.waveformIdle
    readonly property color secondaryInk: skinned ? row.bubbleItem.skinSecondaryTextColor : Theme.textSecondary

    function toggle() {
        if (audio.complete && audio.player !== null)
            audio.player.toggle(audio.row.stableId);
    }
    function clock(ms) {
        const seconds = Math.max(0, Math.round(ms / 1000));
        return Math.floor(seconds / 60) + ":" + ("0" + seconds % 60).slice(-2);
    }

    // The play button: the call controls' face, round.
    Item {
        id: button
        objectName: "chatAudioPlay"
        x: 0
        y: 3
        width: 34
        height: 34
        opacity: audio.complete ? 1 : 0.55
        Accessible.role: Accessible.Button
        Accessible.name: (audio.playing ? "Pause " : "Play ")
                         + (audio.row.fileName.length > 0 ? audio.row.fileName : "audio")
        Accessible.onPressAction: audio.toggle()

        Rectangle {
            anchors.fill: parent
            radius: width / 2
            antialiasing: true
            border.width: 1
            border.color: buttonMouse.containsMouse && audio.complete ? Theme.focusBorder : Theme.buttonBorder
            gradient: Gradient {
                GradientStop { position: 0; color: buttonMouse.pressed ? Theme.buttonBottom : Theme.buttonTop }
                GradientStop { position: 0.5; color: Theme.buttonMid }
                GradientStop { position: 1; color: buttonMouse.pressed ? Theme.buttonTop : Theme.buttonBottom }
            }
        }
        Rectangle {
            x: 4
            y: 2
            width: parent.width - 8
            height: parent.height * 0.46
            radius: height / 2
            antialiasing: true
            opacity: buttonMouse.containsMouse ? 0.42 : 0.28
            gradient: Gradient {
                GradientStop { position: 0; color: Theme.glossStrong }
                GradientStop { position: 1; color: "#00ffffff" }
            }
        }
        // Play is drawn optically centred already (see ProfileGlyph).
        ProfileGlyph {
            anchors.centerIn: parent
            width: 16
            height: 16
            kind: audio.playing ? "pause" : "play"
            ink: Theme.iconInk
        }
        // On its way: a ring round the button fills.
        Shape {
            visible: audio.row.transferState === 0
            x: -3
            y: -3
            width: parent.width + 6
            height: parent.height + 6
            ShapePath {
                fillColor: "transparent"
                strokeColor: audio.idleInk
                strokeWidth: 2
                PathAngleArc {
                    centerX: 20
                    centerY: 20
                    radiusX: 19
                    radiusY: 19
                    startAngle: 0
                    sweepAngle: 360
                }
            }
            ShapePath {
                fillColor: "transparent"
                strokeColor: audio.row.transferProgress > 0 ? audio.ink : "transparent"
                strokeWidth: 2
                capStyle: ShapePath.RoundCap
                PathAngleArc {
                    centerX: 20
                    centerY: 20
                    radiusX: 19
                    radiusY: 19
                    startAngle: -90
                    sweepAngle: 360 * Math.max(0, Math.min(1, audio.row.transferProgress))
                }
            }
        }
        MouseArea {
            id: buttonMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: audio.complete ? Qt.PointingHandCursor : Qt.ArrowCursor
            onClicked: audio.toggle()
        }
    }

    // The loudness, 2 px bars on a 3 px pitch, each the loudest peak it
    // covers; before the first peak arrives (or without any) a flat line.
    Item {
        id: wave
        objectName: "chatAudioWaveform"
        x: 44
        y: 2
        width: parent.width - x
        height: 22
        readonly property int count: Math.max(1, Math.floor((width + 1) / 3))
        readonly property var bins: {
            const peaks = audio.row.peaks || [];
            const result = [];
            for (let bar = 0; bar < count; ++bar) {
                if (peaks.length === 0) {
                    result.push(0);
                    continue;
                }
                const first = Math.floor(bar * peaks.length / count);
                const last = Math.max(first + 1, Math.floor((bar + 1) * peaks.length / count));
                let loudest = 0;
                for (let peak = first; peak < last && peak < peaks.length; ++peak)
                    loudest = Math.max(loudest, Number(peaks[peak]) || 0);
                result.push(Math.max(0, Math.min(255, loudest)));
            }
            return result;
        }
        Accessible.role: Accessible.Slider
        Accessible.name: "Position"

        Repeater {
            model: wave.bins
            Rectangle {
                required property int index
                required property var modelData
                x: index * 3
                width: 2
                height: Math.max(2, Math.round(wave.height * Number(modelData) / 255))
                y: Math.round((wave.height - height) / 2)
                radius: 1
                color: (index + 0.5) / wave.count <= audio.played && audio.mine ? audio.ink : audio.idleInk
            }
        }
        MouseArea {
            anchors.fill: parent
            anchors.topMargin: -2
            anchors.bottomMargin: -2
            enabled: audio.complete && audio.durationMs > 0
            cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
            onClicked: mouse => {
                if (audio.player !== null)
                    audio.player.seek(audio.row.stableId,
                                      Math.max(0, Math.min(1, mouse.x / wave.width)) * audio.durationMs);
            }
        }
    }

    // Where it is, or how the transfer stands.
    Text {
        id: meta
        objectName: "chatAudioTime"
        x: wave.x
        y: wave.y + wave.height + 2
        width: (cancel.visible ? cancel.x - 6 : parent.width) - x
        text: audio.complete || audio.row.transferText.length === 0
              ? audio.clock(audio.positionMs) + " / " + audio.clock(audio.durationMs)
                + (audio.row.fileName.length > 0 ? "  ·  " + audio.row.fileName : "")
              : audio.row.transferText
        textFormat: Text.PlainText
        elide: Text.ElideRight
        color: audio.row.transferState >= 2 && !audio.skinned ? Theme.errorText : audio.secondaryInk
        style: audio.skinned ? Text.Raised : Text.Normal
        styleColor: audio.skinned ? audio.row.bubbleItem.skinTextShadowColor : "transparent"
        font.family: Theme.uiFont
        font.pixelSize: 11
        renderType: Text.NativeRendering
    }
    ChatCancelCross {
        id: cancel
        anchors.right: parent.right
        anchors.verticalCenter: meta.verticalCenter
        visible: audio.row.canCancel
        ink: audio.secondaryInk
        onClicked: audio.row.cancelRequested()
    }
}
