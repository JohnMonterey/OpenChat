import QtQuick
import OpenChat.Native

// Every native profile type and the Profile enums, used the way the page and
// the editor use them. tst_profilerender loads this with every warning
// failing the test.
Item {
    id: root
    width: 420
    height: 300

    readonly property int bubblesMotif: Profile.Bubbles
    readonly property int glitterEffect: Profile.GlitterName
    readonly property int customPreset: Profile.CustomPreset
    readonly property bool animationsAllowed: ProfileRenderPolicy.animationsAllowed
    readonly property bool lowMemoryMode: ProfileRenderPolicy.lowMemoryMode
    readonly property bool tickerRunning: ProfileTicker.running
    readonly property int tickerFrame: ProfileTicker.frame

    ProfileBackdrop {
        objectName: "profileBackdrop"
        anchors.fill: parent
        kind: Profile.PatternBackground
        color1: "#c7e1f5"
        color2: "#f1f8fd"
        motif: Profile.Stars
        motifScale: Profile.SmallMotif
        motifInk: "#94c6ec"
        motifOpacity: 0.45
        aurora: false
        darkBase: false
        previewScale: 1
    }

    ProfileImageLayer {
        objectName: "profileImageLayer"
        anchors.fill: parent
        imageKey: ""
        imageMode: Profile.FitImage
        scrollOffset: 0
    }

    ProfileAmbient {
        objectName: "profileAmbient"
        anchors.fill: parent
        kind: Profile.FallingHearts
        outlineColor: "transparent"
        running: false
        seed: 7
    }

    ProfileNameText {
        id: name
        objectName: "profileNameText"
        x: 12 - glyphLeft
        y: 12 - glyphTop
        text: "Michael"
        flourish: Profile.StarFlourish
        fontFamily: ""
        basePixelSize: 28
        minPixelSize: -1
        color: "#1c3d63"
        color2: "#ffffff"
        effect: Profile.GlowName
        darkBox: false
        availableWidth: 220
        animate: false
        Accessible.role: Accessible.StaticText
        Accessible.name: name.accessibleName
    }

    ProfilePresetThumb {
        objectName: "profilePresetThumb"
        x: 12
        y: 120
        preset: Profile.SceneQueenPreset
        ownerName: "Daniel"
        dark: false
    }

    ProfileMoodFace {
        objectName: "profileMoodFace"
        x: 160
        y: 120
        mood: Profile.MoodHappy
    }

    ProfileTickerClient {
        objectName: "profileTickerClient"
        active: false
        fps: 10
    }

    SongPlayer {
        objectName: "profileSongPlayer"
        songKey: ""
        active: false
        suspended: false
    }
}
