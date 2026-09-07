import QtQuick
import QtQuick.Controls.Basic
import OpenChat

// A setting inside a menu, rather than a command that dismisses the menu.
// AbstractButton retains mouse/keyboard/accessibility activation without the
// MenuItem trigger connection that closes the entire submenu hierarchy.
AbstractButton {
    id: control
    required property Menu settingsMenu
    property bool selectionOnly: false
    signal triggered()
    implicitWidth: 250
    implicitHeight: 32
    leftPadding: 28
    rightPadding: 24
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    activeFocusOnTab: visible && enabled
    checkable: !selectionOnly
    onClicked: triggered()
    Keys.onReturnPressed: {
        if (checkable) toggle();
        triggered();
    }
    Keys.onEnterPressed: {
        if (checkable) toggle();
        triggered();
    }
    Accessible.role: selectionOnly ? Accessible.RadioButton : Accessible.CheckBox
    Accessible.checkable: true
    Accessible.checked: checked

    // Menu only forwards its current index to MenuItem focus automatically.
    // Keep arrow-key navigation working for these persistent button rows too.
    Connections {
        target: control.settingsMenu
        function onCurrentIndexChanged() {
            if (control.settingsMenu.visible && control.visible
                    && control.settingsMenu.itemAt(control.settingsMenu.currentIndex) === control)
                control.forceActiveFocus(Qt.TabFocusReason);
        }
    }
    onActiveFocusChanged: {
        if (!activeFocus) return;
        for (let i = 0; i < settingsMenu.count; ++i) {
            if (settingsMenu.itemAt(i) === control) {
                settingsMenu.currentIndex = i;
                break;
            }
        }
    }
    Keys.onLeftPressed: settingsMenu.close()

    contentItem: Text {
        text: control.text
        color: control.enabled ? Theme.textPrimary : Theme.textSecondary
        font.family: Theme.uiFont
        font.pixelSize: 13
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    Text {
        x: 8
        y: (control.height - height) / 2
        text: "✓"
        visible: control.checked
        color: Theme.textPrimary
        font.pixelSize: 14
    }
    background: Rectangle {
        radius: 3
        readonly property bool highlighted: control.hovered || control.activeFocus
        color: highlighted ? Theme.navSelected : "transparent"
        border.width: highlighted ? 1 : 0
        border.color: Theme.focusBorder
    }
}
