import QtQuick
import OpenChat

Item {
    id: control
    property bool checked: false
    signal toggled(bool checked)
    implicitWidth: 54
    implicitHeight: 30
    width: implicitWidth
    height: implicitHeight
    activeFocusOnTab: true
    Accessible.role: Accessible.CheckBox
    Accessible.name: "Dark mode"
    Accessible.checkable: true
    Accessible.checked: checked
    Accessible.onToggleAction: control.toggled(!control.checked)
    Keys.onSpacePressed: control.toggled(!control.checked)
    Keys.onReturnPressed: control.toggled(!control.checked)

    Rectangle {
        anchors.fill: parent
        radius: height / 2
        border.width: control.activeFocus ? 2 : 1
        border.color: control.checked || control.activeFocus ? Theme.focusBorder : Theme.buttonBorder
        gradient: Gradient {
            GradientStop { position: 0; color: control.checked ? Theme.switchTop : Theme.buttonBottom }
            GradientStop { position: 1; color: control.checked ? Theme.switchBottom : Theme.buttonTop }
        }
        Rectangle {
            x: 7; y: 2; width: parent.width - 14; height: 1
            color: Theme.gloss
        }
    }
    Rectangle {
        x: control.checked ? control.width - width - 3 : 3
        y: 3
        width: 24; height: 24; radius: 12
        border.width: 1
        border.color: Theme.buttonBorder
        gradient: Gradient {
            GradientStop { position: 0; color: Theme.switchKnobTop }
            GradientStop { position: 1; color: Theme.switchKnobBottom }
        }
        Behavior on x { NumberAnimation { duration: 140; easing.type: Easing.InOutQuad } }
    }
    MouseArea {
        anchors.fill: parent
        cursorShape: Qt.PointingHandCursor
        onClicked: {
            control.forceActiveFocus();
            control.toggled(!control.checked);
        }
    }
}
