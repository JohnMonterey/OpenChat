import QtQuick
import OpenChat
import OpenChat.Native

// One action of the Contacting box (SPEC §5.2): an 18 px glyph at x 7 and
// the label at x 32 in the link ink, in the app's own face. When the full
// label (plus the ✓ orb) does not fit the cell it switches to the compact
// label; it never elides or shrinks. Hover: a link-tinted fill and edge, the
// label underlined. Disabled: 45% and still focusable, explaining itself in
// its tooltip rather than vanishing.
Item {
    id: cell
    property var render: null
    property string glyph: "envelope"
    property string label: ""
    property string compactLabel: label
    // The full action for screen readers ("Send Message to Michael").
    property string accessibleName: label
    // Only a real action is ever shown; an unavailable one says why here.
    property bool available: true
    property string reason: ""
    // A green ✓ orb after the label (a verified safety number).
    property bool badge: false
    // The keyboard focus is on this cell (the grid moves it between cells).
    property bool keyboardFocus: false
    // The editor preview shows the box at full strength but inert.
    property bool inert: false
    signal activated()

    readonly property color ink: render ? render.linkColor : "#1f6fa3"
    readonly property bool hovered: area.containsMouse && !inert
    readonly property bool compact: 32 + fullMetrics.advanceWidth + (badge ? 20 : 0) + 4 > width

    height: 30
    opacity: available ? 1 : 0.45

    Accessible.role: Accessible.Button
    Accessible.name: cell.accessibleName
    Accessible.description: cell.available ? "" : cell.reason
    Accessible.onPressAction: if (cell.available && !cell.inert) cell.activated()

    TextMetrics {
        id: fullMetrics
        font: labelText.font
        text: cell.label
    }

    Rectangle {
        anchors.fill: parent
        radius: 3
        visible: cell.hovered && cell.available
        color: Qt.rgba(cell.ink.r, cell.ink.g, cell.ink.b, area.pressed ? 0.22 : 0.13)
        border.width: 1
        border.color: Qt.rgba(cell.ink.r, cell.ink.g, cell.ink.b, 0.40)
    }

    ProfileGlyph {
        x: 7
        anchors.verticalCenter: parent.verticalCenter
        width: 18
        height: 18
        kind: cell.glyph
        ink: cell.ink
    }

    Row {
        x: 32
        anchors.verticalCenter: parent.verticalCenter
        spacing: 5
        Text {
            id: labelText
            objectName: "profileActionLabel"
            anchors.verticalCenter: parent.verticalCenter
            text: cell.compact ? cell.compactLabel : cell.label
            textFormat: Text.PlainText
            color: cell.ink
            font.family: Theme.uiFont
            font.pixelSize: cell.render ? cell.render.bodyPixelSize : 13
            font.underline: cell.hovered && cell.available
            renderType: Text.NativeRendering
        }
        Rectangle {
            objectName: "profileVerifiedOrb"
            visible: cell.badge
            anchors.verticalCenter: parent.verticalCenter
            width: 15
            height: 15
            radius: 7.5
            color: Theme.successFill
            border.width: 1
            border.color: Qt.darker(Theme.successFill, 1.25)
            ProfileGlyph {
                anchors.centerIn: parent
                width: 10
                height: 10
                kind: "check"
                ink: "white"
                stroke: 1.6
            }
        }
    }

    ProfileFocusRing {
        anchors.fill: parent
        shown: cell.keyboardFocus
        radius: 3
        onDark: cell.render ? cell.render.boxDark : false
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        enabled: !cell.inert
        cursorShape: cell.available ? Qt.PointingHandCursor : Qt.ArrowCursor
        onClicked: if (cell.available) cell.activated()
    }

    ProfileTip {
        parent: cell
        visible: !cell.available && cell.reason.length > 0 && area.containsMouse
        text: cell.reason
        y: cell.height + 4
    }
}
