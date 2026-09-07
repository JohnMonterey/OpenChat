import QtQuick
import QtQuick.Shapes
import QtQuick.Controls as Controls
import OpenChat

// Compact call controls. Shares the request row's Aero treatment
// — three-stop gradient, gloss highlight, darker rim — so the call surface reads
// as part of the same interface rather than a separate app.
Item {
    id: button
    property string label: ""
    // "accept" (green), "end" (red), or anything else for the neutral chrome.
    property string accent: "neutral"
    property bool cameraIcon: false
    property bool screenIcon: false
    property bool microphoneIcon: false
    property bool hangupIcon: false
    property bool square: false
    property bool contextMenuEnabled: false
    property bool contextMenuExpanded: false
    property bool checked: false
    // A control that is present but cannot be used right now: greyed, still
    // focusable and still readable, so it can explain itself through its
    // tooltip rather than vanishing and leaving the row a different shape.
    property bool disabled: false
    // The platform's hold interval also gives hover hints a deliberate delay.
    // Use the standard Qt tooltip so its appearance follows the platform style.
    property string tooltip: label
    readonly property int tooltipDelay: Qt.styleHints.mousePressAndHoldInterval
    readonly property bool iconic: cameraIcon || screenIcon || microphoneIcon || hangupIcon
    activeFocusOnTab: !disabled
    opacity: disabled ? 0.45 : 1.0
    Accessible.role: Accessible.Button
    Accessible.name: label
    Accessible.description: tooltip
    Accessible.checkable: cameraIcon || screenIcon || microphoneIcon
    Accessible.checked: checked
    Accessible.onPressAction: if (!button.disabled) clicked()
    Keys.onSpacePressed: if (!button.disabled) clicked()
    Keys.onReturnPressed: if (!button.disabled) clicked()
    Keys.onPressed: event => {
        if (button.contextMenuEnabled && !button.disabled
                && (event.key === Qt.Key_Menu
                    || (event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier)))) {
            button.contextMenuRequested(button.width / 2, button.height);
            event.accepted = true;
        }
    }
    signal clicked
    signal contextMenuRequested(real x, real y)

    readonly property color topColor: accent === "accept" ? Theme.acceptTop
                                    : accent === "end" ? Theme.endCallTop : Theme.buttonTop
    readonly property color midColor: accent === "accept" ? Theme.acceptMid
                                    : accent === "end" ? Theme.endCallMid : Theme.buttonMid
    readonly property color bottomColor: accent === "accept" ? Theme.acceptBottom
                                       : accent === "end" ? Theme.endCallBottom : Theme.buttonBottom
    readonly property color rimColor: accent === "accept" ? Theme.acceptBorder
                                    : accent === "end" ? Theme.endCallBorder : Theme.buttonBorder
    readonly property bool onAccent: accent === "accept" || accent === "end"

    implicitWidth: square ? implicitHeight : caption.implicitWidth + 34 + (iconic ? 24 : 0)
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
            id: micArtwork
            visible: button.microphoneIcon
            width: 17
            height: 17
            readonly property color ink: button.checked ? Theme.endCallBottom : Theme.buttonText
            Rectangle {
                x: 6; y: 1; width: 5; height: 9; radius: 2.5
                color: parent.ink
            }
            Shape {
                anchors.fill: parent
                ShapePath {
                    fillColor: "transparent"
                    strokeColor: micArtwork.ink
                    strokeWidth: 1.5
                    capStyle: ShapePath.RoundCap
                    PathSvg { path: "M 3.5 7.5 L 3.5 8.5 C 3.5 14.5 13.5 14.5 13.5 8.5 L 13.5 7.5 M 8.5 13 L 8.5 16 M 5.5 16 L 11.5 16" }
                }
            }
            Rectangle {
                visible: button.checked
                x: 7.75; y: -1; width: 1.5; height: 20
                rotation: -45
                color: parent.ink
            }
        }
        Shape {
            visible: button.hangupIcon
            width: 17
            height: 17
            ShapePath {
                fillColor: Theme.onAccentText
                strokeWidth: 0
                PathSvg { path: "M 1 7 Q 8.5 1 16 7 Q 17 8 16 10 L 14 12 Q 13 12.5 12 11 L 11 8.5 Q 8.5 7 6 8.5 L 5 11 Q 4 12.5 3 12 L 1 10 Q 0 8 1 7 Z" }
            }
        }
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
            visible: !button.square
            text: button.label
            color: button.onAccent ? Theme.onAccentText : Theme.buttonText
            font.family: Theme.uiFont
            font.pixelSize: 13
            renderType: Text.NativeRendering
        }
    }

    // A corner chevron keeps the microphone's primary mute action distinct
    // from its secondary settings menu, and follows the menu's real lifetime.
    Shape {
        objectName: "callActionMenuArrow"
        visible: button.contextMenuEnabled
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.rightMargin: 3
        anchors.bottomMargin: 3
        width: 6
        height: 4
        rotation: button.contextMenuExpanded ? 180 : 0
        ShapePath {
            fillColor: "transparent"
            strokeColor: button.onAccent ? Theme.onAccentText : Theme.buttonText
            strokeWidth: 1.2
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            PathSvg { path: "M 0.5 0.5 L 3 3 L 5.5 0.5" }
        }
    }

    MouseArea {
        id: buttonMouse
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: button.contextMenuEnabled ? Qt.LeftButton | Qt.RightButton : Qt.LeftButton
        cursorShape: button.disabled ? Qt.ArrowCursor : Qt.PointingHandCursor
        onClicked: mouse => {
            if (button.disabled)
                return;
            button.forceActiveFocus();
            if (mouse.button === Qt.RightButton)
                button.contextMenuRequested(mouse.x, mouse.y);
            else
                button.clicked();
        }
    }

    Controls.ToolTip {
        objectName: "callActionTooltip"
        parent: button
        text: button.tooltip
        delay: button.tooltipDelay
        visible: button.tooltip.length > 0 && buttonMouse.containsMouse
                 && !buttonMouse.pressed && !button.contextMenuExpanded
    }
}
