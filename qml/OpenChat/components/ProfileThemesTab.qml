import QtQuick
import OpenChat
import OpenChat.Native

// Themes (SPEC §14.5, `final-editor-themes.png`): the ten presets as live
// miniatures carrying the owner's own name (ProfilePresetThumb), two to a row.
// Resting on a tile for 250 ms, by pointer or keyboard, tries the preset on in
// the preview; moving away puts the draft back. A click, Enter or Space
// applies it (one undo step). The chosen preset wears a check orb and an
// amber "edited" tag once any knob differs from it; Aero Sky is tagged
// "default". The footer resets the style to the preset or picks a surprise.
Item {
    id: tab
    objectName: "profileThemesTab"

    property var profiles: null
    property Item editor: null

    readonly property var draft: profiles ? profiles.draft : null
    readonly property string ownerName: draft && draft.displayName.length > 0 ? draft.displayName
                                        : profiles ? profiles.personName : ""
    readonly property int tileWidth: Math.floor((width - 32 - 10) / 2)
    readonly property int thumbHeight: Math.round(tileWidth * 0.62)
    // The preset the pointer or the keyboard rests on, tried on after 250 ms.
    property int restingPreset: -1

    function rest(preset) {
        restingPreset = preset;
        tryOnTimer.restart();
    }
    function leave(preset) {
        if (restingPreset !== preset)
            return;
        restingPreset = -1;
        tryOnTimer.stop();
        if (profiles)
            profiles.setTryOnPreset(-1);
    }
    function apply(preset) {
        restingPreset = -1;
        tryOnTimer.stop();
        profiles.applyPreset(preset);
    }
    function focusField(field) {
        grid.forceActiveFocus(Qt.OtherFocusReason);
    }

    implicitHeight: column.y + column.implicitHeight + 16
    Component.onDestruction: {
        if (profiles && restingPreset >= 0)
            profiles.setTryOnPreset(-1);
    }

    Timer {
        id: tryOnTimer
        interval: 250
        onTriggered: {
            if (tab.profiles && tab.restingPreset >= 0)
                tab.profiles.setTryOnPreset(tab.restingPreset);
        }
    }

    property Component footer: Component {
        Rectangle {
            implicitHeight: 52
            color: Theme.composerBackground
            Rectangle { width: parent.width; height: 1; color: Theme.rule }
            Row {
                x: 16
                anchors.verticalCenter: parent.verticalCenter
                spacing: 8
                ProfileEditorRail.Button {
                    objectName: "profileResetToPresetButton"
                    width: 128
                    height: 32
                    label: "Reset to preset"
                    fontPixelSize: 13
                    enabled: tab.draft !== null && tab.draft.styleEditedSincePreset
                    onClicked: tab.profiles.resetToPreset()
                }
                ProfileEditorRail.Button {
                    objectName: "profileSurpriseButton"
                    width: 118
                    height: 32
                    glyph: "dice"
                    label: "Surprise me"
                    fontPixelSize: 13
                    onClicked: tab.profiles.surpriseMe()
                }
            }
        }
    }

    Column {
        id: column
        x: 16
        y: 14
        width: tab.width - 32

        Text {
            text: "Themes"
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
            text: "Start from a look, then change anything. Your words, photo and friends stay put."
            color: Theme.textSecondaryStrong
            font.family: Theme.uiFont
            font.pixelSize: 13
            lineHeight: 1.05
            renderType: Text.NativeRendering
        }
        Item { width: 1; height: 12 }

        ProfileTileGrid {
            id: grid
            objectName: "profileThemesGrid"
            accessibleName: "Themes"
            model: tab.profiles ? tab.profiles.presets : []
            columns: 2
            columnSpacing: 10
            rowSpacing: 10
            ringRadius: 7
            currentIndex: {
                const presets = tab.profiles ? tab.profiles.presets : [];
                for (let i = 0; i < presets.length; ++i) {
                    if (tab.draft && presets[i].id === tab.draft.preset)
                        return i;
                }
                return -1;
            }
            onActivated: index => tab.apply(model[index].id)
            onHighlighted: index => tab.rest(model[index].id)
            onActiveFocusChanged: {
                if (!activeFocus && tab.restingPreset >= 0 && !anyTileHovered())
                    tab.leave(tab.restingPreset);
            }
            function anyTileHovered() {
                for (let i = 0; i < count; ++i) {
                    if (itemAt(i) && itemAt(i).hovered)
                        return true;
                }
                return false;
            }

            delegate: Item {
                id: tile
                required property var modelData
                required property int index
                readonly property bool chosen: tab.draft !== null && modelData.id === tab.draft.preset
                readonly property bool hovered: tileMouse.containsMouse
                readonly property bool edited: chosen && tab.draft.styleEditedSincePreset
                objectName: "profileThemeTile_" + modelData.slug
                width: tab.tileWidth
                height: tab.thumbHeight + 26
                Accessible.role: Accessible.RadioButton
                Accessible.name: modelData.name + (edited ? ", edited" : "")
                                 + (modelData.id === Profile.AeroSkyPreset ? ", default" : "")
                Accessible.checkable: true
                Accessible.checked: chosen
                Accessible.onPressAction: tab.apply(modelData.id)

                Rectangle {
                    visible: tile.chosen || tile.hovered
                    x: -4
                    y: -4
                    width: parent.width + 8
                    height: parent.height + 6
                    radius: 7
                    color: tile.chosen ? Theme.navSelected : Theme.buttonHover
                    border.width: tile.chosen ? 2 : 1
                    border.color: tile.chosen ? Theme.focusBorder : Theme.buttonBorder
                }
                ProfilePresetThumb {
                    id: thumb
                    width: parent.width
                    height: tab.thumbHeight
                    preset: tile.modelData.id
                    ownerName: tab.ownerName
                    dark: Theme.darkMode
                }
                Rectangle {
                    width: thumb.width
                    height: thumb.height
                    radius: 5
                    color: "transparent"
                    border.width: 1
                    border.color: Theme.inputBorder
                }
                // The chosen preset's check orb (CosmeticPicker's).
                Rectangle {
                    visible: tile.chosen
                    x: parent.width - 14
                    y: -6
                    width: 20
                    height: 20
                    radius: 10
                    border.width: 1
                    border.color: Theme.accentOrbRim
                    gradient: Gradient {
                        GradientStop { position: 0; color: Theme.switchTop }
                        GradientStop { position: 1; color: Theme.switchBottom }
                    }
                    ProfileEditorRail.Glyph {
                        anchors.centerIn: parent
                        width: 12
                        height: 12
                        kind: "check"
                        ink: "#ffffff"
                    }
                }
                Row {
                    y: thumb.height + 5
                    spacing: 6
                    Text {
                        text: tile.modelData.name
                        color: Theme.textPrimary
                        font.family: Theme.uiFont
                        font.pixelSize: 13
                        renderType: Text.NativeRendering
                    }
                    Rectangle {
                        objectName: "profileThemeTag"
                        readonly property string tag: tile.edited ? "edited"
                                                      : tile.modelData.id === Profile.AeroSkyPreset ? "default" : ""
                        visible: tag.length > 0
                        anchors.verticalCenter: parent.verticalCenter
                        width: tagText.implicitWidth + 10
                        height: 16
                        radius: 8
                        color: tile.edited ? Theme.warningBackground : Theme.panelBackground
                        border.width: 1
                        border.color: tile.edited ? Theme.warningBorder : Theme.inputBorder
                        Text {
                            id: tagText
                            objectName: "profileThemeTagText"
                            anchors.centerIn: parent
                            text: parent.tag
                            color: tile.edited ? Theme.warningText : Theme.textSecondaryStrong
                            font.family: Theme.uiFont
                            font.pixelSize: 10
                            renderType: Text.NativeRendering
                        }
                    }
                }
                MouseArea {
                    id: tileMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onContainsMouseChanged: {
                        if (containsMouse)
                            tab.rest(tile.modelData.id);
                        else
                            tab.leave(tile.modelData.id);
                    }
                    onClicked: {
                        grid.focusIndex = tile.index;
                        tab.apply(tile.modelData.id);
                    }
                }
            }
        }
    }
}
