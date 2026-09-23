import QtQuick
import OpenChat
import OpenChat.Native

// General → Low memory mode: one switch, what it costs, and — while this
// process still runs with the other choice — the restart that finishes
// switching. MemorySettings saves the choice and applies what it can at once;
// drawing without the graphics card has to wait for the restart.
Item {
    id: panel
    objectName: "lowMemoryPanel"
    // A restart ends a call, so the window withholds it during one.
    property bool restartAllowed: true
    implicitHeight: column.height + 18

    Column {
        id: column
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.topMargin: 12
        spacing: 12

        Item {
            width: parent.width
            height: 44

            Text {
                anchors.left: parent.left
                anchors.top: parent.top
                text: "Low memory mode"
                color: Theme.textPrimary
                font.family: Theme.uiFont
                font.pixelSize: 15
                renderType: Text.NativeRendering
            }
            Text {
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                width: parent.width - lowMemorySwitch.width - 16
                text: "Use as little memory as possible"
                elide: Text.ElideRight
                color: Theme.textSecondary
                font.family: Theme.uiFont
                font.pixelSize: 12
            }
            AeroSwitch {
                id: lowMemorySwitch
                objectName: "lowMemorySwitch"
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                Accessible.name: "Low memory mode"
                checked: MemorySettings.lowMemoryMode
                onToggled: checked => MemorySettings.lowMemoryMode = checked
            }
        }

        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            text: "Draws OpenChat without the graphics card, keeps profile pictures compressed "
                  + "until they are shown, loads call audio only when a call starts, and hands "
                  + "unused memory back to the system every minute. Scrolling and animations "
                  + "take more processor time."
            color: Theme.textSecondary
            font.family: Theme.uiFont
            font.pixelSize: 13
            renderType: Text.NativeRendering
        }

        Item {
            objectName: "lowMemoryRestart"
            width: parent.width
            height: visible ? 36 : 0
            visible: MemorySettings.restartPending

            Text {
                anchors.left: parent.left
                anchors.right: restartButton.left
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                wrapMode: Text.WordWrap
                text: panel.restartAllowed ? "Restart OpenChat to finish switching."
                                           : "Restart OpenChat after the call to finish switching."
                color: Theme.textPrimary
                font.family: Theme.uiFont
                font.pixelSize: 13
                renderType: Text.NativeRendering
            }
            AeroButton {
                id: restartButton
                objectName: "lowMemoryRestartButton"
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                width: 130
                height: 34
                label: "Restart now"
                fontPixelSize: 14
                enabled: panel.restartAllowed
                onClicked: MemorySettings.restartApplication()
            }
        }
    }
}
