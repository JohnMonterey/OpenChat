import QtQuick
import QtQuick.Dialogs
import OpenChat
import OpenChat.Native

// Background (SPEC §14.6, `final-editor-narrow.png`): the backdrop's style
// (Solid · Gradient · Pattern · Picture), its two colours, the pattern
// gallery (None and the 17 motifs, each tile painted live in the page's own
// base and ink), the pattern's colour, size and strength, and the picture:
// chosen from a file, re-encoded here (at most 1920 px and 224 KB) with its
// progress shown, then placed (Tile · Fill · Fit · Center), kept still or
// scrolled with the page, or removed. The readability notice follows when a
// background change made the renderer correct something.
Item {
    id: tab
    objectName: "profileBackgroundTab"

    property var profiles: null
    property Item editor: null

    readonly property var draft: profiles ? profiles.draft : null
    readonly property var render: draft ? draft.render : null
    readonly property int kind: draft ? draft.backgroundKind : Profile.SolidBackground
    readonly property bool pattern: kind === Profile.PatternBackground
    readonly property bool picture: kind === Profile.ImageBackground
    // The gallery's order: the motifs by family, as the mockup lists them.
    readonly property var motifOrder: [Profile.Stars, Profile.Hearts, Profile.BrokenHearts, Profile.Skulls,
                                       Profile.Sparkles, Profile.MusicNotes, Profile.Flowers, Profile.Checkerboard,
                                       Profile.Zebra, Profile.Leopard, Profile.PolkaDots, Profile.Pinstripes,
                                       Profile.Plaid, Profile.Halftone, Profile.CyberGrid, Profile.Bubbles,
                                       Profile.LinenWeave]
    // -1 is None.
    readonly property var tiles: [-1].concat(motifOrder)
    readonly property string motifName: {
        if (!pattern || !profiles)
            return "None";
        const found = profiles.motifChoices.find(choice => choice.id === draft.motif);
        return found ? found.name : "";
    }
    readonly property int halfWidth: Math.floor((width - 32 - 8) / 2)

    function chooseStyle(index) {
        if (index === Profile.ImageBackground && !draft.hasBackgroundImage) {
            fileDialog.open(); // Picture needs a picture first
            return;
        }
        draft.backgroundKind = index;
    }
    function chooseTile(motif) {
        if (motif < 0) {
            // None: the pattern goes and the colours stay, as a gradient.
            if (pattern)
                draft.backgroundKind = Profile.GradientBackground;
            return;
        }
        profiles.beginGesture("background:pattern");
        draft.motif = motif;
        draft.backgroundKind = Profile.PatternBackground;
        profiles.endGesture();
    }
    function focusField(field) {
        style.forceActiveFocus(Qt.OtherFocusReason);
    }

    implicitHeight: column.y + column.implicitHeight + 16

    FileDialog {
        id: fileDialog
        objectName: "profileBackgroundFileDialog"
        title: "Choose a picture for your page"
        nameFilters: ["Pictures (*.jpg *.jpeg *.png *.bmp *.gif *.webp)"]
        onAccepted: tab.profiles.importBackground(selectedFile)
    }

    // A slider drag, or a run of arrow presses, is one undo step: the
    // gesture closes once the knob has rested for 400 ms.
    Timer {
        id: sliderRest
        interval: 400
        onTriggered: tab.profiles.endGesture()
    }

    Column {
        id: column
        x: 16
        y: 14
        width: tab.width - 32

        Text {
            text: "Background"
            color: Theme.categoryText
            font.family: Theme.uiFont
            font.pixelSize: 17
            renderType: Text.NativeRendering
            Accessible.role: Accessible.Heading
            Accessible.name: text
        }
        Item { width: 1; height: 12 }
        ProfileSegmented {
            id: style
            objectName: "profileBackgroundStyle"
            width: parent.width
            accessibleName: "Background style"
            options: ["Solid", "Gradient", "Pattern", "Picture"]
            fontPixelSize: 12
            currentIndex: tab.kind
            onActivated: index => tab.chooseStyle(index)
        }
        Item { width: 1; height: 14 }
        Row {
            spacing: 8
            ProfileColorWell {
                objectName: "profileBaseColourWell"
                width: tab.halfWidth
                label: "Base colour"
                color: tab.draft ? tab.draft.backgroundColor1 : "white"
                editor: tab.editor
                page: tab.draft
                onPicked: colour => tab.draft.backgroundColor1 = colour
            }
            ProfileColorWell {
                objectName: "profileFadeColourWell"
                width: tab.halfWidth
                label: "Fades to"
                enabled: tab.kind !== Profile.SolidBackground
                color: tab.draft ? tab.draft.backgroundColor2 : "white"
                editor: tab.editor
                page: tab.draft
                onPicked: colour => tab.draft.backgroundColor2 = colour
            }
        }
        Item { width: 1; height: 16 }
        Row {
            spacing: 6
            Text {
                text: "Pattern"
                color: Theme.textSecondaryStrong
                font.family: Theme.uiFont
                font.pixelSize: 12
                renderType: Text.NativeRendering
            }
            Text {
                objectName: "profilePatternName"
                text: "·  " + tab.motifName
                color: Theme.textPrimary
                font.family: Theme.uiFont
                font.pixelSize: 12
                renderType: Text.NativeRendering
            }
        }
        Item { width: 1; height: 6 }
        ProfileTileGrid {
            id: patterns
            objectName: "profilePatternGrid"
            accessibleName: "Pattern"
            readonly property int tileWidth: Math.floor((tab.width - 32 - 5 * 5) / 6)
            model: tab.tiles
            columns: 6
            columnSpacing: 5
            rowSpacing: 5
            ringRadius: 4
            currentIndex: tab.pattern && tab.draft ? tab.tiles.indexOf(tab.draft.motif) : 0
            onActivated: index => tab.chooseTile(tab.tiles[index])

            delegate: Rectangle {
                id: patternTile
                required property var modelData
                required property int index
                readonly property int motif: modelData
                readonly property bool chosen: index === patterns.currentIndex
                objectName: motif < 0 ? "profilePatternTile_none" : "profilePatternTile_" + motif
                width: patterns.tileWidth
                height: 34
                radius: 4
                clip: true
                color: tab.render ? tab.render.color1 : "white"
                Accessible.role: Accessible.RadioButton
                Accessible.name: motif < 0 ? "No pattern"
                                 : tab.profiles.motifChoices.find(choice => choice.id === motif).name
                Accessible.checkable: true
                Accessible.checked: chosen
                Accessible.onPressAction: patterns.activate(index)

                ProfileBackdrop {
                    anchors.fill: parent
                    anchors.margins: 1
                    kind: patternTile.motif < 0 ? Profile.GradientBackground : Profile.PatternBackground
                    color1: tab.render ? tab.render.color1 : "white"
                    color2: tab.render ? tab.render.color2 : "white"
                    motif: Math.max(0, patternTile.motif)
                    motifScale: Profile.SmallMotif
                    motifInk: tab.render ? tab.render.motifInk : "black"
                    motifOpacity: Math.max(0.45, tab.render ? tab.render.motifOpacity : 0.45)
                    previewScale: 0.5
                    aurora: false
                }
                // None: a single stroke across the tile.
                Rectangle {
                    visible: patternTile.motif < 0
                    anchors.centerIn: parent
                    width: Math.sqrt(parent.width * parent.width + parent.height * parent.height) - 10
                    height: 1.5
                    rotation: -Math.atan2(parent.height, parent.width) * 180 / Math.PI
                    color: tab.render && tab.render.boxDark ? "#b0ffffff" : "#90ffffff"
                }
                Rectangle {
                    anchors.fill: parent
                    radius: 4
                    color: "transparent"
                    border.width: patternTile.chosen ? 2 : 1
                    border.color: patternTile.chosen ? Theme.focusBorder : Theme.inputBorder
                }
                Rectangle {
                    visible: patternTile.chosen
                    anchors.fill: parent
                    anchors.margins: 2
                    radius: 2
                    color: "transparent"
                    border.width: 1
                    border.color: "#b3ffffff"
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: patterns.activate(patternTile.index)
                }
            }
        }

        // The pattern's own knobs, while there is a pattern.
        Column {
            visible: tab.pattern
            width: parent.width
            Item { width: 1; height: 16 }
            Row {
                spacing: 8
                ProfileColorWell {
                    objectName: "profilePatternColourWell"
                    width: tab.halfWidth
                    label: "Pattern colour"
                    color: tab.draft ? tab.draft.motifInk : "black"
                    editor: tab.editor
                    page: tab.draft
                    onPicked: colour => tab.draft.motifInk = colour
                }
                Column {
                    Text {
                        text: "Size"
                        color: Theme.textSecondaryStrong
                        font.family: Theme.uiFont
                        font.pixelSize: 12
                        renderType: Text.NativeRendering
                    }
                    Item { width: 1; height: 8 }
                    ProfileSegmented {
                        objectName: "profilePatternSize"
                        width: tab.halfWidth
                        height: 30
                        accessibleName: "Pattern size"
                        options: ["S", "M", "L"]
                        currentIndex: tab.draft ? tab.draft.motifScale : 1
                        onActivated: index => tab.draft.motifScale = index
                    }
                }
            }
            Item { width: 1; height: 14 }
            Item {
                width: parent.width
                height: 20
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Strength"
                    color: Theme.textSecondaryStrong
                    font.family: Theme.uiFont
                    font.pixelSize: 12
                    renderType: Text.NativeRendering
                }
                Text {
                    objectName: "profilePatternStrengthValue"
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    text: (tab.draft ? tab.draft.motifOpacity : 0) + "%"
                    color: Theme.textPrimary
                    font.family: Theme.uiFont
                    font.pixelSize: 12
                    renderType: Text.NativeRendering
                }
            }
            AeroSlider {
                objectName: "profilePatternStrength"
                width: parent.width
                accessibleName: "Pattern strength"
                value: tab.draft ? (tab.draft.motifOpacity - 10) / 70 : 0
                onMoved: position => {
                    if (!sliderRest.running)
                        tab.profiles.beginGesture("background:strength");
                    sliderRest.restart();
                    tab.draft.motifOpacity = Math.round(10 + position * 70);
                }
            }
        }

        Item { width: 1; height: 14 }
        Rectangle { width: parent.width; height: 1; color: Theme.softRule }
        Item { width: 1; height: 12 }

        // The picture.
        Text {
            text: "Picture"
            color: Theme.textSecondaryStrong
            font.family: Theme.uiFont
            font.pixelSize: 12
            renderType: Text.NativeRendering
        }
        Item { width: 1; height: 6 }
        Rectangle {
            objectName: "profilePictureCard"
            width: parent.width
            height: pictureColumn.implicitHeight + 20
            radius: 5
            color: Theme.panelBackground
            border.width: 1
            border.color: Theme.inputBorder
            Rectangle { x: 5; y: 1; width: parent.width - 10; height: 1; color: Theme.glossStrong }

            Column {
                id: pictureColumn
                x: 10
                y: 10
                width: parent.width - 20
                spacing: 8

                Row {
                    visible: tab.draft !== null && tab.draft.hasBackgroundImage && !tab.profiles.backgroundImporting
                    spacing: 10
                    Rectangle {
                        width: 96
                        height: 60
                        radius: 3
                        color: tab.render ? tab.render.color1 : "white"
                        border.width: 1
                        border.color: Theme.inputBorder
                        clip: true
                        ProfileImageLayer {
                            objectName: "profilePictureThumbnail"
                            anchors.fill: parent
                            anchors.margins: 1
                            imageKey: tab.draft ? tab.draft.backgroundImageKey : ""
                            imageMode: Profile.FitImage
                            scrollOffset: 0
                        }
                    }
                    Column {
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 6
                        Text {
                            width: pictureColumn.width - 106
                            wrapMode: Text.Wrap
                            text: tab.picture ? "On your page" : "Saved with your page; choose Picture above to show it"
                            color: Theme.textSecondaryStrong
                            font.family: Theme.uiFont
                            font.pixelSize: 12
                            renderType: Text.NativeRendering
                        }
                        Text {
                            objectName: "profileRemovePicture"
                            text: "Remove picture"
                            color: Theme.errorText
                            font.family: Theme.uiFont
                            font.pixelSize: 13
                            renderType: Text.NativeRendering
                            activeFocusOnTab: true
                            Accessible.role: Accessible.Button
                            Accessible.name: text
                            Accessible.onPressAction: tab.profiles.removeBackgroundImage()
                            Keys.onReturnPressed: tab.profiles.removeBackgroundImage()
                            Keys.onSpacePressed: tab.profiles.removeBackgroundImage()
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
                                onClicked: tab.profiles.removeBackgroundImage()
                            }
                        }
                    }
                }

                // Re-encoding: the importer's progress.
                Column {
                    visible: tab.profiles !== null && tab.profiles.backgroundImporting
                    width: parent.width
                    spacing: 6
                    Text {
                        text: "Preparing your picture…"
                        color: Theme.textPrimary
                        font.family: Theme.uiFont
                        font.pixelSize: 13
                        renderType: Text.NativeRendering
                    }
                    Rectangle {
                        objectName: "profilePictureProgress"
                        width: parent.width
                        height: 8
                        radius: 4
                        color: Theme.fieldBackground
                        border.width: 1
                        border.color: Theme.inputBorder
                        Accessible.role: Accessible.ProgressBar
                        Accessible.name: "Preparing your picture"
                        Rectangle {
                            x: 1
                            y: 1
                            height: 6
                            radius: 3
                            width: (parent.width - 2) * Math.max(0, Math.min(1, tab.profiles ? tab.profiles.backgroundImportProgress : 0))
                            gradient: Gradient {
                                GradientStop { position: 0; color: Theme.switchTop }
                                GradientStop { position: 1; color: Theme.switchBottom }
                            }
                        }
                    }
                }

                Text {
                    visible: tab.draft !== null && !tab.draft.hasBackgroundImage && !tab.profiles.backgroundImporting
                    width: parent.width
                    wrapMode: Text.Wrap
                    text: "A photo or artwork of your own. It's re-encoded to at most 1920 px and 224 KB."
                    color: Theme.textSecondaryStrong
                    font.family: Theme.uiFont
                    font.pixelSize: 12
                    renderType: Text.NativeRendering
                }
                ProfileEditorRail.Button {
                    objectName: "profileChoosePictureButton"
                    visible: tab.profiles !== null && !tab.profiles.backgroundImporting
                    width: parent.width
                    height: 30
                    glyph: "image"
                    label: tab.draft && tab.draft.hasBackgroundImage ? "Choose a different picture…" : "Choose picture…"
                    fontPixelSize: 13
                    onClicked: fileDialog.open()
                }
            }
        }

        // How the picture sits on the page, while it is the background.
        Column {
            visible: tab.picture
            width: parent.width
            Item { width: 1; height: 10 }
            ProfileSegmented {
                objectName: "profilePictureMode"
                width: parent.width
                accessibleName: "Picture placement"
                options: ["Tile", "Fill", "Fit", "Center"]
                fontPixelSize: 12
                currentIndex: tab.draft ? tab.draft.imageMode : Profile.FillImage
                onActivated: index => tab.draft.imageMode = index
            }
            Item { width: 1; height: 10 }
            Item {
                width: parent.width
                height: 30
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - 58
                    wrapMode: Text.Wrap
                    text: "Stays put while the page scrolls"
                    color: Theme.textPrimary
                    font.family: Theme.uiFont
                    font.pixelSize: 13
                    renderType: Text.NativeRendering
                }
                AeroSwitch {
                    objectName: "profilePictureFixed"
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    width: 46
                    height: 26
                    accessibleName: "Stays put while the page scrolls"
                    checked: tab.draft !== null && tab.draft.imageFixed
                    onToggled: checked => tab.draft.imageFixed = checked
                }
            }
        }

        Item { width: 1; height: 12 }
        ProfileReadabilityNotice {
            width: parent.width
            page: tab.draft
            editorTab: Profile.BackgroundTab
            onShowMe: role => tab.editor.showMe(role)
        }
    }
}
