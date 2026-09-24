import QtQuick
import QtQuick.Dialogs
import OpenChat
import OpenChat.Native

// Song (SPEC §14.11, `final-editor-song.png`): the file the song comes from
// ("WAV · 4:26 · 44.1 kHz stereo"), which 45 seconds of it (a waveform with a
// draggable window; the controller re-encodes once it rests), Listen (the
// page's own player on the draft's song: the cut window with its half-second
// fades, never played by itself), the title and artist, how much of the
// 224 KB the clip uses, and Remove song. WAV imports everywhere, other
// formats where this computer's decoder reads them.
Item {
    id: tab
    objectName: "profileSongTab"

    property var profiles: null
    property Item editor: null

    readonly property var draft: profiles ? profiles.draft : null
    readonly property var source: profiles ? profiles.songSource : ({})
    readonly property bool hasSource: source.fileName !== undefined && source.fileName.length > 0
    readonly property bool importing: profiles !== null && profiles.songImporting
    readonly property var player: editor ? editor.songPlayer : null
    readonly property bool playerHoldsDraft: player !== null && draft !== null && draft.hasSong
                                             && player.songKey === draft.songKey
    readonly property real budget: profiles ? profiles.limits.songBytes : 229376
    readonly property string editingTarget: "song"

    function clock(ms) {
        const seconds = Math.round(ms / 1000);
        return Math.floor(seconds / 60) + ":" + ("0" + seconds % 60).slice(-2);
    }
    function kilobytes(bytes) {
        return Math.round(bytes / 1024) + " KB";
    }
    function focusField(field) {
        chooseButton.forceActiveFocus(Qt.OtherFocusReason);
    }

    implicitHeight: column.y + column.implicitHeight + 16

    FileDialog {
        id: fileDialog
        objectName: "profileSongFileDialog"
        title: "Choose a song"
        nameFilters: ["Songs (*.wav *.mp3 *.m4a *.aac *.ogg *.oga *.opus *.flac)", "All files (*)"]
        onAccepted: tab.profiles.importSong(selectedFile)
    }

    Column {
        id: column
        x: 16
        y: 14
        width: tab.width - 32

        Text {
            text: "Profile song"
            color: Theme.categoryText
            font.family: Theme.uiFont
            font.pixelSize: 17
            renderType: Text.NativeRendering
            Accessible.role: Accessible.Heading
            Accessible.name: text
        }
        Item { width: 1; height: 4 }
        Text {
            width: parent.width
            wrapMode: Text.Wrap
            text: "Up to 45 seconds. It only plays when someone presses play."
            color: Theme.textSecondaryStrong
            font.family: Theme.uiFont
            font.pixelSize: 13
            renderType: Text.NativeRendering
        }
        Item { width: 1; height: 12 }

        // The file card: the file (or the song on the page), or a note of
        // what can be imported.
        Rectangle {
            objectName: "profileSongFileCard"
            visible: tab.hasSource || (tab.draft !== null && tab.draft.hasSong) || tab.importing
            width: parent.width
            height: 52
            radius: 5
            color: Theme.panelBackground
            border.width: 1
            border.color: Theme.inputBorder
            Rectangle { x: 5; y: 1; width: parent.width - 10; height: 1; color: Theme.glossStrong }
            Rectangle {
                x: 10
                anchors.verticalCenter: parent.verticalCenter
                width: 32
                height: 32
                radius: 4
                gradient: Gradient {
                    GradientStop { position: 0; color: Theme.switchTop }
                    GradientStop { position: 1; color: Theme.switchBottom }
                }
                ProfileEditorRail.Glyph {
                    anchors.centerIn: parent
                    width: 18
                    height: 18
                    kind: "note"
                    ink: "#ffffff"
                }
            }
            Column {
                x: 52
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - 62
                spacing: 1
                Text {
                    objectName: "profileSongFileName"
                    width: parent.width
                    elide: Text.ElideMiddle
                    text: tab.hasSource ? tab.source.fileName
                          : tab.importing ? "Reading the file…"
                          : tab.draft && tab.draft.songTitle.length > 0 ? tab.draft.songTitle : "Your song"
                    textFormat: Text.PlainText
                    color: Theme.textPrimary
                    font.family: Theme.uiFont
                    font.pixelSize: 13
                    renderType: Text.NativeRendering
                }
                Text {
                    objectName: "profileSongFileFormat"
                    width: parent.width
                    elide: Text.ElideRight
                    text: tab.hasSource ? tab.source.formatLabel
                          : tab.importing ? "This can take a moment for long files."
                          : tab.draft ? "On your page · " + tab.clock(tab.draft.songDurationMs) : ""
                    color: Theme.textSecondaryStrong
                    font.family: Theme.uiFont
                    font.pixelSize: 11
                    renderType: Text.NativeRendering
                }
            }
        }
        Text {
            visible: !tab.hasSource && !tab.importing && (tab.draft === null || !tab.draft.hasSong)
            width: parent.width
            wrapMode: Text.Wrap
            text: "WAV files work on every computer; MP3 and other formats wherever this computer can play them."
            color: Theme.textSecondaryStrong
            font.family: Theme.uiFont
            font.pixelSize: 12
            renderType: Text.NativeRendering
        }
        Item { width: 1; height: 8 }
        ProfileEditorRail.Button {
            id: chooseButton
            objectName: "profileChooseSongButton"
            width: parent.width
            height: 32
            glyph: tab.hasSource || (tab.draft && tab.draft.hasSong) ? "" : "note"
            label: tab.hasSource || (tab.draft && tab.draft.hasSong) ? "Choose a different file…" : "Choose a song file…"
            fontPixelSize: 13
            onClicked: fileDialog.open()
        }

        // Which 45 seconds, while the file it came from is known.
        Column {
            visible: tab.hasSource
            width: parent.width
            Item { width: 1; height: 16 }
            Item {
                width: parent.width
                height: 18
                Text {
                    text: "Which part"
                    color: Theme.textSecondaryStrong
                    font.family: Theme.uiFont
                    font.pixelSize: 12
                    renderType: Text.NativeRendering
                }
                Text {
                    objectName: "profileSongWindowText"
                    anchors.right: parent.right
                    text: tab.profiles ? tab.clock(tab.profiles.songWindowStartMs) + " – "
                                         + tab.clock(tab.profiles.songWindowStartMs + tab.profiles.songWindowMs) : ""
                    color: Theme.textPrimary
                    font.family: Theme.uiFont
                    font.pixelSize: 12
                    font.bold: true
                    renderType: Text.NativeRendering
                }
            }
            Item { width: 1; height: 6 }
            ProfileWaveform {
                id: waveform
                objectName: "profileSongWaveform"
                width: parent.width
                height: 64
                peaks: tab.profiles ? tab.profiles.songPeaks : []
                durationMs: tab.source.durationMs || 0
                windowStartMs: tab.profiles ? tab.profiles.songWindowStartMs : 0
                windowMs: tab.profiles ? tab.profiles.songWindowMs : 0
                interactive: tab.source.available === true
                onWindowMoved: startMs => tab.profiles.setSongWindow(startMs)
            }
            Item { width: 1; height: 4 }
            Item {
                width: parent.width
                height: 14
                Text {
                    text: "0:00"
                    color: Theme.textSecondaryStrong
                    font.family: Theme.uiFont
                    font.pixelSize: 10
                    renderType: Text.NativeRendering
                }
                Text {
                    anchors.right: parent.right
                    text: tab.clock(tab.source.durationMs || 0)
                    color: Theme.textSecondaryStrong
                    font.family: Theme.uiFont
                    font.pixelSize: 10
                    renderType: Text.NativeRendering
                }
            }
            Text {
                visible: tab.source.available === false
                width: parent.width
                topPadding: 4
                wrapMode: Text.Wrap
                text: "The file this song came from has moved, so the part can't change. Choose the file again to pick another part."
                color: Theme.textSecondaryStrong
                font.family: Theme.uiFont
                font.pixelSize: 12
                renderType: Text.NativeRendering
            }
        }

        Column {
            visible: tab.draft !== null && (tab.draft.hasSong || tab.importing)
            width: parent.width
            Item { width: 1; height: 8 }
            Row {
                spacing: 8
                ProfileEditorRail.Button {
                    id: listen
                    objectName: "profileSongListenButton"
                    width: 110
                    height: 30
                    glyph: tab.player && tab.player.playing && tab.playerHoldsDraft ? "pause" : "play"
                    glyphSize: 13
                    fontPixelSize: 13
                    label: tab.importing ? "Preparing…"
                           : tab.player && tab.player.playing && tab.playerHoldsDraft ? "Pause" : "Listen"
                    enabled: !tab.importing && tab.playerHoldsDraft && tab.player.valid
                    onClicked: tab.player.toggle()
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    width: column.width - 118
                    wrapMode: Text.Wrap
                    text: "Fades in and out over half a second."
                    color: Theme.textSecondaryStrong
                    font.family: Theme.uiFont
                    font.pixelSize: 11
                    renderType: Text.NativeRendering
                }
            }
        }

        Column {
            visible: tab.draft !== null && tab.draft.hasSong
            width: parent.width
            spacing: 4
            Item { width: 1; height: 10 }
            ProfileFieldLabel {
                width: parent.width
                text: "Title"
                count: tab.draft ? tab.draft.songTitle.length : 0
                limit: tab.profiles ? tab.profiles.limits.songTitle : 60
            }
            ProfileTextArea {
                objectName: "profileField_songTitle"
                width: parent.width
                multiLine: false
                maximumLength: tab.profiles ? tab.profiles.limits.songTitle : 60
                value: tab.draft ? tab.draft.songTitle : ""
                accessibleName: "Song title"
                profiles: tab.profiles
                gestureKey: "field:songTitle"
                onEdited: text => tab.draft.songTitle = text
            }
            Item { width: 1; height: 4 }
            ProfileFieldLabel {
                width: parent.width
                text: "Artist"
                count: tab.draft ? tab.draft.songArtist.length : 0
                limit: tab.profiles ? tab.profiles.limits.songArtist : 60
            }
            ProfileTextArea {
                objectName: "profileField_songArtist"
                width: parent.width
                multiLine: false
                maximumLength: tab.profiles ? tab.profiles.limits.songArtist : 60
                value: tab.draft ? tab.draft.songArtist : ""
                accessibleName: "Artist"
                profiles: tab.profiles
                gestureKey: "field:songArtist"
                onEdited: text => tab.draft.songArtist = text
            }
            Item { width: 1; height: 10 }
            Item {
                width: parent.width
                height: 16
                Text {
                    text: "Clip size"
                    color: Theme.textSecondaryStrong
                    font.family: Theme.uiFont
                    font.pixelSize: 12
                    renderType: Text.NativeRendering
                }
                Text {
                    objectName: "profileSongClipSize"
                    anchors.right: parent.right
                    text: tab.kilobytes(tab.profiles ? tab.profiles.songClipBytes : 0) + " of " + tab.kilobytes(tab.budget)
                    color: Theme.textPrimary
                    font.family: Theme.uiFont
                    font.pixelSize: 12
                    renderType: Text.NativeRendering
                }
            }
            Rectangle {
                objectName: "profileSongClipMeter"
                width: parent.width
                height: 8
                radius: 4
                color: Theme.fieldBackground
                border.width: 1
                border.color: Theme.inputBorder
                Accessible.role: Accessible.ProgressBar
                Accessible.name: "Clip size"
                Rectangle {
                    x: 1
                    y: 1
                    height: 6
                    radius: 3
                    width: (parent.width - 2) * Math.min(1, (tab.profiles ? tab.profiles.songClipBytes : 0) / tab.budget)
                    gradient: Gradient {
                        GradientStop { position: 0; color: Theme.switchTop }
                        GradientStop { position: 1; color: Theme.switchBottom }
                    }
                }
            }
            Item { width: 1; height: 12 }
            Text {
                objectName: "profileRemoveSong"
                text: "Remove song"
                color: Theme.errorText
                font.family: Theme.uiFont
                font.pixelSize: 13
                renderType: Text.NativeRendering
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: text
                Accessible.onPressAction: tab.profiles.removeSong()
                Keys.onReturnPressed: tab.profiles.removeSong()
                Keys.onSpacePressed: tab.profiles.removeSong()
                Rectangle {
                    visible: parent.activeFocus
                    anchors.fill: parent
                    anchors.margins: -2
                    radius: 2
                    color: "transparent"
                    border.width: 2
                    border.color: Theme.focusBorder
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: tab.profiles.removeSong()
                }
            }
        }
    }
}
