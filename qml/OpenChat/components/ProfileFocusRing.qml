import QtQuick
import OpenChat

// The app's own keyboard-focus ring on a profile page (SPEC §16.2): a 2 px
// focusBorder ring 2 px outside the element and a 1 px contrasting halo just
// outside that, so it reads on every theme. It never takes the page's link
// colour: a page can recolour its boxes, not the app's focus.
//
// Fill the focused element with it (anchors.fill: parent) and bind `shown`.
Item {
    id: ring
    objectName: "profileFocusRing"
    property bool shown: false
    // The focused element's corner radius; the ring follows it outwards.
    property real radius: 3
    // Over a dark box the halo is light, over a light box dark.
    property bool onDark: false
    readonly property color ringColor: Theme.focusBorder

    visible: shown
    Accessible.ignored: true

    Rectangle {
        anchors.fill: parent
        anchors.margins: -3
        radius: ring.radius + 3
        color: "transparent"
        border.width: 1
        border.color: ring.onDark ? "#bfffffff" : "#40000000"
    }
    Rectangle {
        anchors.fill: parent
        anchors.margins: -2
        radius: ring.radius + 2
        color: "transparent"
        border.width: 2
        border.color: ring.ringColor
    }
}
