import QtQuick
import OpenChat

Item {
    id: frame
    objectName: "aeroWindowFrame"

    required property Window window

    Rectangle {
        anchors.fill: parent
        anchors.margins: 5
        radius: 9
        color: "#2a000000"
    }

    Rectangle {
        anchors.fill: parent
        anchors.margins: 1
        radius: 9
        border.width: 1
        border.color: Theme.frameOuter
        gradient: Gradient {
            GradientStop { position: 0.0; color: Theme.glassTop }
            GradientStop { position: 0.23; color: Theme.glassMid }
            GradientStop { position: 1.0; color: Theme.glassBottom }
        }
    }

    Rectangle {
        anchors.fill: parent
        anchors.margins: 2
        radius: 8
        color: "transparent"
        border.width: 1
        border.color: "#bdeeff"
    }

    Rectangle {
        x: Theme.frameInset
        y: Theme.titleBarHeight
        width: parent.width - Theme.frameInset * 2
        height: parent.height - Theme.titleBarHeight - Theme.frameInset
        color: "transparent"
        border.width: 1
        border.color: "#355f79"
    }

    MouseArea {
        x: Theme.frameInset
        y: 2
        width: parent.width - Theme.frameInset * 2 - 164
        height: Theme.titleBarHeight - 3
        onPressed: frame.window.startSystemMove()
        onDoubleClicked: {
            if (frame.window.visibility === Window.Maximized)
                frame.window.showNormal()
            else
                frame.window.showMaximized()
        }
    }

    Item {
        x: 17
        y: 13
        width: 20
        height: 20

        Rectangle {
            anchors.fill: parent
            radius: 3
            border.width: 1
            border.color: "#4f8cae"
            gradient: Gradient {
                GradientStop { position: 0; color: "#8ee1ff" }
                GradientStop { position: 1; color: "#3aa3d6" }
            }
        }
        Rectangle { x: 4; y: 5; width: 8; height: 6; radius: 2; color: "#e9fbff" }
        Rectangle { x: 8; y: 9; width: 8; height: 6; radius: 2; color: "#ffffff" }
        Rectangle { x: 6; y: 9; width: 3; height: 3; rotation: 45; color: "#e9fbff" }
        Rectangle { x: 13; y: 13; width: 3; height: 3; rotation: 45; color: "#ffffff" }
    }

    Text {
        x: 46
        y: 11
        text: "OpenChat"
        color: Theme.titleText
        font.family: Theme.uiFont
        font.pixelSize: 17
        style: Text.Raised
        styleColor: "#b8ffffff"
    }

    Row {
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.rightMargin: 12
        spacing: 1

        CaptionButton {
            symbol: "−"
            buttonWidth: 46
            onTriggered: frame.window.showMinimized()
        }
        CaptionButton {
            symbol: frame.window.visibility === Window.Maximized ? "❐" : "□"
            buttonWidth: 43
            onTriggered: {
                if (frame.window.visibility === Window.Maximized)
                    frame.window.showNormal()
                else
                    frame.window.showMaximized()
            }
        }
        CaptionButton {
            symbol: "×"
            buttonWidth: 74
            destructive: true
            onTriggered: frame.window.close()
        }
    }

    component CaptionButton: Item {
        id: caption
        property string symbol
        property int buttonWidth: 44
        property bool destructive: false
        signal triggered

        width: buttonWidth
        height: 31

        Rectangle {
            anchors.fill: parent
            border.width: 1
            border.color: caption.destructive ? "#8c3430" : "#47728d"
            gradient: Gradient {
                GradientStop {
                    position: 0
                    color: caption.destructive
                           ? (captionMouse.containsMouse ? "#ff806b" : "#e98a7c")
                           : (captionMouse.containsMouse ? "#d8f2ff" : "#c8e5f4")
                }
                GradientStop { position: 0.5; color: caption.destructive ? "#d94d3d" : "#7db4d1" }
                GradientStop { position: 1; color: caption.destructive ? "#a91e1c" : "#4d8fb7" }
            }
        }
        Text {
            anchors.centerIn: parent
            anchors.verticalCenterOffset: caption.symbol === "×" ? -1 : -3
            text: caption.symbol
            color: "white"
            font.family: "Arial"
            font.bold: true
            font.pixelSize: caption.symbol === "×" ? 23 : 18
            style: Text.Outline
            styleColor: caption.destructive ? "#7f2420" : "#446579"
        }
        MouseArea {
            id: captionMouse
            anchors.fill: parent
            hoverEnabled: true
            onClicked: caption.triggered()
        }
    }
}
