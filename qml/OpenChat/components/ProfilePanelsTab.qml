import QtQuick
import OpenChat
import OpenChat.Native

// Panels (docs/profile-panels.md): boxes of the owner's own. "Add new panel"
// offers a blank panel and ready-made starts (text, a photo album, favourite
// games, a video, a top five); every panel then opens into its title, icon,
// colours and blocks. How much of the page's room for panel words and media
// is used shows at the top, so nothing is ever silently cut.
Item {
    id: tab
    objectName: "profilePanelsTab"

    property var profiles: null
    property Item editor: null

    readonly property var draft: profiles ? profiles.draft : null
    readonly property var panelIds: draft ? draft.panelIds : []
    property int openPanel: 0
    property bool choosing: false
    readonly property string editingTarget: {
        const focused = Window.activeFocusItem;
        for (let at = focused; at; at = at.parent) {
            if (at.editTarget !== undefined && typeof at.editTarget === "string" && at.editTarget.length > 0)
                return at.editTarget;
            if (at === tab)
                break;
        }
        return "";
    }

    // The preview asked for a panel ("panel:<id>") or for a new one ("addPanel").
    function focusField(field) {
        if (field === "addPanel") {
            tab.choosing = true;
            addButton.forceActiveFocus(Qt.OtherFocusReason);
            return;
        }
        if (field.indexOf("panel:") === 0) {
            tab.openPanel = parseInt(field.slice(6));
            Qt.callLater(() => {
                const card = findCard(tab.openPanel);
                if (card)
                    card.focusTitle();
            });
            return;
        }
        addButton.forceActiveFocus(Qt.OtherFocusReason);
    }
    function findCard(id) {
        for (let i = 0; i < cards.count; ++i) {
            const card = cards.itemAt(i);
            if (card && card.panelId === id)
                return card;
        }
        return null;
    }
    function add(template) {
        if (!tab.draft)
            return;
        const id = tab.draft.addPanel(template);
        tab.choosing = false;
        if (id > 0) {
            tab.openPanel = id;
            Qt.callLater(() => {
                const card = findCard(id);
                if (card)
                    card.focusTitle();
            });
        }
    }

    implicitHeight: column.y + column.implicitHeight + 16

    // One budget: its sentence and a thin bar, red from nine tenths.
    component Meter: Column {
        id: meter
        property string label: ""
        property real used: 0
        width: parent ? parent.width : 0
        spacing: 3
        Text {
            text: meter.label
            color: meter.used >= 0.9 ? Theme.errorText : Theme.textSecondaryStrong
            font.family: Theme.uiFont
            font.pixelSize: 11
            renderType: Text.NativeRendering
        }
        Rectangle {
            width: meter.width
            height: 5
            radius: 2.5
            color: Theme.fieldBackground
            border.width: 1
            border.color: Theme.inputBorder
            Accessible.role: Accessible.ProgressBar
            Accessible.name: meter.label
            Rectangle {
                height: parent.height
                radius: 2.5
                width: parent.width * Math.max(0, Math.min(1, meter.used))
                color: meter.used >= 0.9 ? Theme.errorText : Theme.accentBlue
            }
        }
    }

    Column {
        id: column
        x: 16
        y: 14
        width: tab.width - 32
        spacing: 12

        Text {
            width: parent.width
            wrapMode: Text.Wrap
            text: "Add boxes of your own: words, pictures, a video, your favorite games, anything."
            color: Theme.textSecondaryStrong
            font.family: Theme.uiFont
            font.pixelSize: 12
            renderType: Text.NativeRendering
        }

        ProfileEditorRail.Button {
            id: addButton
            objectName: "profileAddPanelButton"
            width: parent.width
            height: 34
            glyph: "plus"
            label: tab.draft && !tab.draft.canAddPanel ? "Your page has all the panels it can hold" : "Add new panel"
            fontPixelSize: 14
            enabled: tab.draft !== null && tab.draft.canAddPanel
            onClicked: tab.choosing = !tab.choosing
        }

        // The starting points.
        Column {
            visible: tab.choosing
            width: parent.width
            spacing: 6
            Repeater {
                model: tab.profiles ? tab.profiles.panelTemplates : []
                delegate: Rectangle {
                    id: choice
                    required property var modelData
                    objectName: "profilePanelTemplate_" + modelData.value
                    width: column.width
                    height: 52
                    radius: 5
                    color: choiceMouse.containsMouse ? Theme.buttonHover : Theme.buttonBackground
                    border.width: activeFocus ? 2 : 1
                    border.color: activeFocus ? Theme.focusBorder : Theme.buttonBorder
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: modelData.label + ". " + modelData.blurb
                    Keys.onReturnPressed: tab.add(modelData.value)
                    Keys.onSpacePressed: tab.add(modelData.value)
                    ProfileGlyph {
                        x: 12
                        anchors.verticalCenter: parent.verticalCenter
                        width: 22
                        height: 22
                        kind: choice.modelData.glyph
                        ink: Theme.textPrimary
                    }
                    Column {
                        x: 46
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - 56
                        spacing: 1
                        Text {
                            text: choice.modelData.label
                            color: Theme.textPrimary
                            font.family: Theme.uiFont
                            font.pixelSize: 13
                            font.bold: true
                            renderType: Text.NativeRendering
                        }
                        Text {
                            width: parent.width
                            elide: Text.ElideRight
                            text: choice.modelData.blurb
                            color: Theme.textSecondaryStrong
                            font.family: Theme.uiFont
                            font.pixelSize: 11
                            renderType: Text.NativeRendering
                        }
                    }
                    MouseArea {
                        id: choiceMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: tab.add(choice.modelData.value)
                    }
                }
            }
        }

        // The owner's panels, newest last, as on the page.
        Repeater {
            id: cards
            model: tab.panelIds
            delegate: ProfilePanelEditor {
                required property var modelData
                profiles: tab.profiles
                editor: tab.editor
                panelId: modelData
                expanded: tab.openPanel === modelData
                onExpandedChanged: {
                    if (expanded)
                        tab.openPanel = panelId;
                    else if (tab.openPanel === panelId)
                        tab.openPanel = 0;
                }
                onRemoved: addButton.forceActiveFocus(Qt.OtherFocusReason)
            }
        }
        Text {
            visible: tab.panelIds.length === 0 && !tab.choosing
            width: parent.width
            wrapMode: Text.Wrap
            horizontalAlignment: Text.AlignHCenter
            text: "No panels yet."
            color: Theme.textSecondaryStrong
            font.family: Theme.uiFont
            font.pixelSize: 12
            font.italic: true
            renderType: Text.NativeRendering
        }

        // How full the panels are.
        Column {
            visible: tab.panelIds.length > 0
            width: parent.width
            spacing: 6
            Meter {
                readonly property real share: tab.draft ? tab.draft.panelTextUsed : 0
                label: "Room for words in panels: " + (share > 0 && share < 0.01 ? "under 1" : Math.round(share * 100))
                       + "% used"
                used: tab.draft ? tab.draft.panelTextUsed : 0
            }
            Meter {
                label: "Room for pictures and video: " + (tab.draft ? tab.draft.panelMediaCount : 0) + " of "
                       + (tab.profiles ? tab.profiles.limits.maxPanelMedia : 24) + " pieces, "
                       + Math.round((tab.draft ? tab.draft.panelMediaUsed : 0) * 100) + "% used"
                used: tab.draft ? tab.draft.panelMediaUsed : 0
            }
        }
    }
}
