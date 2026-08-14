import QtQuick
import QtQuick.Window

Window {
    id: root
    objectName: "openChatWindow"

    width: 860
    height: 680
    minimumWidth: 720
    minimumHeight: 560
    visible: true
    title: "OpenChat"
    color: Theme.contentBackground

    Text {
        anchors.centerIn: parent
        text: root.title
        color: Theme.textPrimary
        font.family: Theme.uiFont
        font.pixelSize: 20
    }
}
