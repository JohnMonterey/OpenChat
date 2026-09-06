import QtQuick
import QtQuick.Shapes
import OpenChat

// A compact pill for the call controls. Shares the request row's Aero treatment
// — three-stop gradient, gloss highlight, darker rim — so the call surface reads
// as part of the same interface rather than a separate app.
Item {
    id: button
    property string label: ""
    // "accept" (green), "end" (red), or anything else for the neutral chrome.
    property string accent: "neutral"
    property bool cameraIcon: false
    property bool screenIcon: false
    property bool checked: false
    // A control that is present but cannot be used right now: greyed, still
    // focusable and still readable, so it can explain itself through its
    // tooltip rather than vanishing and leaving the row a different shape.
    property bool disabled: false
    // Shown on hover. Says what the button does, and when it is disabled, why.
    property string tooltip: ""
    readonly property bool iconic: cameraIcon || screenIcon
    activeFocusOnTab: !disabled
    opacity: disabled ? 0.45 : 1.0
    Accessible.role: Accessible.Button
    Accessible.name: label
    Accessible.description: tooltip
    Accessible.checkable: iconic
    Accessible.checked: checked
    Accessible.onPressAction: if (!button.disabled) clicked()
    Keys.onSpacePressed: if (!button.disabled) clicked()
    Keys.onReturnPressed: if (!button.disabled) clicked()
    signal clicked

    readonly property color topColor: accent === "accept" ? Theme.acceptTop
                                    : accent === "end" ? Theme.endCallTop : Theme.buttonTop
    readonly property color midColor: accent === "accept" ? Theme.acceptMid
                                    : accent === "end" ? Theme.endCallMid : Theme.buttonMid
    readonly property color bottomColor: accent === "accept" ? Theme.acceptBottom
                                       : accent === "end" ? Theme.endCallBottom : Theme.buttonBottom
    readonly property color rimColor: accent === "accept" ? Theme.acceptBorder
                                    : accent === "end" ? Theme.endCallBorder : Theme.buttonBorder
    readonly property bool onAccent: accent === "accept" || accent === "end"

    implicitWidth: caption.implicitWidth + 34 + (iconic ? 24 : 0)
    implicitHeight: 30
    width: implicitWidth
    height: implicitHeight

    Rectangle {
        anchors.fill: parent
        radius: 4
        border.width: 1
        border.color: (button.activeFocus || button.checked) && !button.disabled
                      ? Theme.focusBorder : button.rimColor
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
            opacity: buttonMouse.containsMouse && !button.disabled ? 0.42 : 0.28
            gradient: Gradient {
                GradientStop { position: 0; color: Theme.glossStrong }
                GradientStop { position: 1; color: "#ffffff00" }
            }
        }
    }

    Row {
        anchors.centerIn: parent
        spacing: 7
        Item {
            visible: button.cameraIcon
            width: 17
            height: 17
            Rectangle {
                x: 0; y: 4; width: 12; height: 10; radius: 2
                color: button.checked ? Theme.cameraAccent : Theme.buttonText
            }
            Shape {
                x: 12; y: 4; width: 5; height: 10
                ShapePath {
                    fillColor: button.checked ? Theme.cameraAccent : Theme.buttonText
                    strokeWidth: 0
                    PathSvg { path: "M 0 3 L 5 0 L 5 10 L 0 7 Z" }
                }
            }
        }
        // A display on a stand, drawn to the same 17px box and the same accent as
        // the camera above it: the two controls are siblings and look it.
        Item {
            visible: button.screenIcon
            width: 17
            height: 17

            Rectangle {
                x: 1; y: 3; width: 15; height: 10; radius: 2
                color: "transparent"
                border.width: 1.5
                border.color: button.checked ? Theme.cameraAccent : Theme.buttonText
            }
            Rectangle {
                x: 6; y: 13; width: 5; height: 1.5
                color: button.checked ? Theme.cameraAccent : Theme.buttonText
            }
        }
        Text {
            id: caption
            text: button.label
            color: button.onAccent ? Theme.onAccentText : Theme.buttonText
            font.family: Theme.uiFont
            font.pixelSize: 13
            renderType: Text.NativeRendering
        }
    }

    MouseArea {
        id: buttonMouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: button.disabled ? Qt.ArrowCursor : Qt.PointingHandCursor
        onClicked: {
            if (!button.disabled)
                button.clicked();
        }
    }

    // The tooltip, in the same flat card the rest of the app uses for
    // transient explanations.
    Rectangle {
        objectName: "callActionTooltip"
        visible: button.tooltip.length > 0 && buttonMouse.containsMouse
        anchors.bottom: parent.top
        anchors.bottomMargin: 6
        anchors.horizontalCenter: parent.horizontalCenter
        width: tooltipText.implicitWidth + 16
        height: tooltipText.implicitHeight + 10
        radius: 3
        color: Theme.contentBackground
        border.width: 1
        border.color: Theme.inputBorder
        z: 10

        Text {
            id: tooltipText
            anchors.centerIn: parent
            text: button.tooltip
            color: Theme.textPrimary
            font.family: Theme.uiFont
            font.pixelSize: 12
            renderType: Text.NativeRendering
        }
    }
}
