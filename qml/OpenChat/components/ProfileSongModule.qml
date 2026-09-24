import QtQuick
import QtQuick.Shapes
import QtQuick.Window
import OpenChat
import OpenChat.Native

// The profile song (SPEC §5.4): MySpace's embedded mini player as an app-owned
// object, never themed and never in a box. Silver glass over light boxes,
// smoked glass over dark ones. It drives the page's one SongPlayer, which it
// never starts by itself: play is always the viewer's press.
//
// States: idle ▶ with the length; playing ❚❚ with fill, knob and five
// decorative equaliser bars; paused keeps the fill; loading (the song is still
// arriving) greys the orb and says "Arriving…"; unavailable (no decoder, no
// output) says so at full strength instead of the groove.
Item {
    id: song
    objectName: "profileSongModule"
    property var view: null
    readonly property var page: view ? view.page : null
    readonly property var player: view ? view.songPlayer : null
    readonly property bool smoked: view && view.render ? view.render.songMaterial === 1 : false
    readonly property bool loading: page !== null && page.songPending
    readonly property bool unavailable: !loading && (!player || !player.valid || player.error.length > 0)
    readonly property bool playing: !loading && !unavailable && player.playing
    readonly property bool paused: !loading && !unavailable && !player.playing && player.positionMs > 0
    readonly property string state_: loading ? "loading" : unavailable ? "unavailable" : playing ? "playing"
                                                                                                : paused ? "paused" : "idle"
    readonly property bool canPlay: !loading && !unavailable
    readonly property real durationMs: player && player.valid ? player.durationMs : (page ? page.songDurationMs : 0)
    readonly property real progress: canPlay && durationMs > 0 ? Math.min(1, player.positionMs / durationMs) : 0
    readonly property int seconds: Math.round(durationMs / 1000)
    readonly property string title: page ? page.songTitle : ""
    readonly property string artist: page ? page.songArtist : ""

    readonly property color glassTop: smoked ? "#3a4d5e" : "#fdfeff"
    readonly property color glassMid: smoked ? "#26343f" : "#eaf0f5"
    readonly property color glassBottom: smoked ? "#1b252e" : "#dbe4ec"
    readonly property color edge: smoked ? "#56687a" : "#a9b8c6"
    readonly property color ink: smoked ? "#eef4f9" : "#1f3246"
    readonly property color secondaryInk: smoked ? "#b8c9d8" : "#4f6278"

    function clock(ms) {
        const s = Math.max(0, Math.round(ms / 1000));
        return Math.floor(s / 60) + ":" + (s % 60 < 10 ? "0" : "") + (s % 60);
    }
    function toggle() {
        if (song.canPlay && song.player)
            song.player.toggle();
    }

    implicitHeight: 57

    // The orb is the Tab stop: Space or Enter plays or pauses, ←/→ seek 5 s.
    Accessible.role: Accessible.Grouping
    Accessible.name: "Profile song"

    // A 1 px shadow line under the bottom edge.
    Rectangle {
        x: 1
        y: 56
        width: parent.width - 2
        height: 1
        color: song.smoked ? "#50000000" : "#1f1b3a58"
    }
    Rectangle {
        width: parent.width
        height: 56
        radius: 5
        border.width: 1
        border.color: song.edge
        gradient: Gradient {
            GradientStop { position: 0; color: song.glassTop }
            GradientStop { position: 0.48; color: song.glassMid }
            GradientStop { position: 0.5; color: song.glassBottom }
            GradientStop { position: 1; color: song.glassMid }
        }
        Rectangle {
            x: 6
            y: 1
            width: parent.width - 12
            height: 1
            color: song.smoked ? "#40ffffff" : "#ffffff"
        }
    }

    // The Windows 7 media orb.
    Item {
        id: orb
        objectName: "profileSongOrb"
        x: 8
        y: 8
        width: 40
        height: 40
        activeFocusOnTab: true
        readonly property bool hovered: orbArea.containsMouse && song.canPlay

        Accessible.role: Accessible.Button
        Accessible.name: "Profile song: " + (song.title.length > 0 ? song.title : "untitled")
                         + (song.artist.length > 0 ? " by " + song.artist : "") + ", " + song.seconds + " seconds. "
                         + (song.playing ? "Pause" : "Play")
        Accessible.onPressAction: song.toggle()
        Keys.onPressed: event => {
            if (event.key === Qt.Key_Space || event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                song.toggle();
                event.accepted = true;
            } else if ((event.key === Qt.Key_Left || event.key === Qt.Key_Right) && song.canPlay && song.player) {
                song.player.seekBy(event.key === Qt.Key_Left ? -5000 : 5000);
                event.accepted = true;
            }
        }

        Rectangle {
            anchors.fill: parent
            anchors.margins: -2
            radius: width / 2
            color: orb.hovered ? "#5588c8f0" : "transparent"
        }
        Rectangle {
            anchors.fill: parent
            radius: width / 2
            border.width: 1
            border.color: song.canPlay ? (song.smoked ? "#8fc9ef" : "#2f6d9c") : song.edge
            gradient: Gradient {
                GradientStop {
                    position: 0
                    color: song.canPlay ? (orb.hovered ? "#a8d8f8" : "#8cc6ee") : (song.smoked ? "#3b4a57" : "#e6ebf0")
                }
                GradientStop {
                    position: 0.5
                    color: song.canPlay ? (orb.hovered ? "#4f9ad2" : "#3f86c0") : (song.smoked ? "#2b3842" : "#cfd8e0")
                }
                GradientStop {
                    position: 1
                    color: song.canPlay ? "#1f5f94" : (song.smoked ? "#222c34" : "#bcc7d1")
                }
            }
        }
        Rectangle {
            x: 6
            y: 3
            width: parent.width - 12
            height: parent.height * 0.44
            radius: height / 2
            gradient: Gradient {
                GradientStop { position: 0; color: "#b0ffffff" }
                GradientStop { position: 1; color: "#10ffffff" }
            }
        }
        // The play triangle or the pause bars, with a 1 px shadow.
        Repeater {
            model: 2
            Shape {
                required property int index
                x: 0
                y: index === 0 ? 1 : 0
                width: 40
                height: 40
                ShapePath {
                    strokeColor: "transparent"
                    fillColor: index === 0 ? "#40002040"
                                           : song.canPlay ? "#ffffff" : (song.smoked ? "#7d8fa1" : "#8796a5")
                    PathSvg {
                        path: song.playing ? "M 14 12 L 18.5 12 L 18.5 28 L 14 28 Z M 21.5 12 L 26 12 L 26 28 L 21.5 28 Z"
                                           : "M 16 11.5 L 29 20 L 16 28.5 Z"
                    }
                }
            }
        }
        ProfileFocusRing {
            anchors.fill: parent
            anchors.margins: -2
            shown: orb.activeFocus
            radius: width / 2
            onDark: song.smoked
        }
        MouseArea {
            id: orbArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: song.canPlay ? Qt.PointingHandCursor : Qt.ArrowCursor
            onClicked: {
                orb.forceActiveFocus();
                song.toggle();
            }
        }
    }

    ProfileGlyph {
        id: note
        x: orb.x + orb.width + 11
        y: 9
        width: 14
        height: 14
        kind: "note"
        ink: song.secondaryInk
    }
    // "Title by Artist": the title bold, both elided, both plain text.
    Row {
        id: line
        x: note.x + 19
        y: 7
        width: song.width - x - (eq.visible ? eq.width + 16 : 12)
        spacing: 4
        Text {
            id: titleText
            objectName: "profileSongTitle"
            width: Math.min(implicitWidth, line.width)
            elide: Text.ElideRight
            text: song.title.length > 0 ? song.title : "Untitled"
            textFormat: Text.PlainText
            color: song.ink
            font.family: Theme.uiFont
            font.pixelSize: 13
            font.bold: true
            renderType: Text.NativeRendering
        }
        Text {
            objectName: "profileSongArtist"
            visible: song.artist.length > 0 && titleText.width < line.width
            width: Math.max(0, Math.min(implicitWidth, line.width - titleText.width - line.spacing))
            elide: Text.ElideRight
            text: "by " + song.artist
            textFormat: Text.PlainText
            color: song.secondaryInk
            font.family: Theme.uiFont
            font.pixelSize: 13
            renderType: Text.NativeRendering
        }
    }

    // Decorative, tied to the playing state (not a level meter), at 10 fps.
    ProfileTickerClient {
        id: eqClock
        fps: 10
        active: eq.visible && song.view !== null && song.view.animate && ProfileRenderPolicy.animationsAllowed
                && song.Window.window !== null && song.Window.window.visibility !== Window.Hidden
                && song.Window.window.visibility !== Window.Minimized
    }
    Row {
        id: eq
        objectName: "profileSongEqualiser"
        visible: song.playing
        anchors.right: parent.right
        anchors.rightMargin: 12
        y: 10
        height: 12
        spacing: 2
        Repeater {
            model: [0.5, 0.9, 0.65, 1.0, 0.4]
            Rectangle {
                required property real modelData
                required property int index
                // Each bar steps through its own short cycle; frame 0 is the still frame.
                readonly property real level: eqClock.frame === 0 ? modelData
                    : 0.3 + 0.7 * Math.abs(Math.sin((eqClock.frame + index * 3) * (0.9 + index * 0.17)))
                anchors.bottom: parent.bottom
                width: 3
                height: Math.max(2, Math.round(12 * level))
                radius: 1
                gradient: Gradient {
                    GradientStop { position: 0; color: "#9be146" }
                    GradientStop { position: 1; color: "#45ad0b" }
                }
            }
        }
    }

    // The sunken groove: fill and knob while playing or paused, a ghost
    // fill while the song arrives.
    Item {
        id: groove
        objectName: "profileSongGroove"
        visible: !song.unavailable
        x: note.x
        y: 33
        width: song.width - x - timeText.implicitWidth - 22
        height: 10
        Rectangle {
            anchors.fill: parent
            radius: 4
            color: song.smoked ? "#0e151b" : "#ffffff"
            border.width: 1
            border.color: song.smoked ? "#4a5c6c" : "#b3c1ce"
        }
        Rectangle {
            x: 3
            y: 1
            width: parent.width - 6
            height: 1
            color: song.smoked ? "#50000000" : "#24526878"
        }
        Rectangle {
            visible: song.progress > 0 || song.loading
            x: 1
            y: 1
            height: parent.height - 2
            width: song.loading ? (parent.width - 2) * 0.35 : Math.max(8, (parent.width - 2) * song.progress)
            radius: 3
            opacity: song.loading ? 0.45 : 1
            gradient: Gradient {
                GradientStop { position: 0; color: "#8cc6ee" }
                GradientStop { position: 1; color: "#2f78b4" }
            }
            Rectangle {
                x: 2
                y: 1
                width: parent.width - 4
                height: 2
                radius: 1
                color: "#70ffffff"
            }
        }
        Rectangle {
            visible: song.playing || song.paused
            x: 1 + (parent.width - 2) * song.progress - width / 2
            anchors.verticalCenter: parent.verticalCenter
            width: 12
            height: 12
            radius: 6
            border.color: song.smoked ? "#8fc9ef" : "#6f93b3"
            gradient: Gradient {
                GradientStop { position: 0; color: "#ffffff" }
                GradientStop { position: 1; color: "#d6e2ec" }
            }
        }
        MouseArea {
            anchors.fill: parent
            anchors.margins: -4
            enabled: song.canPlay && song.durationMs > 0
            cursorShape: Qt.PointingHandCursor
            function seekTo(x) {
                song.player.seek(Math.round(Math.max(0, Math.min(1, (x - 5) / (groove.width - 2))) * song.durationMs));
            }
            onPressed: mouse => seekTo(mouse.x)
            onPositionChanged: mouse => { if (pressed) seekTo(mouse.x); }
        }
    }
    Text {
        objectName: "profileSongUnavailable"
        visible: song.unavailable
        x: note.x
        y: 31
        width: song.width - x - 12
        elide: Text.ElideRight
        text: "Can't play this song on this computer."
        textFormat: Text.PlainText
        color: song.secondaryInk
        font.family: Theme.uiFont
        font.pixelSize: 12
        renderType: Text.NativeRendering
    }
    Text {
        id: timeText
        objectName: "profileSongTime"
        visible: !song.unavailable
        anchors.right: parent.right
        anchors.rightMargin: 11
        anchors.verticalCenter: groove.verticalCenter
        text: song.loading ? "Arriving…"
              : song.state_ === "idle" ? song.clock(song.durationMs)
              : song.clock(song.player.positionMs) + " / " + song.clock(song.durationMs)
        textFormat: Text.PlainText
        color: song.secondaryInk
        font.family: Theme.uiFont
        font.pixelSize: 11
        renderType: Text.NativeRendering
    }
}
