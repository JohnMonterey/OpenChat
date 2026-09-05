pragma ComponentBehavior: Bound

import QtQuick
import OpenChat

// One inbound contact request: the sender's avatar and identity beside a pair of
// accept / decline actions. Bound to the ContactController handed in as
// `controller`; each button forwards the row's requestId to the matching invokable.
//
// The actions sit in their own right-hand column and the identity column is
// anchored between the avatar and that column, so no element can overlap another
// at any sidebar width.
Item {
    id: row
    required property string requestId
    required property string accountId
    required property string displayName
    required property string subtitle
    required property string handle
    property var controller

    implicitHeight: 64

    Avatar {
        id: avatarImage
        x: 13
        anchors.verticalCenter: parent.verticalCenter
        width: 40
        height: 40
        avatarKey: "userpfp_none"
    }

    // The Aero treatment shared by both actions: a three-stop vertical gradient
    // under a gloss highlight, bounded by a darker rim, brightening on hover. The
    // glyph is a horizontal stroke, crossed by a vertical one when `plus` is set,
    // so accept and decline read as inverses of one another.
    component ActionButton: Item {
        id: action
        property color topColor
        property color midColor
        property color bottomColor
        property color borderColor
        property bool plus: false
        readonly property bool hovered: actionMouse.containsMouse
        // containsPress rather than pressed, so dragging off the button while held
        // returns it to rest the way a native button does.
        readonly property bool pressed: actionMouse.containsPress
        signal clicked

        // Glyph metrics chosen so both strokes land on whole pixels: in the square
        // 34x34 button (34-14)/2 and (34-2)/2 are both integers, so neither stroke
        // straddles a half-pixel and the two share one exact centre. A 13px stroke
        // put both on .5 boundaries, which antialiasing smeared into a visibly
        // off-centre glyph. Matches the 14x2 strokes of the sidebar's add-contact
        // affordance.
        readonly property int glyphLength: 14
        readonly property int glyphThickness: 2

        // Rest, hover, and pressed share one ramp: hover lifts the whole gradient,
        // pressed sinks it below rest so the button reads as pushed in.
        function shade(base) {
            if (action.pressed)
                return Qt.darker(base, 1.18);
            return action.hovered ? Qt.lighter(base, 1.06) : base;
        }

        width: 34
        height: width

        Rectangle {
            anchors.fill: parent
            radius: 4
            border.width: 1
            border.color: action.borderColor
            gradient: Gradient {
                GradientStop { position: 0; color: action.shade(action.topColor) }
                GradientStop { position: 0.52; color: action.shade(action.midColor) }
                GradientStop { position: 1; color: action.shade(action.bottomColor) }
            }
        }

        // Gloss over the upper half, inset from the rim so the border stays crisp.
        Rectangle {
            x: 2
            y: 2
            width: parent.width - 4
            height: (parent.height - 4) / 2
            radius: 2
            // The gloss recedes under press, which is what sells the push.
            color: action.pressed ? "#1fffffff" : "#4dffffff"
        }

        // The minus: the plus's horizontal stroke on its own.
        Rectangle {
            width: action.glyphLength
            height: action.glyphThickness
            x: Math.round((action.width - width) / 2)
            y: Math.round((action.height - height) / 2) + (action.pressed ? 1 : 0)
            radius: 1
            color: "white"
        }
        // Crossed by the vertical stroke to make the plus.
        Rectangle {
            visible: action.plus
            width: action.glyphThickness
            height: action.glyphLength
            x: Math.round((action.width - width) / 2)
            y: Math.round((action.height - height) / 2) + (action.pressed ? 1 : 0)
            radius: 1
            color: "white"
        }

        MouseArea {
            id: actionMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: action.clicked()
        }
    }

    Row {
        id: actions
        anchors.right: parent.right
        anchors.rightMargin: 13
        anchors.verticalCenter: parent.verticalCenter
        spacing: 6

        ActionButton {
            objectName: "requestAccept_" + row.requestId
            topColor: Theme.acceptTop
            midColor: Theme.acceptMid
            bottomColor: Theme.acceptBottom
            borderColor: Theme.acceptBorder
            plus: true
            onClicked: {
                if (row.controller)
                    row.controller.accept(row.requestId);
            }
        }

        ActionButton {
            objectName: "requestDecline_" + row.requestId
            topColor: Theme.declineTop
            midColor: Theme.declineMid
            bottomColor: Theme.declineBottom
            borderColor: Theme.declineBorder
            onClicked: {
                if (row.controller)
                    row.controller.decline(row.requestId);
            }
        }
    }

    Column {
        anchors.left: avatarImage.right
        anchors.leftMargin: 10
        anchors.right: actions.left
        anchors.rightMargin: 10
        anchors.verticalCenter: parent.verticalCenter
        spacing: 2

        Text {
            width: parent.width
            text: row.displayName
            color: Theme.textPrimary
            font.family: Theme.uiFont
            font.pixelSize: 16
            elide: Text.ElideRight
            renderType: Text.NativeRendering
        }

        Text {
            width: parent.width
            text: row.subtitle
            color: Theme.textSecondary
            font.family: Theme.uiFont
            font.pixelSize: 13
            elide: Text.ElideRight
            renderType: Text.NativeRendering
        }
    }
}
