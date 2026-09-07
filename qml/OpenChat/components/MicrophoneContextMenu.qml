import QtQuick
import QtQml.Models
import QtQuick.Controls.Basic
import OpenChat
import OpenChat.Native

// Shares the settings page's persisted state and live processing connection.
AeroMenu {
    id: micMenu
    objectName: "microphoneContextMenu"
    readonly property var settings: MicrophoneSettings

    component SettingSlider: Item {
        id: row
        property string label
        property string displayValue
        property alias value: slider.value
        signal moved(real position)
        implicitWidth: 250
        implicitHeight: 64
        width: parent ? parent.width : implicitWidth
        Text {
            x: 10; y: 7
            text: row.label
            color: row.enabled ? Theme.textPrimary : Theme.textSecondary
            font.family: Theme.uiFont
            font.pixelSize: 13
        }
        Text {
            anchors.right: parent.right
            anchors.rightMargin: 10
            y: 7
            text: row.displayValue
            color: Theme.textSecondary
            font.family: Theme.uiFont
            font.pixelSize: 12
        }
        AeroSlider {
            id: slider
            objectName: row.objectName + "Slider"
            x: 10; y: 29
            width: parent.width - 20
            accessibleName: row.label
            opacity: row.enabled ? 1 : 0.45
            onMoved: position => row.moved(position)
        }
    }

    AeroMenu {
        id: inputMenu
        objectName: "microphoneInputMenu"
        title: "Input device"
        AeroMenuItem {
            text: "Used for the next call"
            enabled: false
        }
        Instantiator {
            model: [{ id: "", name: "System default" }].concat(micMenu.settings.inputDevices)
            delegate: AeroMenuItem {
                required property var modelData
                text: modelData.name
                checkable: true
                autoExclusive: true
                checked: micMenu.settings.inputDeviceId === modelData.id
                onTriggered: micMenu.settings.inputDeviceId = modelData.id
            }
            onObjectAdded: (index, object) => inputMenu.insertItem(index + 1, object)
            onObjectRemoved: (index, object) => inputMenu.removeItem(object)
        }
    }

    SettingSlider {
        objectName: "callMicrophoneGain"
        label: "Input volume"
        displayValue: Math.round(micMenu.settings.gain * 100) + "%"
        value: micMenu.settings.gain / 2
        onMoved: position => micMenu.settings.gain = position * 2
    }
    AeroMenu {
        id: enhancementMenu
        title: "Mic Enhancement"
        objectName: "micEnhancementMenu"
        AeroMenuSetting {
            settingsMenu: enhancementMenu
            objectName: "callStudioVoice"
            text: "Studio Voice"
            checkable: true
            checked: micMenu.settings.studioVoice
            onTriggered: micMenu.settings.studioVoice = checked
        }
        AeroMenuSetting {
            settingsMenu: enhancementMenu
            text: "Noise reduction"
            checkable: true
            checked: micMenu.settings.noiseReduction
            onTriggered: micMenu.settings.noiseReduction = checked
        }
        AeroMenuSetting {
            settingsMenu: enhancementMenu
            text: "Automatic gain"
            checkable: true
            checked: micMenu.settings.automaticGain
            onTriggered: micMenu.settings.automaticGain = checked
        }
        AeroMenuSetting {
            settingsMenu: enhancementMenu
            text: "Compressor"
            checkable: true
            checked: micMenu.settings.compressor
            onTriggered: micMenu.settings.compressor = checked
        }
        AeroMenuSetting {
            settingsMenu: enhancementMenu
            objectName: "callNoiseGate"
            text: "Noise gate"
            checkable: true
            checked: micMenu.settings.noiseGateEnabled
            onTriggered: micMenu.settings.noiseGateEnabled = checked
        }
        SettingSlider {
            objectName: "callNoiseGateThreshold"
            label: "Gate opens above"
            displayValue: Math.round(micMenu.settings.noiseGateThresholdDb) + " dB"
            enabled: micMenu.settings.noiseGateEnabled
            readonly property real span: micMenu.settings.maxThresholdDb - micMenu.settings.minThresholdDb
            value: (micMenu.settings.noiseGateThresholdDb - micMenu.settings.minThresholdDb) / span
            onMoved: position => micMenu.settings.noiseGateThresholdDb = micMenu.settings.minThresholdDb + position * span
        }
    }
    MenuSeparator {
        padding: 4
        contentItem: Rectangle { implicitHeight: 1; color: Theme.rule }
    }
    VoiceEffectsMenu {}
    SettingSlider {
        objectName: "callEffectIntensity"
        label: "Effect intensity"
        displayValue: Math.round(micMenu.settings.effectIntensity * 100) + "%"
        enabled: micMenu.settings.voiceEffect !== "none"
        value: micMenu.settings.effectIntensity
        onMoved: position => micMenu.settings.effectIntensity = position
    }
}
