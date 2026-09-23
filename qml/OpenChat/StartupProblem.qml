import QtQuick
import QtQuick.Window
import OpenChat

// What OpenChat shows when it cannot start, instead of quitting without a
// word. On Windows a failed start used to look like nothing at all: the
// explanation went to a console window that closed with the process.
Window {
    id: startupProblem
    objectName: "startupProblem"
    required property string headline
    required property string explanation

    width: 520
    height: Math.max(220, content.implicitHeight + 44)
    minimumWidth: 400
    minimumHeight: 200
    visible: true
    title: "OpenChat"
    color: Theme.contentBackground

    Column {
        id: content
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 22
        spacing: 14

        Text {
            objectName: "startupProblemHeadline"
            width: parent.width
            text: startupProblem.headline
            color: Theme.textPrimary
            font.family: Theme.uiFont
            font.pixelSize: 21
            font.bold: true
            wrapMode: Text.WordWrap
        }

        Text {
            objectName: "startupProblemExplanation"
            width: parent.width
            text: startupProblem.explanation
            color: Theme.textPrimary
            font.family: Theme.uiFont
            font.pixelSize: 14
            wrapMode: Text.WordWrap
        }

        AeroButton {
            objectName: "startupProblemCloseButton"
            width: 72
            height: 34
            fontPixelSize: 13
            label: "Close"
            onClicked: startupProblem.close()
        }
    }
}
