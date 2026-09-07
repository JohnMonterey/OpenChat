import QtQuick
import QtQuick.Controls.Basic
import QtQml.Models
import OpenChat

Item {
    id: panel
    objectName: "customVocalFxPanel"
    implicitHeight: content.height + 32
    property bool renaming: false
    property bool confirmingDelete: false
    readonly property var preset: VocalFx.selectedPreset

    component Label: Text {
        color: Theme.textSecondary
        font.family: Theme.uiFont
        font.pixelSize: 12
        renderType: Text.NativeRendering
    }
    component Button: AeroButton {
        id: button
        height: 30
        fontPixelSize: 12
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: label
        Accessible.onPressAction: button.clicked()
        Keys.onReturnPressed: clicked()
        Keys.onSpacePressed: clicked()
        Rectangle {
            anchors.fill: parent
            radius: 4
            visible: button.activeFocus
            color: "transparent"
            border.color: Theme.focusBorder
        }
    }
    Connections {
        target: VocalFx
        function onSelectedPresetIdChanged() {
            panel.renaming = false;
            panel.confirmingDelete = false;
            picker.close();
        }
    }
    VocalFxPicker { id: picker }

    Column {
        id: content
        y: 16
        width: parent.width
        spacing: 12
        Label {
            text: "Presets"
            color: Theme.textPrimary
            font.pixelSize: 15
        }
        Label {
            width: parent.width
            text: "Build your voice, one slot at a time. Effects flow from top to bottom."
            wrapMode: Text.WordWrap
        }

        Rectangle {
            width: parent.width
            height: welcome.height + 24
            visible: !VocalFx.storage.scanPromptDismissed && !VocalFx.scanned
            radius: 5
            color: Theme.fieldAccessory
            border.color: Theme.inputBorder
            Column {
                id: welcome
                x: 12; y: 12
                width: parent.width - 24
                spacing: 8
                Label {
                    text: "Use your own plugins?"
                    color: Theme.textPrimary
                    font.pixelSize: 13
                }
                Label {
                    width: parent.width
                    text: "Scan for installed VST / VST3 plugins to add them to your effect library. Built-in FX are ready now."
                    wrapMode: Text.WordWrap
                }
                Row {
                    spacing: 8
                    Button { width: 154; label: "Scan system plugins"; onClicked: VocalFx.scanPlugins() }
                    Button { width: 84; label: "Not now"; onClicked: VocalFx.storage.scanPromptDismissed = true }
                }
            }
        }
        Label {
            width: parent.width
            visible: text.length > 0
            text: VocalFx.scanError || (VocalFx.backend ? VocalFx.backend.scanError : "")
            wrapMode: Text.WordWrap
        }
        Row {
            width: parent.width
            spacing: 8
            Button {
                id: presetSelector
                width: parent.width - 142
                enabled: VocalFx.presets.length > 0
                label: ""
                Label {
                    anchors.fill: parent
                    anchors.margins: 8
                    text: (panel.preset ? panel.preset.name : "No presets yet") + "  ▾"
                    color: Theme.textPrimary
                    elide: Text.ElideRight
                    verticalAlignment: Text.AlignVCenter
                }
                Accessible.name: panel.preset ? "Preset: " + panel.preset.name : "No presets yet"
                onClicked: presetMenu.popup(presetSelector, 0, height)
                AeroMenu {
                    id: presetMenu
                    width: Math.max(220, presetSelector.width)
                    Instantiator {
                        model: VocalFx.presets
                        delegate: AeroMenuItem {
                            required property var modelData
                            text: modelData.name
                            checkable: true
                            checked: VocalFx.selectedPresetId === modelData.id
                            onTriggered: VocalFx.selectPreset(modelData.id)
                        }
                        onObjectAdded: (index, object) => presetMenu.insertItem(index, object)
                        onObjectRemoved: (index, object) => presetMenu.removeItem(object)
                    }
                }
            }
            Button {
                width: 98
                label: "+ New preset"
                onClicked: VocalFx.createPreset()
            }
            Button {
                id: optionsButton
                width: 28
                label: "⋯"
                enabled: panel.preset !== null
                Accessible.name: "Preset options"
                onClicked: optionsMenu.popup(optionsButton, 0, height)
                AeroMenu {
                    id: optionsMenu
                    width: 180
                    AeroMenuItem {
                        text: "Rename"
                        onTriggered: {
                            nameField.text = panel.preset.name;
                            panel.renaming = true;
                            nameField.focusInput();
                        }
                    }
                    AeroMenuItem { text: "Duplicate"; onTriggered: VocalFx.duplicatePreset() }
                    MenuSeparator { contentItem: Rectangle { implicitHeight: 1; color: Theme.rule } }
                    AeroMenuItem { text: "Delete preset…"; onTriggered: panel.confirmingDelete = true }
                }
            }
        }
        Row {
            width: parent.width
            spacing: 8
            visible: panel.renaming
            AeroTextField {
                id: nameField
                width: parent.width - 132
                height: 30
                fontPixelSize: 13
                accessibleName: "Preset name"
                onAccepted: {
                    if (text.trim().length) { VocalFx.renamePreset(text); panel.renaming = false; }
                }
            }
            Button {
                width: 54
                label: "Save"
                enabled: nameField.text.trim().length > 0
                onClicked: { VocalFx.renamePreset(nameField.text); panel.renaming = false; }
            }
            Button { width: 62; label: "Cancel"; onClicked: panel.renaming = false }
        }
        Rectangle {
            visible: panel.confirmingDelete
            width: parent.width
            height: deletion.height + 24
            color: Theme.fieldAccessory
            border.color: Theme.inputBorder
            radius: 4
            Column {
                id: deletion
                x: 12; y: 12
                width: parent.width - 24
                spacing: 8
                Label {
                    width: parent.width
                    text: "Delete “" + (panel.preset ? panel.preset.name : "") + "”? This removes all ten slots."
                    wrapMode: Text.WordWrap
                    color: Theme.textPrimary
                }
                Row {
                    spacing: 8
                    Button { width: 104; label: "Delete preset"; onClicked: VocalFx.deletePreset() }
                    Button { width: 64; label: "Cancel"; onClicked: panel.confirmingDelete = false }
                }
            }
        }
        Rectangle {
            visible: !panel.preset
            width: parent.width
            height: 110
            radius: 5
            color: Theme.fieldBackground
            border.color: Theme.inputBorder
            Column {
                anchors.centerIn: parent
                width: parent.width - 32
                spacing: 8
                Label {
                    width: parent.width
                    text: "Your first vocal chain"
                    color: Theme.textPrimary
                    font.pixelSize: 14
                    horizontalAlignment: Text.AlignHCenter
                }
                Label {
                    width: parent.width
                    text: "Create a preset, then click a slot to add an effect.\nMix up to ten effects into your own sound."
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                }
            }
        }
        Rectangle {
            visible: panel.preset !== null
            width: parent.width
            height: rack.height + 16
            radius: 5
            color: Theme.fieldBackground
            border.color: Theme.inputBorder
            Column {
                id: rack
                x: 8; y: 8
                width: parent.width - 16
                spacing: 3
                Item {
                    width: parent.width
                    height: 25
                    Label { x: 8; text: "FX SLOTS"; font.pixelSize: 10; font.letterSpacing: 1 }
                    Label {
                        anchors.right: parent.right
                        anchors.rightMargin: 8
                        text: panel.preset ? panel.preset.slots.filter(s => s.effectId).length + " / 10 used" : ""
                        font.pixelSize: 11
                    }
                }
                Repeater {
                    model: panel.preset ? VocalFx.slotCount : 0
                    Rectangle {
                        id: slot
                        required property int index
                        readonly property var slotData: panel.preset ? panel.preset.slots[index] : VocalFx.emptySlot()
                        readonly property bool occupied: slotData.effectId.length > 0
                        width: rack.width
                        height: 58
                        radius: 3
                        border.color: Theme.inputBorder
                        gradient: Gradient {
                            GradientStop { position: 0; color: slot.occupied ? Theme.buttonTop : Theme.fieldAccessory }
                            GradientStop { position: 1; color: slot.occupied ? Theme.buttonBottom : Theme.fieldBackground }
                        }
                        Label {
                            x: 8; y: 10
                            text: (slot.index + 1).toString().padStart(2, "0")
                            font.pixelSize: 11
                        }
                        AbstractButton {
                            id: bypass
                            x: 10; y: 35
                            width: 16; height: 16
                            enabled: slot.occupied
                            checkable: true
                            checked: slot.slotData.enabled
                            Accessible.name: "Enable FX Slot " + (slot.index + 1)
                            onClicked: VocalFx.updateSlot(slot.index, {enabled: !slot.slotData.enabled})
                            background: Rectangle {
                                anchors.centerIn: parent
                                width: 10; height: 10; radius: 5
                                color: slot.occupied && slot.slotData.enabled ? Theme.speakingRing : Theme.idleRing
                                border.width: bypass.activeFocus ? 2 : 1
                                border.color: bypass.activeFocus ? Theme.focusBorder : Theme.inputBorder
                            }
                        }
                        AeroMenuItem {
                            id: slotName
                            x: 32; y: 2
                            width: parent.width - 68
                            height: 30
                            leftPadding: 6
                            rightPadding: 6
                            text: slot.occupied ? slot.slotData.name : "Select effect…"
                            opacity: slot.occupied && !slot.slotData.enabled ? 0.55 : 1
                            Accessible.name: "FX Slot " + (slot.index + 1) + ": " + text
                            onTriggered: picker.openFor(slotName, slot.index)
                        }
                        Label {
                            x: 38; y: 36
                            text: !slot.occupied ? "Empty slot" : !slot.slotData.enabled ? "Bypassed" : "Wet / dry"
                            font.pixelSize: 11
                        }
                        AeroSlider {
                            x: parent.width - 166; y: 30
                            width: 94
                            height: 26
                            visible: slot.occupied
                            enabled: slot.slotData.enabled
                            value: slot.slotData.mix
                            accessibleName: "FX Slot " + (slot.index + 1) + " wet mix"
                            onMoved: value => VocalFx.updateSlot(slot.index, {mix: value})
                        }
                        Label {
                            anchors.right: parent.right
                            anchors.rightMargin: 32
                            y: 36
                            visible: slot.occupied
                            text: Math.round(slot.slotData.mix * 100) + "%"
                            font.pixelSize: 11
                        }
                        AeroMenuItem {
                            id: slotOptions
                            anchors.right: parent.right
                            anchors.rightMargin: 2
                            y: 3
                            width: 28; height: 48
                            leftPadding: 7; rightPadding: 0
                            text: "⋮"
                            Accessible.name: "FX Slot " + (slot.index + 1) + " options"
                            onTriggered: slotMenu.popup(slotOptions, 0, slotOptions.height)
                            AeroMenu {
                                id: slotMenu
                                width: 190
                                AeroMenuItem {
                                    text: "Move up"
                                    enabled: slot.index > 0
                                    onTriggered: VocalFx.moveSlot(slot.index, -1)
                                }
                                AeroMenuItem {
                                    text: "Move down"
                                    enabled: slot.index < VocalFx.slotCount - 1
                                    onTriggered: VocalFx.moveSlot(slot.index, 1)
                                }
                                AeroMenuItem {
                                    text: "Clear slot"
                                    enabled: slot.occupied
                                    onTriggered: VocalFx.updateSlot(slot.index, VocalFx.emptySlot())
                                }
                            }
                        }
                    }
                }
            }
        }
        Label {
            visible: panel.preset !== null
            width: parent.width
            text: "Saved automatically · Choose this preset from your microphone’s Voice Effects menu."
            wrapMode: Text.WordWrap
        }
        Label {
            visible: panel.preset !== null && !VocalFx.backend
            width: parent.width
            text: "Design preview: chain playback is not available in this build."
            wrapMode: Text.WordWrap
        }
        Button {
            visible: VocalFx.storage.scanPromptDismissed || VocalFx.scanned
            width: 164
            enabled: !VocalFx.scanning
            label: VocalFx.scanning ? "Scanning…" : VocalFx.scanned ? "Rescan system plugins" : "Scan system plugins"
            onClicked: VocalFx.scanPlugins()
        }
    }
}
