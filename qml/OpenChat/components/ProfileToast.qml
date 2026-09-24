import QtQuick
import OpenChat
import OpenChat.Native

// The Save toast (SPEC §14.12): a tooltip-glass pill with a green check orb
// that says where the new page is going, then fades away by itself. It
// announces itself to screen readers as an alert.
Item {
    id: toast
    objectName: "profileToast"
    property string text: ""
    property int holdMs: 4000
    property bool shown: false
    readonly property int fadeMs: ProfileRenderPolicy.animationsAllowed ? 150 : 0

    function show(message) {
        toast.text = message;
        toast.shown = true;
        hold.restart();
    }

    // The orb (18), its gap (8) and 18 px of glass each side, around the words.
    width: Math.min(parent ? parent.width - 32 : 480, label.implicitWidth + 62)
    height: 36
    opacity: shown ? 1 : 0
    visible: opacity > 0
    Behavior on opacity { NumberAnimation { duration: toast.fadeMs; easing.type: Easing.InOutQuad } }

    Accessible.role: Accessible.AlertMessage
    Accessible.name: toast.text

    Timer {
        id: hold
        interval: toast.holdMs
        onTriggered: toast.shown = false
    }

    Rectangle {
        x: 1
        y: 2
        width: parent.width - 2
        height: parent.height
        radius: height / 2
        color: Theme.tooltipShadowFill
    }
    Rectangle {
        anchors.fill: parent
        radius: height / 2
        border.width: 1
        border.color: Theme.tooltipBorder
        gradient: Gradient {
            GradientStop { position: 0; color: Theme.tooltipTop }
            GradientStop { position: 0.48; color: Theme.tooltipMid }
            GradientStop { position: 1; color: Theme.tooltipBottom }
        }
        Rectangle {
            x: parent.radius
            y: 1
            width: parent.width - 2 * parent.radius
            height: 1
            color: Theme.tooltipHighlight
        }
    }
    Row {
        id: row
        anchors.centerIn: parent
        spacing: 8
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: 18
            height: 18
            radius: 9
            color: Theme.successFill
            ProfileGlyph {
                anchors.centerIn: parent
                width: 12
                height: 12
                kind: "check"
                ink: "white"
            }
        }
        Text {
            id: label
            anchors.verticalCenter: parent.verticalCenter
            width: Math.min(implicitWidth, toast.width - 62)
            elide: Text.ElideRight
            text: toast.text
            textFormat: Text.PlainText
            color: Theme.textPrimary
            font.family: Theme.uiFont
            font.pixelSize: 13
            renderType: Text.NativeRendering
        }
    }
}
