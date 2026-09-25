import QtQuick
import OpenChat

// An attachment kind's glossy chip: a round bead in the kind's hue (the
// presence bead's gradient and rim, drawn from that one colour) holding the
// kind's glyph in white. The attach menu, the tray and the chat all use it,
// so a photo is the same blue wherever it shows.
Item {
    id: chip
    // 1 photo, 2 video, 3 audio, anything else a file.
    property int kind: 4
    // Greyed, for a kind this computer cannot send.
    property bool dimmed: false

    readonly property color hue: kind === 1 ? Theme.attachPhoto
        : kind === 2 ? Theme.attachVideo
        : kind === 3 ? Theme.attachAudio : Theme.attachFile
    readonly property string glyph: kind === 1 ? "image" : kind === 2 ? "film" : kind === 3 ? "music" : "file"

    implicitWidth: 30
    implicitHeight: 30
    opacity: dimmed ? 0.4 : 1
    Accessible.ignored: true

    Rectangle {
        anchors.fill: parent
        radius: width / 2
        antialiasing: true
        border.width: 1
        border.color: Qt.darker(chip.hue, 1.3)
        gradient: Gradient {
            GradientStop { position: 0; color: Qt.lighter(chip.hue, 1.3) }
            GradientStop { position: 0.52; color: chip.hue }
            GradientStop { position: 1; color: Qt.darker(chip.hue, 1.12) }
        }
    }
    Rectangle {
        x: Math.round(parent.width * 0.16)
        y: 2
        width: parent.width - 2 * x
        height: Math.round(parent.height * 0.42)
        radius: height / 2
        antialiasing: true
        gradient: Gradient {
            GradientStop { position: 0; color: "#a0ffffff" }
            GradientStop { position: 1; color: "#08ffffff" }
        }
    }
    // About 56 % of the bead, but always an even number of pixels less than
    // it: centring snaps to whole pixels, so a glyph of the other parity
    // would sit half a pixel up and to the left.
    ProfileGlyph {
        anchors.centerIn: parent
        width: parent.width - 2 * Math.round(parent.width * 0.22)
        height: width
        kind: chip.glyph
        ink: Theme.attachChipGlyph
    }
}
