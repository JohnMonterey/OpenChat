import QtQuick
import OpenChat

// The editor's segmented control (SPEC §14.3): joined segments in a 28 px
// strip with radius 4, the chosen one wearing the switch's blue gradient with
// a white label. One Tab stop, the chosen segment; ←/→ move the choice at once
// (a segment is a radio button, and the keyboard focus moves with the choice,
// so a screen reader announces each), Home/End jump to the ends. `activated`
// reports a choice made here; `currentIndex` follows the model, so a refused
// choice snaps back.
FocusScope {
    id: segmented

    property var options: []
    property int currentIndex: -1
    property int fontPixelSize: 13
    property string accessibleName: ""
    readonly property int tabIndex: Math.max(0, Math.min(options.length - 1, currentIndex))
    signal activated(int index)

    function choose(index) {
        if (index >= 0 && index < options.length && index !== currentIndex)
            activated(index);
    }
    // The focus sits on the chosen segment while the control has it.
    function focusChosen() {
        const part = segments.itemAt(tabIndex);
        if (part && segmented.activeFocus && !part.activeFocus)
            part.forceActiveFocus(Qt.TabFocusReason);
    }

    implicitWidth: 240
    implicitHeight: 28
    height: implicitHeight
    opacity: enabled ? 1 : 0.55
    onActiveFocusChanged: if (activeFocus) focusChosen()
    onTabIndexChanged: focusChosen()
    Accessible.role: Accessible.Grouping
    Accessible.name: accessibleName
    Keys.onLeftPressed: choose(Math.max(0, currentIndex - 1))
    Keys.onRightPressed: choose(Math.min(options.length - 1, currentIndex + 1))
    Keys.onPressed: event => {
        if (event.key === Qt.Key_Home) {
            choose(0);
            event.accepted = true;
        } else if (event.key === Qt.Key_End) {
            choose(options.length - 1);
            event.accepted = true;
        }
    }

    Rectangle {
        anchors.fill: parent
        radius: 4
        color: Theme.buttonBackground
        border.width: segmented.activeFocus ? 2 : 1
        border.color: segmented.activeFocus ? Theme.focusBorder : Theme.buttonBorder
    }

    Row {
        anchors.fill: parent

        Repeater {
            id: segments
            model: segmented.options

            Item {
                id: part
                required property string modelData
                required property int index
                readonly property bool chosen: index === segmented.currentIndex
                objectName: "profileSegment_" + modelData
                // The focused one stays a Tab stop until the focus has left it.
                activeFocusOnTab: index === segmented.tabIndex || activeFocus
                width: segmented.options.length > 0 ? segmented.width / segmented.options.length : 0
                height: segmented.height
                Accessible.role: Accessible.RadioButton
                Accessible.name: modelData
                Accessible.checkable: true
                Accessible.checked: chosen
                Accessible.onPressAction: segmented.choose(index)

                Rectangle {
                    visible: part.chosen
                    anchors.fill: parent
                    anchors.margins: 1
                    radius: 3
                    gradient: Gradient {
                        GradientStop { position: 0; color: Theme.switchTop }
                        GradientStop { position: 1; color: Theme.switchBottom }
                    }
                    Rectangle { x: 2; width: parent.width - 4; height: 1; color: "#60ffffff" }
                }
                Rectangle {
                    visible: !part.chosen && partMouse.containsMouse && segmented.enabled
                    anchors.fill: parent
                    anchors.margins: 1
                    radius: 3
                    color: Theme.buttonHover
                }
                // Dividers between unchosen neighbours only.
                Rectangle {
                    visible: part.index > 0 && !part.chosen && part.index !== segmented.currentIndex + 1
                    y: 5
                    width: 1
                    height: parent.height - 10
                    color: Theme.buttonBorder
                }
                Text {
                    anchors.centerIn: parent
                    width: Math.min(implicitWidth, parent.width - 4)
                    horizontalAlignment: Text.AlignHCenter
                    elide: Text.ElideRight
                    text: part.modelData
                    color: part.chosen ? Theme.onAccentText : Theme.buttonText
                    font.family: Theme.uiFont
                    font.pixelSize: segmented.fontPixelSize
                    renderType: Text.NativeRendering
                }
                MouseArea {
                    id: partMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: segmented.choose(part.index)
                }
            }
        }
    }
}
