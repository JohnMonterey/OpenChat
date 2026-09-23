import QtQuick
import OpenChat
import OpenChat.Native

// Audio & Video → Connection: transport preference for real-time voice calls.
// Controls whether voice media uses low-latency UDP with automatic WebSocket fallback,
// pure UDP, or pure TCP (Relay WebSocket only).
Item {
    id: panel
    objectName: "connectionSettingsPanel"
    readonly property var settings: TransportSettings
    implicitHeight: column.height + 18

    Column {
        id: column
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.topMargin: 12
        spacing: 14

        Text {
            text: "Media Connection"
            color: Theme.textPrimary
            font.family: Theme.uiFont
            font.pixelSize: 15
            renderType: Text.NativeRendering
        }

        Column {
            id: modeList
            objectName: "connectionModeList"
            width: parent.width
            spacing: 4

            readonly property var modes: [
                { id: "auto", name: "Automatic (UDP with TCP fallback)", desc: "Low-latency UDP voice relay; transparently falls back to TCP WebSocket if UDP is blocked." },
                { id: "udp", name: "UDP only", desc: "Forces UDP voice packets. Audio will not fall back to TCP." },
                { id: "tcp", name: "TCP only (Relay WebSocket)", desc: "Carries voice over the reliable TCP WebSocket connection." }
            ]

            Repeater {
                model: modeList.modes

                Rectangle {
                    id: modeRow
                    required property int index
                    required property var modelData
                    objectName: "connectionMode_" + modelData.id
                    readonly property bool selected: panel.settings.mode === modelData.id
                    width: modeList.width
                    height: 48
                    radius: 4
                    color: selected ? Theme.navSelected
                                    : (rowMouse.containsMouse ? Theme.buttonMid : "transparent")
                    border.width: selected || rowMouse.containsMouse ? 1 : 0
                    border.color: selected ? Theme.focusBorder : Theme.buttonBorder
                    Accessible.role: Accessible.RadioButton
                    Accessible.name: modelData.name
                    Accessible.checkable: true
                    Accessible.checked: selected

                    function activate() { panel.settings.mode = modeRow.modelData.id; }

                    Row {
                        anchors.left: parent.left
                        anchors.leftMargin: 10
                        anchors.right: parent.right
                        anchors.rightMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 9

                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 14; height: 14; radius: 7
                            color: Theme.fieldBackground
                            border.width: 1
                            border.color: modeRow.selected ? Theme.focusBorder : Theme.inputBorder
                            Rectangle {
                                anchors.centerIn: parent
                                width: 6; height: 6; radius: 3
                                visible: modeRow.selected
                                color: Theme.switchBottom
                            }
                        }

                        Column {
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width - 24
                            spacing: 2

                            Text {
                                text: modeRow.modelData.name
                                color: Theme.textPrimary
                                font.family: Theme.uiFont
                                font.pixelSize: 13
                                font.bold: modeRow.selected
                                renderType: Text.NativeRendering
                            }

                            Text {
                                text: modeRow.modelData.desc
                                color: Theme.textSecondary
                                font.family: Theme.uiFont
                                font.pixelSize: 11
                                elide: Text.ElideRight
                                width: parent.width
                                renderType: Text.NativeRendering
                            }
                        }
                    }

                    MouseArea {
                        id: rowMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: modeRow.activate()
                    }
                }
            }
        }
    }
}
