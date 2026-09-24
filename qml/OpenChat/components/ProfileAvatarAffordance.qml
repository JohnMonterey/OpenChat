import QtQuick
import OpenChat
import OpenChat.Native

// The one treatment every clickable person picture gets (SPEC §12), laid over
// `target` (a sibling: it copies the target's geometry). Hover: a 2 px
// focusBorder ring 3 px outside the picture, an avatarHalo ring 5 px outside,
// the profile badge on the corner and a pointing hand, faded in over 120 ms.
// Keyboard focus: the rings without the badge. Pressed: a profileHover shade.
// The face itself is never darkened: darkening plus a glyph stays the "change
// picture" language on your own photo.
//
// With `interactive` (the default) it has its own mouse area and keyboard
// handling and raises clicked(). A row whose own MouseArea decides what a
// click means sets interactive: false and drives `hovered` and `pressed`.
Item {
    id: affordance
    property Item target: null
    property bool hovered: false
    property bool pressed: false
    // A container that keeps keyboard focus itself (a tile grid) lights the
    // current picture through this.
    property bool focused: false
    property bool interactive: true
    property bool keyboardFocusable: false
    property string accessibleName: ""
    // −1: by the picture's size (18 px, 22 px from 68 px up, none under 36).
    property int badgeSize: -1
    property real cornerRadius: target && target.cornerRadius !== undefined ? target.cornerRadius : 5
    signal clicked()

    readonly property bool pointerOver: interactive ? area.containsMouse : hovered
    readonly property bool pressedNow: interactive ? area.pressed : pressed
    readonly property bool lit: pointerOver || focused || activeFocus
    readonly property int shownBadgeSize: badgeSize >= 0 ? badgeSize : width >= 68 ? 22 : width >= 36 ? 18 : 0
    readonly property int fadeMs: ProfileRenderPolicy.animationsAllowed ? 120 : 0

    x: target ? target.x : 0
    y: target ? target.y : 0
    width: target ? target.width : 0
    height: target ? target.height : 0
    activeFocusOnTab: interactive && keyboardFocusable

    Accessible.role: Accessible.Button
    Accessible.name: affordance.accessibleName
    Accessible.ignored: !affordance.interactive
    Accessible.onPressAction: if (affordance.interactive) affordance.clicked()

    Keys.onPressed: event => {
        if (affordance.interactive && (event.key === Qt.Key_Space || event.key === Qt.Key_Return
                                       || event.key === Qt.Key_Enter)) {
            affordance.clicked();
            event.accepted = true;
        }
    }

    Item {
        objectName: "profileAffordanceRings"
        anchors.fill: parent
        opacity: affordance.lit ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: affordance.fadeMs; easing.type: Easing.InOutQuad } }

        Rectangle {
            anchors.fill: parent
            anchors.margins: -5
            radius: affordance.cornerRadius + 5
            color: "transparent"
            border.width: 2
            border.color: Theme.avatarHalo
        }
        Rectangle {
            anchors.fill: parent
            anchors.margins: -3
            radius: affordance.cornerRadius + 3
            color: "transparent"
            border.width: 2
            border.color: Theme.focusBorder
        }
    }

    Rectangle {
        objectName: "profileAffordanceShade"
        anchors.fill: parent
        visible: affordance.pressedNow
        radius: affordance.cornerRadius
        color: Theme.profileHover
    }

    ProfileBadge {
        objectName: "profileAffordanceBadge"
        size: Math.max(1, affordance.shownBadgeSize)
        readonly property int overlap: affordance.shownBadgeSize >= 22 ? 6 : 5
        x: affordance.width - width + overlap
        y: affordance.height - height + overlap
        opacity: affordance.pointerOver && affordance.shownBadgeSize > 0 ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: affordance.fadeMs; easing.type: Easing.InOutQuad } }
    }

    MouseArea {
        id: area
        anchors.fill: parent
        enabled: affordance.interactive
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: affordance.clicked()
    }
}
