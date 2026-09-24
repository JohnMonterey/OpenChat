import QtQuick
import OpenChat
import OpenChat.Native

// One of the owner's panels in the Panels tab: a card that opens into the
// panel's title, icon, colours and blocks, and closes to one line. Adding a
// block, duplicating or deleting the panel are here too; where it sits on
// the page is the Layout tab's.
Rectangle {
    id: card
    objectName: "profilePanelEditor_" + panelId
    property var profiles: null
    property Item editor: null
    property int panelId: 0
    property bool expanded: false
    signal removed()

    readonly property var draft: profiles ? profiles.draft : null
    readonly property var limits: profiles ? profiles.limits : ({})
    readonly property string editTarget: "panel:" + panelId
    property var info: ({})
    property var blockIds: []
    function refresh() {
        if (!card.draft || card.panelId <= 0)
            return;
        card.info = card.draft.panel(card.panelId);
        const ids = card.info.blockIds || [];
        if (ids.length !== card.blockIds.length || ids.some((id, i) => id !== card.blockIds[i]))
            card.blockIds = ids;
    }
    onPanelIdChanged: refresh()
    Component.onCompleted: refresh()
    Connections {
        target: card.draft
        function onPanelChanged(id) {
            if (id === card.panelId)
                card.refresh();
        }
        function onPanelsChanged() { card.refresh(); }
    }
    function focusTitle() {
        card.expanded = true;
        title.focusInput();
    }

    width: parent ? parent.width : 0
    implicitHeight: column.implicitHeight + 16
    height: implicitHeight
    radius: 6
    color: Theme.fieldBackground
    border.width: 1
    border.color: card.expanded ? Theme.focusBorder : Theme.inputBorder

    component SectionLabel: Text {
        color: Theme.textPrimary
        font.family: Theme.uiFont
        font.pixelSize: 13
        font.bold: true
        renderType: Text.NativeRendering
    }

    Column {
        id: column
        x: 8
        y: 8
        width: parent.width - 16
        spacing: 10

        // The card's head: its icon and title; a click opens or closes it.
        Item {
            width: parent.width
            height: 30
            activeFocusOnTab: true
            Accessible.role: Accessible.Button
            Accessible.name: (card.info.shownTitle || "Panel") + (card.expanded ? ", open" : ", closed")
            Keys.onReturnPressed: card.expanded = !card.expanded
            Keys.onSpacePressed: card.expanded = !card.expanded
            Rectangle {
                visible: parent.activeFocus
                anchors.fill: parent
                radius: 4
                color: "transparent"
                border.width: 2
                border.color: Theme.focusBorder
            }
            Row {
                x: 4
                anchors.verticalCenter: parent.verticalCenter
                spacing: 8
                ProfileGlyph {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 18
                    height: 18
                    kind: card.info.glyph || "boxes"
                    ink: Theme.textSecondaryStrong
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    width: card.width - 110
                    elide: Text.ElideRight
                    text: card.info.shownTitle || ""
                    color: Theme.textPrimary
                    font.family: Theme.uiFont
                    font.pixelSize: 14
                    font.bold: true
                    renderType: Text.NativeRendering
                }
            }
            Text {
                anchors.right: chevron.left
                anchors.rightMargin: 6
                anchors.verticalCenter: parent.verticalCenter
                visible: card.info.hasContent !== true
                text: "empty"
                color: Theme.textSecondaryStrong
                font.family: Theme.uiFont
                font.pixelSize: 11
                font.italic: true
                renderType: Text.NativeRendering
            }
            ProfileGlyph {
                id: chevron
                anchors.right: parent.right
                anchors.rightMargin: 4
                anchors.verticalCenter: parent.verticalCenter
                width: 14
                height: 14
                kind: card.expanded ? "up" : "down"
                ink: Theme.textSecondaryStrong
            }
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: card.expanded = !card.expanded
            }
        }

        Column {
            visible: card.expanded
            width: parent.width
            spacing: 10

            // Title
            ProfileFieldLabel {
                width: parent.width
                text: "Title"
                count: (card.info.title || "").length
                limit: card.limits.panelTitle
            }
            ProfileTextArea {
                id: title
                objectName: "profilePanelTitle_" + card.panelId
                width: parent.width
                multiLine: false
                maximumLength: card.limits.panelTitle
                value: card.info.title || ""
                placeholder: "Give your panel a name"
                accessibleName: "Panel title"
                profiles: card.profiles
                gestureKey: "panelTitle:" + card.panelId
                onEdited: text => card.draft.setPanelTitle(card.panelId, text)
            }
            Item {
                width: parent.width
                height: 28
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Show the title strip"
                    color: Theme.textPrimary
                    font.family: Theme.uiFont
                    font.pixelSize: 13
                    renderType: Text.NativeRendering
                }
                AeroSwitch {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    width: 46
                    height: 26
                    accessibleName: "Show the title strip"
                    checked: card.info.showTitle !== false
                    onToggled: checked => card.draft.setPanelShowTitle(card.panelId, checked)
                }
            }

            // Icon
            SectionLabel { text: "Icon" }
            Flow {
                width: parent.width
                spacing: 4
                Repeater {
                    model: card.profiles ? card.profiles.panelIcons : []
                    delegate: ProfileChipButton {
                        required property var modelData
                        compact: true
                        width: 34
                        height: 30
                        glyph: modelData.glyph
                        label: modelData.glyph.length > 0 ? "" : "∅"
                        accessibleName: "Icon: " + modelData.label
                        tooltip: modelData.label
                        checkable: true
                        checked: (card.info.icon || 0) === modelData.value
                        onClicked: card.draft.setPanelIcon(card.panelId, modelData.value)
                    }
                }
            }

            // Colours
            Item {
                width: parent.width
                height: 28
                SectionLabel {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Its own colours"
                }
                AeroSwitch {
                    objectName: "profilePanelOwnColours_" + card.panelId
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    width: 46
                    height: 26
                    accessibleName: "Give this panel its own colours"
                    checked: card.info.ownColours === true
                    onToggled: checked => card.draft.setPanelOwnColours(card.panelId, checked)
                }
            }
            Text {
                visible: card.info.ownColours !== true
                width: parent.width
                wrapMode: Text.Wrap
                text: "Off: the panel wears your page's box style, like the other boxes."
                color: Theme.textSecondaryStrong
                font.family: Theme.uiFont
                font.pixelSize: 12
                renderType: Text.NativeRendering
            }
            Grid {
                visible: card.info.ownColours === true
                width: parent.width
                columns: 2
                columnSpacing: 8
                rowSpacing: 8
                readonly property real half: (width - 8) / 2
                ProfileColorWell {
                    width: parent.half
                    label: "Strip colour"
                    color: card.info.headerFill || "white"
                    editor: card.editor
                    page: card.draft
                    onPicked: colour => card.draft.setPanelColour(card.panelId, "headerFill", colour)
                }
                ProfileColorWell {
                    width: parent.half
                    label: "Strip text"
                    color: card.info.headerText || "black"
                    editor: card.editor
                    page: card.draft
                    onPicked: colour => card.draft.setPanelColour(card.panelId, "headerText", colour)
                }
                ProfileColorWell {
                    width: parent.half
                    label: "Box colour"
                    color: card.info.boxFill || "white"
                    editor: card.editor
                    page: card.draft
                    onPicked: colour => card.draft.setPanelColour(card.panelId, "boxFill", colour)
                }
                ProfileColorWell {
                    width: parent.half
                    label: "Text colour"
                    color: card.info.bodyInk || "black"
                    editor: card.editor
                    page: card.draft
                    onPicked: colour => card.draft.setPanelColour(card.panelId, "bodyInk", colour)
                }
            }

            // Blocks
            SectionLabel { text: "In this panel" }
            Repeater {
                model: card.blockIds
                delegate: ProfileBlockEditor {
                    required property var modelData
                    profiles: card.profiles
                    blockId: modelData
                }
            }
            Text {
                visible: card.blockIds.length === 0
                width: parent.width
                wrapMode: Text.Wrap
                text: "Nothing yet. Add text, pictures, a list or more below."
                color: Theme.textSecondaryStrong
                font.family: Theme.uiFont
                font.pixelSize: 12
                font.italic: true
                renderType: Text.NativeRendering
            }
            Text {
                text: "Add"
                color: Theme.textSecondaryStrong
                font.family: Theme.uiFont
                font.pixelSize: 12
                renderType: Text.NativeRendering
            }
            Flow {
                width: parent.width
                spacing: 4
                Repeater {
                    model: [
                        { kind: Profile.TextBlock, label: "Text", glyph: "text" },
                        { kind: Profile.ImageBlock, label: "Pictures", glyph: "image" },
                        { kind: Profile.VideoBlock, label: "Video", glyph: "film" },
                        { kind: Profile.ListBlock, label: "List", glyph: "columns" },
                        { kind: Profile.DividerBlock, label: "Divider", glyph: "minus" }
                    ].filter(entry => entry.kind !== Profile.VideoBlock
                                      || (card.profiles && card.profiles.videoSupported))
                    delegate: ProfileChipButton {
                        required property var modelData
                        objectName: "profileAddBlock_" + modelData.label
                        glyph: modelData.glyph
                        label: modelData.label
                        fontPixelSize: 12
                        height: 28
                        enabled: card.info.canAddBlock === true
                        accessibleName: "Add " + modelData.label.toLowerCase() + " block"
                        onClicked: card.draft.addBlock(card.panelId, modelData.kind, 0)
                    }
                }
            }

            // The panel itself.
            Rectangle { width: parent.width; height: 1; color: Theme.rule }
            Row {
                spacing: 6
                ProfileEditorRail.Button {
                    height: 28
                    glyph: "copy"
                    label: "Duplicate panel"
                    fontPixelSize: 12
                    enabled: card.draft !== null && card.draft.canAddPanel
                    onClicked: card.draft.duplicatePanel(card.panelId)
                }
                ProfileEditorRail.Button {
                    objectName: "profileDeletePanel_" + card.panelId
                    height: 28
                    glyph: "cross"
                    label: "Delete panel"
                    fontPixelSize: 12
                    labelColor: Theme.errorText
                    onClicked: {
                        card.draft.removePanel(card.panelId);
                        card.removed();
                    }
                }
            }
            Text {
                width: parent.width
                wrapMode: Text.Wrap
                text: "Move it or hide it in Layout. Ctrl+Z undoes anything here."
                color: Theme.textSecondaryStrong
                font.family: Theme.uiFont
                font.pixelSize: 11
                renderType: Text.NativeRendering
            }
        }
    }
}
