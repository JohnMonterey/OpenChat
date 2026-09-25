import QtQuick
import QtQuick.Controls.Basic
import OpenChat
import OpenChat.Native

// What the composer's "+" offers: a photo, a video, a sound or any file, one
// row each with the kind's glossy chip and a line on what it takes. It wears
// AeroMenu's look (the rim, the gloss band along the top) over a soft shadow
// and opens above the button: it grows out of the button's corner while its
// rows rise into place one after another, and fades back quickly. Being a
// Menu, the arrows, Enter and Esc work and a screen reader hears a menu.
Menu {
    id: menu
    objectName: "attachmentMenu"
    // Video needs libvpx; without it the row stays, greyed, and says why.
    property bool videoSupported: true
    // The photo types this computer can read, as the Photo row names them.
    property string photoHint: "JPG, PNG, WebP"
    // A row was picked: 1 photo, 2 video, 3 audio, 4 file.
    signal picked(int kind)

    readonly property real motion: ProfileRenderPolicy.animationsAllowed ? 1 : 0

    popupType: Popup.Item
    width: 260
    padding: 5
    margins: 8
    overlap: 1
    transformOrigin: Popup.BottomLeft
    // A press on the "+" it hangs from is the button's to handle (it closes
    // the menu); a press anywhere else closes it on the spot.
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside | Popup.CloseOnPressOutsideParent
    title: "Attach"

    enter: Transition {
        ParallelAnimation {
            NumberAnimation {
                property: "opacity"
                from: 0
                to: 1
                duration: 170 * menu.motion
                easing.type: Easing.OutCubic
            }
            NumberAnimation {
                property: "scale"
                from: 0.92
                to: 1
                duration: 170 * menu.motion
                easing.type: Easing.OutCubic
            }
        }
    }
    exit: Transition {
        ParallelAnimation {
            NumberAnimation {
                property: "opacity"
                to: 0
                duration: 110 * menu.motion
                easing.type: Easing.InCubic
            }
            NumberAnimation {
                property: "scale"
                to: 0.96
                duration: 110 * menu.motion
                easing.type: Easing.InCubic
            }
        }
    }

    background: Item {
        implicitWidth: 260
        // Two soft layers under the card: a wide faint one and a close one.
        Rectangle {
            x: -1
            y: 1
            width: parent.width + 2
            height: parent.height + 3
            radius: 8
            color: Theme.menuShadow
            opacity: 0.55
        }
        Rectangle {
            y: 2
            width: parent.width
            height: parent.height
            radius: 7
            color: Theme.menuShadow
        }
        Rectangle {
            anchors.fill: parent
            radius: 6
            color: Theme.contentBackground
            border.color: Theme.inputBorder
            Rectangle {
                x: 1
                y: 1
                width: parent.width - 2
                height: 16
                radius: 5
                gradient: Gradient {
                    GradientStop { position: 0; color: Theme.glossStrong }
                    GradientStop { position: 1; color: "transparent" }
                }
            }
        }
    }

    contentItem: ListView {
        implicitHeight: contentHeight
        model: menu.contentModel
        currentIndex: menu.currentIndex
        interactive: false
        spacing: 1
    }

    KindRow {
        objectName: "attachPhoto"
        order: 0
        kind: 1
        text: "Photo"
        hint: menu.photoHint
        onTriggered: menu.picked(1)
    }
    KindRow {
        objectName: "attachVideo"
        order: 1
        kind: 2
        text: "Video"
        hint: menu.videoSupported ? "Up to 1 minute" : "Not available on this computer"
        enabled: menu.videoSupported
        onTriggered: menu.picked(2)
    }
    KindRow {
        objectName: "attachAudio"
        order: 2
        kind: 3
        text: "Audio"
        hint: "MP3, M4A, WAV… up to 5 minutes"
        onTriggered: menu.picked(3)
    }
    KindRow {
        objectName: "attachFile"
        order: 3
        kind: 4
        text: "File"
        hint: "Any file up to 16 MB"
        onTriggered: menu.picked(4)
    }

    // One pickable kind: its chip, its name and a hint, rising into place
    // `order` beats after the menu that holds it starts to open.
    component KindRow: MenuItem {
        id: row
        property int order: 0
        property int kind: 4
        property string hint: ""
        // 0 as the menu starts to open, 1 once this row has risen into place.
        property real reveal: 1

        implicitHeight: 46
        leftPadding: 8
        rightPadding: 10
        hoverEnabled: true
        Accessible.description: row.hint

        Connections {
            target: row.menu
            function onAboutToShow() {
                if (ProfileRenderPolicy.animationsAllowed) {
                    row.reveal = 0;
                    rise.restart();
                } else {
                    row.reveal = 1;
                }
            }
        }
        SequentialAnimation {
            id: rise
            PauseAnimation { duration: 40 + 25 * row.order }
            NumberAnimation {
                target: row
                property: "reveal"
                to: 1
                duration: 150
                easing.type: Easing.OutCubic
            }
        }

        contentItem: Item {
            implicitWidth: 220
            implicitHeight: 34
            opacity: row.reveal
            transform: Translate { y: (1 - row.reveal) * 6 }

            AttachmentChip {
                id: chip
                anchors.verticalCenter: parent.verticalCenter
                width: 30
                height: 30
                kind: row.kind
                dimmed: !row.enabled
            }
            Text {
                id: title
                x: chip.width + 11
                y: Math.round(parent.height / 2) - implicitHeight + 1
                width: parent.width - x
                text: row.text
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: row.enabled ? Theme.textPrimary : Theme.textSecondary
                font.family: Theme.uiFont
                font.pixelSize: 13
                renderType: Text.NativeRendering
            }
            Text {
                x: title.x
                y: title.y + title.implicitHeight
                width: title.width
                text: row.hint
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: Theme.textSecondary
                font.family: Theme.uiFont
                font.pixelSize: 11
                renderType: Text.NativeRendering
            }
        }
        background: Rectangle {
            radius: 4
            color: row.highlighted && row.enabled ? Theme.navSelected : "transparent"
            border.width: row.highlighted && row.enabled ? 1 : 0
            border.color: Theme.focusBorder
        }
    }
}
