import QtQuick
import OpenChat

// A horizontal Aero slider. The control works in a 0..1 position; the caller
// maps that onto whatever the setting is (a percentage, decibels) so the same
// slider serves every scale without knowing about any of them.
//
// Deliberately not a Controls Slider: the rest of the settings surface draws
// its own Aero chrome (AeroSwitch, AeroButton), and matching the knob to the
// switch's knob is what makes the row read as one family.
Item {
    id: control
    // Where the knob sits, 0..1. Set by the caller; the caller updates it from
    // moved(), so a rejected value simply snaps back.
    property real value: 0
    property string accessibleName: ""
    signal moved(real value)

    implicitWidth: 180
    implicitHeight: 26
    width: implicitWidth
    height: implicitHeight
    activeFocusOnTab: true
    Accessible.role: Accessible.Slider
    Accessible.name: accessibleName
    Keys.onLeftPressed: control.nudge(-0.05)
    Keys.onRightPressed: control.nudge(0.05)
    Keys.onPressed: event => {
        if (event.key === Qt.Key_Home) { control.moved(0); event.accepted = true; }
        else if (event.key === Qt.Key_End) { control.moved(1); event.accepted = true; }
    }

    function nudge(delta) {
        control.moved(Math.max(0, Math.min(1, control.value + delta)));
    }

    function positionFor(x) {
        const usable = control.width - knob.width;
        return usable <= 0 ? 0 : Math.max(0, Math.min(1, (x - knob.width / 2) / usable));
    }

    // The groove: a shallow inset, like the text field's well.
    Rectangle {
        id: groove
        anchors.verticalCenter: parent.verticalCenter
        x: knob.width / 2 - 1
        width: parent.width - knob.width + 2
        height: 6
        radius: 3
        color: Theme.fieldBackground
        border.width: 1
        border.color: control.activeFocus ? Theme.focusBorder : Theme.inputBorder
        Rectangle { x: 1; y: 1; width: parent.width - 2; height: 1; color: Theme.insetTop }
    }

    // The filled part, up to the knob, in the switch's blue.
    Rectangle {
        anchors.verticalCenter: parent.verticalCenter
        x: groove.x + 1
        width: Math.max(0, knob.x + knob.width / 2 - x)
        height: groove.height - 2
        radius: height / 2
        gradient: Gradient {
            GradientStop { position: 0; color: Theme.switchTop }
            GradientStop { position: 1; color: Theme.switchBottom }
        }
    }

    Rectangle {
        id: knob
        x: (control.width - width) * control.value
        anchors.verticalCenter: parent.verticalCenter
        width: 18; height: 18; radius: 9
        border.width: 1
        border.color: control.activeFocus ? Theme.focusBorder : Theme.buttonBorder
        gradient: Gradient {
            GradientStop { position: 0; color: Theme.switchKnobTop }
            GradientStop { position: 1; color: Theme.switchKnobBottom }
        }
        Rectangle {
            x: 4; y: 3; width: parent.width - 8; height: 1
            color: Theme.gloss
        }
    }

    MouseArea {
        anchors.fill: parent
        cursorShape: Qt.PointingHandCursor
        onPressed: mouse => {
            control.forceActiveFocus();
            control.moved(control.positionFor(mouse.x));
        }
        onPositionChanged: mouse => {
            if (pressed)
                control.moved(control.positionFor(mouse.x));
        }
    }
}
