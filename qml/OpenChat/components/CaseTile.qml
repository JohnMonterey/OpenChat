import QtQuick
import OpenChat

Rectangle {
    id: root
    property bool selected: false
    property bool settled: false
    property bool animateReveal: true
    // The catalogue map this tile shows (see Cosmetics.item); empty draws the
    // "?" placeholder of a claim saved before rewards existed.
    property var item: ({})
    readonly property bool known: !!(item && item.id)
    readonly property bool won: selected && settled
    // The item's tier colour, as crate grades read: a bar and a glow rising
    // from the bottom edge, and the border once it wins.
    readonly property color tierColor: known ? item.rarityColor : Theme.rule
    readonly property color tierInk: Theme.darkMode ? Qt.lighter(tierColor, 1.3) : Qt.darker(tierColor, 1.3)
    radius: 6
    color: won && !known ? Theme.selectedTop : Theme.buttonBackground
    border.color: won ? (known ? tierColor : Theme.focusBorder) : Theme.inputBorder
    border.width: won && known ? 2 : 1
    opacity: settled && !selected ? 0.38 : 1
    scale: settled && selected ? 1.035 : 1
    y: settled && selected ? -3 : 0
    Behavior on scale { enabled: root.animateReveal; NumberAnimation { duration: 380; easing.type: Easing.OutBack } }
    Behavior on y { enabled: root.animateReveal; NumberAnimation { duration: 380; easing.type: Easing.OutCubic } }
    Behavior on opacity { enabled: root.animateReveal; NumberAnimation { duration: 300 } }

    Rectangle {
        visible: root.known
        anchors.fill: parent
        anchors.margins: root.border.width
        radius: root.radius - 1
        gradient: Gradient {
            GradientStop { position: 0.4; color: "transparent" }
            GradientStop {
                position: 1
                color: Qt.rgba(root.tierColor.r, root.tierColor.g, root.tierColor.b, root.won ? 0.42 : 0.22)
            }
        }
    }
    Rectangle { x: 7; y: 1; width: parent.width - 14; height: 1; color: Theme.glossStrong }

    CosmeticPreview {
        visible: root.known
        x: 6
        y: 12
        width: parent.width - 12
        height: 60
        item: root.item
    }
    Text {
        visible: root.known
        x: 6
        y: 80
        width: parent.width - 12
        horizontalAlignment: Text.AlignHCenter
        elide: Text.ElideRight
        text: root.known ? root.item.name : ""
        color: Theme.textPrimary
        font.family: Theme.uiFont
        font.pixelSize: 12
        // Narrow tiles shrink a long name before eliding it.
        fontSizeMode: Text.HorizontalFit
        minimumPixelSize: 9
    }
    Text {
        visible: root.known
        x: 6
        y: 97
        width: parent.width - 12
        horizontalAlignment: Text.AlignHCenter
        text: root.known ? root.item.rarityName : ""
        color: root.tierInk
        font.family: Theme.uiFont
        font.pixelSize: 10
        font.bold: true
        font.capitalization: Font.AllUppercase
        font.letterSpacing: 0.6
    }
    Rectangle {
        visible: root.known
        x: root.border.width + 3
        y: parent.height - root.border.width - 5
        width: parent.width - 2 * x
        height: 3
        radius: 1.5
        color: root.tierColor
    }

    Rectangle {
        visible: !root.known
        anchors.centerIn: parent
        width: 35; height: 35; radius: 7; rotation: 45
        color: root.won ? Theme.selectedBottom : Theme.fieldBackground
        border.color: Theme.rule
        Text {
            anchors.centerIn: parent
            rotation: -45
            text: "?"
            font.family: Theme.uiFont
            font.pixelSize: 23
            color: root.won ? Theme.focusBorder : Theme.textSecondary
        }
    }
    Rectangle {
        visible: !root.known
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 6
        anchors.horizontalCenter: parent.horizontalCenter
        width: 20; height: 2; radius: 1
        color: root.won ? Theme.focusBorder : Theme.rule
    }
}
