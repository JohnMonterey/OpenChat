import QtQuick
import OpenChat
import OpenChat.Native

// Audio & Video → Microphone: which device, how loud, and the noise gate that
// keeps the room out of the call. Every control writes straight to
// MicrophoneSettings, which persists it and hands it to the live call engine,
// so a change made mid-call is heard on the next frame.
//
// The level bar is driven by the same processor a call uses, so what it shows
// green is exactly what would be sent and what it shows grey is exactly what
// the gate would hold back. Testing is explicit — a button, never automatic —
// because opening the microphone lights the OS's "in use" indicator, and a
// settings page should not do that on its own.
Item {
    id: panel
    objectName: "microphoneSettingsPanel"
    readonly property var settings: MicrophoneSettings
    implicitHeight: column.height + 18

    // The bar and the threshold marker share one scale: -60 dBFS at the left
    // edge, full scale at the right.
    readonly property real meterFloorDb: -60
    function meterPosition(db) {
        return Math.max(0, Math.min(1, (db - panel.meterFloorDb) / -panel.meterFloorDb));
    }

    Component.onDestruction: panel.settings.stopTest()

    Column {
        id: column
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.topMargin: 12
        spacing: 14

        // --- Device -------------------------------------------------------
        Text {
            text: "Microphone"
            color: Theme.textPrimary
            font.family: Theme.uiFont
            font.pixelSize: 15
            renderType: Text.NativeRendering
        }

        Column {
            id: deviceList
            objectName: "microphoneDeviceList"
            width: parent.width
            spacing: 2
            readonly property var devices: panel.settings.inputDevices

            // "System default" is a row like the others, so following the OS
            // is a choice made the same way as choosing a device.
            Repeater {
                model: [{ id: "", name: "System default", isDefault: false }].concat(deviceList.devices)

                Rectangle {
                    id: deviceRow
                    required property int index
                    required property var modelData
                    objectName: "microphoneDevice_" + index
                    readonly property bool selected: panel.settings.inputDeviceId === modelData.id
                    width: deviceList.width
                    height: 32
                    radius: 4
                    color: selected ? Theme.navSelected
                                    : (rowMouse.containsMouse ? Theme.buttonMid : "transparent")
                    border.width: selected || rowMouse.containsMouse ? 1 : 0
                    border.color: selected ? Theme.focusBorder : Theme.buttonBorder
                    Accessible.role: Accessible.RadioButton
                    Accessible.name: modelData.name
                    Accessible.checkable: true
                    Accessible.checked: selected

                    function activate() { panel.settings.inputDeviceId = deviceRow.modelData.id; }

                    Row {
                        anchors.left: parent.left
                        anchors.leftMargin: 10
                        anchors.right: parent.right
                        anchors.rightMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 9

                        // A radio bead: filled with the switch blue when chosen.
                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 14; height: 14; radius: 7
                            color: Theme.fieldBackground
                            border.width: 1
                            border.color: deviceRow.selected ? Theme.focusBorder : Theme.inputBorder
                            Rectangle {
                                anchors.centerIn: parent
                                width: 6; height: 6; radius: 3
                                visible: deviceRow.selected
                                color: Theme.switchBottom
                            }
                        }

                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width - 14 - 9 - (defaultTag.visible ? defaultTag.width + 9 : 0)
                            text: deviceRow.modelData.name
                            elide: Text.ElideRight
                            color: Theme.textPrimary
                            font.family: Theme.uiFont
                            font.pixelSize: 13
                            renderType: Text.NativeRendering
                        }

                        Text {
                            id: defaultTag
                            anchors.verticalCenter: parent.verticalCenter
                            visible: deviceRow.modelData.isDefault === true
                            text: "default"
                            color: Theme.textSecondary
                            font.family: Theme.uiFont
                            font.pixelSize: 11
                            renderType: Text.NativeRendering
                        }
                    }

                    MouseArea {
                        id: rowMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: deviceRow.activate()
                    }
                }
            }

            Text {
                width: parent.width
                visible: deviceList.devices.length === 0
                text: "No microphone was found. Plug one in and it will appear here."
                wrapMode: Text.WordWrap
                color: Theme.textSecondary
                font.family: Theme.uiFont
                font.pixelSize: 12
                renderType: Text.NativeRendering
            }
        }

        // --- Volume -------------------------------------------------------
        Item {
            width: parent.width
            height: 30

            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text: "Input volume"
                color: Theme.textPrimary
                font.family: Theme.uiFont
                font.pixelSize: 14
                renderType: Text.NativeRendering
            }
            Text {
                id: gainLabel
                objectName: "microphoneGainLabel"
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                width: 46
                horizontalAlignment: Text.AlignRight
                text: Math.round(panel.settings.gain * 100) + "%"
                color: Theme.textSecondary
                font.family: Theme.uiFont
                font.pixelSize: 12
                renderType: Text.NativeRendering
            }
            AeroSlider {
                objectName: "microphoneGainSlider"
                anchors.right: gainLabel.left
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(220, parent.width * 0.45)
                accessibleName: "Input volume"
                // 0..1 across the slider is 0..200%; the midpoint is unity.
                value: panel.settings.gain / 2
                onMoved: position => panel.settings.gain = position * 2
            }
        }

        // --- Gate ---------------------------------------------------------
        Item {
            width: parent.width
            height: 44

            Text {
                anchors.left: parent.left
                anchors.top: parent.top
                text: "Noise gate"
                color: Theme.textPrimary
                font.family: Theme.uiFont
                font.pixelSize: 14
                renderType: Text.NativeRendering
            }
            Text {
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                width: parent.width - gateSwitch.width - 16
                text: "Send only when you are talking; keep background noise out of the call"
                elide: Text.ElideRight
                color: Theme.textSecondary
                font.family: Theme.uiFont
                font.pixelSize: 12
                renderType: Text.NativeRendering
            }
            AeroSwitch {
                id: gateSwitch
                objectName: "noiseGateSwitch"
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                Accessible.name: "Noise gate"
                checked: panel.settings.noiseGateEnabled
                onToggled: checked => panel.settings.noiseGateEnabled = checked
            }
        }

        Item {
            width: parent.width
            height: visible ? 30 : 0
            visible: panel.settings.noiseGateEnabled

            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text: "Gate opens above"
                color: Theme.textPrimary
                font.family: Theme.uiFont
                font.pixelSize: 14
                renderType: Text.NativeRendering
            }
            Text {
                id: thresholdLabel
                objectName: "noiseGateThresholdLabel"
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                width: 46
                horizontalAlignment: Text.AlignRight
                text: Math.round(panel.settings.noiseGateThresholdDb) + " dB"
                color: Theme.textSecondary
                font.family: Theme.uiFont
                font.pixelSize: 12
                renderType: Text.NativeRendering
            }
            AeroSlider {
                objectName: "noiseGateThresholdSlider"
                anchors.right: thresholdLabel.left
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(220, parent.width * 0.45)
                accessibleName: "Noise gate threshold"
                readonly property real span: panel.settings.maxThresholdDb - panel.settings.minThresholdDb
                value: (panel.settings.noiseGateThresholdDb - panel.settings.minThresholdDb) / span
                onMoved: position =>
                    panel.settings.noiseGateThresholdDb = panel.settings.minThresholdDb + position * span
            }
        }

        // --- Meter --------------------------------------------------------
        Item {
            width: parent.width
            height: 44

            AeroButton {
                id: testButton
                objectName: "microphoneTestButton"
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                width: 138
                height: 32
                fontPixelSize: 13
                label: panel.settings.testing ? "Stop test" : "Test microphone"
                onClicked: panel.settings.testing ? panel.settings.stopTest()
                                                  : panel.settings.startTest()
            }

            // The bar: the groove is the same well as the slider's; the fill
            // is green while the gate would let sound through and grey while
            // it would hold it back, so the gate can be tuned by eye.
            Rectangle {
                id: meter
                objectName: "microphoneLevelMeter"
                anchors.left: testButton.right
                anchors.leftMargin: 14
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                height: 14
                radius: 4
                color: Theme.fieldBackground
                border.width: 1
                border.color: Theme.inputBorder
                readonly property bool live: panel.settings.testing
                readonly property bool open: panel.settings.gateOpen

                Rectangle { x: 1; y: 1; width: parent.width - 2; height: 1; color: Theme.insetTop }

                Rectangle {
                    x: 1; y: 1
                    height: parent.height - 2
                    radius: 3
                    width: meter.live ? (parent.width - 2) * panel.meterPosition(panel.settings.levelDb) : 0
                    color: meter.open ? Theme.speakingRing : Theme.idleRing
                    Behavior on width { NumberAnimation { duration: 40 } }
                }

                // Where the gate sits on the same scale.
                Rectangle {
                    visible: panel.settings.noiseGateEnabled
                    x: 1 + (parent.width - 2) * panel.meterPosition(panel.settings.noiseGateThresholdDb) - 1
                    y: -2
                    width: 2
                    height: parent.height + 4
                    color: Theme.textSecondary
                }
            }

            Text {
                objectName: "microphoneMeterStatus"
                anchors.left: meter.left
                anchors.top: meter.bottom
                anchors.topMargin: 3
                text: panel.settings.testError !== "" ? panel.settings.testError
                    : !panel.settings.testing ? ""
                    : panel.settings.gateOpen ? "Sending" : "Held back by the gate"
                color: panel.settings.testError !== "" ? Theme.declineMid : Theme.textSecondary
                font.family: Theme.uiFont
                font.pixelSize: 11
                renderType: Text.NativeRendering
            }
        }
    }
}
