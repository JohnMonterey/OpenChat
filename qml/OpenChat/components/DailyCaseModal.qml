import QtQuick
import QtQuick.Controls
import OpenChat
import OpenChat.Native

Popup {
    id: root
    objectName: "dailyCaseModal"
    required property var controller
    property var returnFocus: null
    readonly property var reward: controller ? controller.reward : ({})
    readonly property bool rewarded: !!(reward && reward.id)
    readonly property var categoryNames: ({
        bubble: "chat bubble", frame: "avatar frame", bead: "presence bead",
        flair: "name flair", scene: "profile scene"
    })
    function ink(color) { return Theme.darkMode ? Qt.lighter(color, 1.3) : Qt.darker(color, 1.3) }
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(700, parent.width - 32)
    height: 452
    padding: 24
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    Overlay.modal: Rectangle { color: Theme.dialogScrim }
    background: Rectangle {
        color: Theme.contentBackground
        border.color: Theme.inputBorder
        radius: 8
        Rectangle { x: 8; y: 1; width: parent.width - 16; height: 1; color: Theme.glossStrong }
    }
    onOpened: {
        action.forceActiveFocus()
        if (controller) controller.display()
    }
    onAboutToHide: { if (controller) controller.dismiss(); }
    onClosed: {
        if (returnFocus) returnFocus.forceActiveFocus()
    }
    Component.onDestruction: { if (controller) controller.dismiss(); }

    component Preference: CheckBox {
        id: toggle
        palette.windowText: Theme.textSecondary
        font.family: Theme.uiFont
        font.pixelSize: 12
        indicator: Rectangle {
            implicitWidth: 14; implicitHeight: 14
            x: 4; y: (toggle.height - height) / 2
            radius: 2
            color: Theme.fieldBackground
            border.color: toggle.activeFocus ? Theme.focusBorder : Theme.inputBorder
            Rectangle {
                anchors.fill: parent; anchors.margins: 3; radius: 1
                color: Theme.focusBorder
                visible: toggle.checked
            }
        }
    }

    contentItem: Item {
        Text {
            text: "Daily case"
            color: Theme.textPrimary
            font.family: Theme.uiFont
            font.pixelSize: 23
        }
        Text {
            y: 33
            text: "A little moment, once a day."
            color: Theme.textSecondary
            font.family: Theme.uiFont
            font.pixelSize: 13
        }
        Button {
            id: closeButton
            objectName: "caseCloseButton"
            anchors.right: parent.right
            width: 30; height: 30
            text: "×"
            palette.buttonText: Theme.iconInk
            background: Rectangle {
                radius: 4
                color: closeButton.hovered ? Theme.buttonHover : "transparent"
                border.color: closeButton.activeFocus ? Theme.focusBorder : "transparent"
            }
            Accessible.name: "Close daily case"
            onClicked: root.close()
            KeyNavigation.tab: sound
        }
        Loader {
            id: reelLoader
            y: 64
            width: parent.width
            height: 198
            active: root.visible
            sourceComponent: CaseReel { controller: root.controller }
        }
        // The odds, always on show: each tier's share of a draw, in its colour.
        Row {
            objectName: "caseOdds"
            y: 270
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 12
            scale: Math.min(1, parent.width / implicitWidth)
            Repeater {
                model: Cosmetics.tiers()
                Row {
                    required property var modelData
                    spacing: 4
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 7; height: 7; radius: 3.5
                        color: modelData.color
                    }
                    Text {
                        text: modelData.name + " " + modelData.percent + "%"
                        color: root.ink(modelData.color)
                        font.family: Theme.uiFont
                        font.pixelSize: 11
                    }
                }
            }
        }
        Text {
            objectName: "caseStatus"
            y: 290
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            textFormat: Text.StyledText
            text: root.controller.error.length ? root.controller.error
                : root.controller.state === DailyCaseController.Opening ? "Finding your moment…"
                : root.controller.state === DailyCaseController.OpenedToday && root.rewarded
                    ? "<b>" + root.reward.name + "</b> · <font color=\"" + root.ink(root.reward.rarityColor)
                      + "\">" + root.reward.rarityName + "</font> " + (root.categoryNames[root.reward.category] || "")
                      + " · See you tomorrow"
                : root.controller.state === DailyCaseController.OpenedToday ? "Today's case opened · See you tomorrow"
                : "One free case a day · Items can't be kept yet"
            color: root.controller.error.length ? Theme.warningText : Theme.textSecondary
            font.family: Theme.uiFont
            font.pixelSize: 12
            wrapMode: Text.WordWrap
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }
        Row {
            anchors.bottom: parent.bottom
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 6
            Preference {
                id: sound
                text: "Sound"
                checked: !root.controller.muted
                enabled: root.controller.state !== DailyCaseController.Opening
                onToggled: root.controller.muted = !checked
                KeyNavigation.tab: motion
            }
            Preference {
                id: motion
                text: "Less motion"
                checked: root.controller.reducedMotion
                enabled: root.controller.state !== DailyCaseController.Opening
                onToggled: root.controller.reducedMotion = checked
                KeyNavigation.tab: action.enabled ? action : closeButton
            }
        }
        Button {
            id: action
            objectName: "caseOpenButton"
            anchors.horizontalCenter: parent.horizontalCenter
            y: 320
            width: root.width < 480 ? 132 : 170
            height: 36
            enabled: root.controller.state === DailyCaseController.Available
            text: root.controller.state === DailyCaseController.Opening ? "Opening…"
                : root.controller.state === DailyCaseController.OpenedToday ? "Opened today" : "Open daily case"
            font.family: Theme.uiFont
            font.pixelSize: 13
            palette.buttonText: Theme.buttonText
            background: Rectangle {
                radius: 4
                color: action.enabled ? (action.hovered ? Theme.buttonHover : Theme.buttonBackground) : Theme.buttonDisabled
                border.color: action.activeFocus ? Theme.focusBorder : Theme.buttonBorder
            }
            onClicked: root.controller.open()
            KeyNavigation.tab: closeButton
        }
    }
}
