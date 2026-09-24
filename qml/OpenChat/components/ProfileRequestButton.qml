import QtQuick
import OpenChat

// Labelled Accept / Decline for the request stub (SPEC §13): tinted glass that
// keeps a dark label at ≥ 4.5:1 in both app modes, carrying the request row's
// real +/− pill inside it at 26 px. White words on the pill's light gradient
// would measure only 1.5-2.5:1, so the label never sits on the saturated fill.
Item {
    id: button
    property bool accept: true
    property string label: accept ? "Accept" : "Decline"
    property string accessibleName: label
    readonly property bool hovered: area.containsMouse
    signal clicked()

    function activate() {
        if (button.enabled)
            button.clicked();
    }

    implicitWidth: Math.max(128, content.implicitWidth + 22)
    implicitHeight: 36
    width: implicitWidth
    height: implicitHeight
    opacity: enabled ? 1 : 0.55
    activeFocusOnTab: true

    Accessible.role: Accessible.Button
    Accessible.name: button.accessibleName
    Accessible.onPressAction: button.activate()
    Keys.onPressed: event => {
        if (event.key === Qt.Key_Space || event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            button.activate();
            event.accepted = true;
        }
    }

    Rectangle {
        anchors.fill: parent
        radius: 4
        border.width: button.activeFocus ? 2 : 1
        border.color: button.activeFocus ? Theme.focusBorder : (button.accept ? Theme.acceptBorder : Theme.declineBorder)
        gradient: Gradient {
            GradientStop {
                position: 0
                color: button.accept ? (button.hovered ? Qt.lighter(Theme.acceptSoftTop, 1.03) : Theme.acceptSoftTop)
                                     : (button.hovered ? Qt.lighter(Theme.declineSoftTop, 1.03) : Theme.declineSoftTop)
            }
            GradientStop {
                position: 1
                color: button.accept ? (area.pressed ? Qt.darker(Theme.acceptSoftBottom, 1.05) : Theme.acceptSoftBottom)
                                     : (area.pressed ? Qt.darker(Theme.declineSoftBottom, 1.05) : Theme.declineSoftBottom)
            }
        }
        Rectangle {
            x: 4
            y: 1
            width: parent.width - 8
            height: 1
            color: Theme.gloss
        }
    }

    Row {
        id: content
        x: 5
        anchors.verticalCenter: parent.verticalCenter
        spacing: 9
        // The request row's pill.
        Rectangle {
            width: 26
            height: 26
            radius: 4
            border.width: 1
            border.color: button.accept ? Theme.acceptBorder : Theme.declineBorder
            gradient: Gradient {
                GradientStop { position: 0; color: button.accept ? Theme.acceptTop : Theme.declineTop }
                GradientStop { position: 0.5; color: button.accept ? Theme.acceptMid : Theme.declineMid }
                GradientStop { position: 1; color: button.accept ? Theme.acceptBottom : Theme.declineBottom }
            }
            Rectangle {
                x: 2
                y: 1
                width: parent.width - 4
                height: 11
                radius: 3
                color: "#40ffffff"
            }
            Rectangle {
                anchors.centerIn: parent
                width: 12
                height: 2.5
                radius: 1
                color: "white"
            }
            Rectangle {
                visible: button.accept
                anchors.centerIn: parent
                width: 2.5
                height: 12
                radius: 1
                color: "white"
            }
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: button.label
            textFormat: Text.PlainText
            color: button.accept ? Theme.acceptSoftText : Theme.declineSoftText
            font.family: Theme.uiFont
            font.pixelSize: 15
            font.bold: true
            renderType: Text.NativeRendering
        }
        Item {
            width: 3
            height: 1
        }
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: button.activate()
    }
}
