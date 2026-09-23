import QtQuick
import OpenChat
import OpenChat.Native

Item {
    id: root
    objectName: "caseReel"
    required property var controller
    readonly property real tileWidth: width < 440 ? 82 : 104
    readonly property real tileGap: 10
    readonly property real stride: tileWidth + tileGap
    readonly property real winnerCenter: width / 2 + (controller.winnerIndex - controller.position) * stride
    property real impact: 0
    height: 198
    clip: true

    Rectangle {
        anchors.fill: parent
        radius: 6
        gradient: Gradient {
            GradientStop { position: 0; color: Theme.fieldBackground }
            GradientStop { position: 1; color: Theme.contentBottom }
        }
        border.color: Theme.rule
    }
    Item {
        id: belt
        objectName: "caseBelt"
        x: root.width / 2 - root.tileWidth / 2 - root.controller.position * root.stride
        y: 35
        Repeater {
            model: root.controller.tileCount
            CaseTile {
                required property int index
                x: index * root.stride
                width: root.tileWidth
                height: 128
                selected: index === root.controller.winnerIndex
                settled: root.controller.state === DailyCaseController.OpenedToday
                animateReveal: !root.controller.reducedMotion
            }
        }
    }
    // A quiet edge vignette conceals partial cards without blurring the belt.
    Rectangle {
        width: 28; height: parent.height
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0; color: Theme.contentBackground }
            GradientStop { position: 1; color: "transparent" }
        }
    }
    Rectangle {
        anchors.right: parent.right
        width: 28; height: parent.height
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0; color: "transparent" }
            GradientStop { position: 1; color: Theme.contentBackground }
        }
    }
    Rectangle {
        objectName: "caseSelector"
        x: (parent.width - width) / 2
        width: 1; height: parent.height - 16; y: 8
        color: Theme.focusBorder
        opacity: 0.85
    }
    Repeater {
        model: 2
        Rectangle {
            required property int index
            x: (root.width - width) / 2
            y: index === 0 ? 10 : root.height - 20
            width: 10; height: 10; rotation: 45; radius: 1
            color: Theme.focusBorder
        }
    }
    Rectangle {
        anchors.centerIn: parent
        width: root.tileWidth + 16 + 55 * root.impact
        height: 144 + 40 * root.impact
        radius: 12
        color: "transparent"
        border.color: Theme.accentBlue
        border.width: 2
        opacity: (1 - root.impact) * root.impact * 3
    }
    NumberAnimation { id: burst; target: root; property: "impact"; from: 0; to: 1; duration: 650; easing.type: Easing.OutCubic }
    Connections {
        target: root.controller
        function onRevealed() { if (!root.controller.reducedMotion) burst.start(); }
    }
}
