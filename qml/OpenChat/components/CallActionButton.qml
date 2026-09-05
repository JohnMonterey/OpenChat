import QtQuick
import OpenChat

// A compact pill for the call controls. Shares the request row's Aero treatment
// — three-stop gradient, gloss highlight, darker rim — so the call surface reads
// as part of the same interface rather than a separate app.
Item {
    id: button
    property string label: ""
    // "accept" (green), "end" (red), or anything else for the neutral chrome.
    property string accent: "neutral"
    signal clicked

    readonly property color topColor: accent === "accept" ? Theme.acceptTop
                                    : accent === "end" ? Theme.endCallTop : "#fafcfd"
    readonly property color midColor: accent === "accept" ? Theme.acceptMid
                                    : accent === "end" ? Theme.endCallMid : "#eef3f7"
    readonly property color bottomColor: accent === "accept" ? Theme.acceptBottom
                                       : accent === "end" ? Theme.endCallBottom : "#e2eaf1"
    readonly property color rimColor: accent === "accept" ? Theme.acceptBorder
                                    : accent === "end" ? Theme.endCallBorder : "#aebdca"
    readonly property bool onAccent: accent === "accept" || accent === "end"

    implicitWidth: caption.implicitWidth + 34
    implicitHeight: 30
    width: implicitWidth
    height: implicitHeight

    Rectangle {
        anchors.fill: parent
        radius: 4
        border.width: 1
        border.color: button.rimColor
        gradient: Gradient {
            GradientStop { position: 0; color: button.topColor }
            GradientStop { position: 0.5; color: button.midColor }
            GradientStop { position: 1; color: button.bottomColor }
        }

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 1
            height: parent.height / 2
            radius: 3
            opacity: buttonMouse.containsMouse ? 0.42 : 0.28
            gradient: Gradient {
                GradientStop { position: 0; color: "#ffffff" }
                GradientStop { position: 1; color: "#ffffff00" }
            }
        }
    }

    Text {
        id: caption
        anchors.centerIn: parent
        text: button.label
        color: button.onAccent ? "#ffffff" : "#3f5570"
        font.family: Theme.uiFont
        font.pixelSize: 13
        renderType: Text.NativeRendering
    }

    MouseArea {
        id: buttonMouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: button.clicked()
    }
}
