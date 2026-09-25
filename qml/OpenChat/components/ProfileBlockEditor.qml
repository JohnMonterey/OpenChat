import QtQuick
import QtQuick.Dialogs
import OpenChat
import OpenChat.Native

// One block of a panel in the Panels tab (docs/profile-panels.md): its kind
// and its move / duplicate / remove buttons, then the controls of its kind.
// Everything writes straight to the draft (each change one undo step, a
// text field's focus period one); the block re-reads itself only when it
// changed, so typing never rebuilds its neighbours.
Rectangle {
    id: editorBlock
    objectName: "profileBlockEditor_" + blockId
    property var profiles: null
    property int blockId: 0

    readonly property var draft: profiles ? profiles.draft : null
    readonly property var limits: profiles ? profiles.limits : ({})
    property var info: ({})
    function refresh() {
        editorBlock.info = editorBlock.draft && editorBlock.blockId > 0 ? editorBlock.draft.block(editorBlock.blockId) : {};
    }
    onBlockIdChanged: refresh()
    Component.onCompleted: refresh()
    Connections {
        target: editorBlock.draft
        function onBlockChanged(id) {
            if (id === editorBlock.blockId)
                editorBlock.refresh();
        }
        function onPanelsChanged() { editorBlock.refresh(); } // its place in the panel
        function onMediaChanged() { editorBlock.refresh(); }
    }

    readonly property int kind: info.kind || 0
    readonly property bool importingHere: profiles !== null && profiles.panelImporting
                                          && profiles.panelImportBlock === blockId
    readonly property string editTarget: info.panelId ? "panel:" + info.panelId : ""
    readonly property var kindGlyphs: ({ 1: "text", 2: "image", 3: "film", 4: "columns", 5: "minus" })

    function focusFirst() {
        const first = body.nextItemInFocusChain(true);
        if (first)
            first.forceActiveFocus(Qt.TabFocusReason);
    }

    width: parent ? parent.width : 0
    implicitHeight: content.implicitHeight + 16
    height: implicitHeight
    radius: 5
    color: Theme.contentBackground
    border.width: 1
    border.color: Theme.inputBorder

    component Label: Text {
        width: parent ? parent.width : 0
        wrapMode: Text.Wrap
        color: Theme.textSecondaryStrong
        font.family: Theme.uiFont
        font.pixelSize: 12
        renderType: Text.NativeRendering
    }
    component SmallButton: ProfileChipButton {
        compact: true
        glyphSize: 13
        width: 26
        height: 26
    }
    component Progress: Column {
        property string label: ""
        width: parent ? parent.width : 0
        spacing: 5
        Text {
            text: parent.label
            color: Theme.textPrimary
            font.family: Theme.uiFont
            font.pixelSize: 12
            renderType: Text.NativeRendering
        }
        Rectangle {
            width: parent.width
            height: 8
            radius: 4
            color: Theme.fieldBackground
            border.width: 1
            border.color: Theme.inputBorder
            Accessible.role: Accessible.ProgressBar
            Accessible.name: parent.label
            Rectangle {
                x: 1
                y: 1
                height: 6
                radius: 3
                width: (parent.width - 2) * Math.max(0.03, Math.min(1, editorBlock.profiles
                                                                    ? editorBlock.profiles.panelImportProgress : 0))
                gradient: Gradient {
                    GradientStop { position: 0; color: Theme.switchTop }
                    GradientStop { position: 1; color: Theme.switchBottom }
                }
            }
        }
    }

    FileDialog {
        id: pictureDialog
        title: "Choose pictures for this panel"
        fileMode: FileDialog.OpenFiles
        nameFilters: ["Pictures (*.jpg *.jpeg *.png *.bmp *.gif *.webp)"]
        onAccepted: editorBlock.profiles.importPanelPictures(editorBlock.blockId, selectedFiles)
    }
    FileDialog {
        id: videoDialog
        title: "Choose a video"
        nameFilters: ["Videos (*.mp4 *.m4v *.mov *.webm *.mkv *.avi *.wmv)", "All files (*)"]
        onAccepted: editorBlock.profiles.importPanelVideo(editorBlock.blockId, selectedFile)
    }
    FileDialog {
        id: coverDialog
        property int item: -1
        title: "Choose a cover picture"
        nameFilters: ["Pictures (*.jpg *.jpeg *.png *.bmp *.gif *.webp)"]
        onAccepted: editorBlock.profiles.importItemCover(editorBlock.blockId, item, selectedFile)
    }

    Column {
        id: content
        x: 8
        y: 8
        width: parent.width - 16
        spacing: 8

        // The block's head: what it is, and moving, copying or removing it.
        Item {
            width: parent.width
            height: 26
            Row {
                anchors.verticalCenter: parent.verticalCenter
                spacing: 6
                ProfileGlyph {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 15
                    height: 15
                    kind: editorBlock.kindGlyphs[editorBlock.kind] || ""
                    ink: Theme.textSecondaryStrong
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: editorBlock.info.kindName || ""
                    color: Theme.textPrimary
                    font.family: Theme.uiFont
                    font.pixelSize: 13
                    font.bold: true
                    renderType: Text.NativeRendering
                }
            }
            Row {
                anchors.right: parent.right
                spacing: 2
                SmallButton {
                    glyph: "up"
                    accessibleName: "Move block up"
                    tooltip: "Move up"
                    enabled: (editorBlock.info.index || 0) > 0
                    onClicked: editorBlock.draft.moveBlock(editorBlock.blockId, -1)
                }
                SmallButton {
                    glyph: "down"
                    accessibleName: "Move block down"
                    tooltip: "Move down"
                    enabled: (editorBlock.info.index || 0) < (editorBlock.info.count || 1) - 1
                    onClicked: editorBlock.draft.moveBlock(editorBlock.blockId, 1)
                }
                SmallButton {
                    glyph: "copy"
                    accessibleName: "Duplicate block"
                    tooltip: "Duplicate"
                    onClicked: editorBlock.draft.duplicateBlock(editorBlock.blockId)
                }
                SmallButton {
                    objectName: "profileRemoveBlock_" + editorBlock.blockId
                    glyph: "cross"
                    accessibleName: "Remove block"
                    tooltip: "Remove (Ctrl+Z brings it back)"
                    onClicked: editorBlock.draft.removeBlock(editorBlock.blockId)
                }
            }
        }

        Loader {
            id: body
            width: parent.width
            sourceComponent: {
                switch (editorBlock.kind) {
                case Profile.TextBlock: return textEditor;
                case Profile.ImageBlock: return pictureEditor;
                case Profile.VideoBlock: return videoEditor;
                case Profile.ListBlock: return listEditor;
                case Profile.DividerBlock: return dividerEditor;
                }
                return null;
            }
        }
    }

    // --- Text
    Component {
        id: textEditor
        Column {
            spacing: 6
            ProfileSegmented {
                width: parent.width
                options: ["Text", "Heading", "Quote", "Callout"]
                currentIndex: editorBlock.info.textStyle || 0
                accessibleName: "Text style"
                onActivated: index => editorBlock.draft.setBlockTextStyle(editorBlock.blockId, index)
            }
            ProfileSegmented {
                width: parent.width
                options: ["Left", "Centre", "Right"]
                currentIndex: editorBlock.info.align || 0
                accessibleName: "Alignment"
                onActivated: index => editorBlock.draft.setBlockAlign(editorBlock.blockId, index)
            }
            ProfileFieldLabel {
                width: parent.width
                readonly property bool heading: editorBlock.info.textStyle === Profile.HeadingText
                text: heading ? "Heading" : "Words"
                count: (editorBlock.info.text || "").length
                limit: heading ? editorBlock.limits.panelTitle : editorBlock.limits.blockText
            }
            ProfileTextArea {
                objectName: "profileBlockText_" + editorBlock.blockId
                width: parent.width
                multiLine: editorBlock.info.textStyle !== Profile.HeadingText
                minimumHeight: 80
                maximumLength: editorBlock.info.textStyle === Profile.HeadingText ? editorBlock.limits.panelTitle
                                                                                  : editorBlock.limits.blockText
                value: editorBlock.info.text || ""
                placeholder: "Write anything you like…"
                accessibleName: "Block text"
                profiles: editorBlock.profiles
                gestureKey: "block:" + editorBlock.blockId
                onEdited: text => editorBlock.draft.setBlockText(editorBlock.blockId, text)
            }
        }
    }

    // --- Pictures
    Component {
        id: pictureEditor
        Column {
            id: pictures
            readonly property var images: editorBlock.info.images || []
            spacing: 8
            ProfileSegmented {
                width: parent.width
                options: ["Grid", "Stack", "Strip"]
                currentIndex: editorBlock.info.gallery || 0
                accessibleName: "Picture layout"
                onActivated: index => editorBlock.draft.setBlockGallery(editorBlock.blockId, index)
            }
            ProfileSegmented {
                width: parent.width
                options: ["Plain", "Rounded", "Polaroid", "Circle"]
                currentIndex: editorBlock.info.frame === undefined ? 1 : editorBlock.info.frame
                accessibleName: "Picture frame"
                onActivated: index => editorBlock.draft.setBlockFrame(editorBlock.blockId, index)
            }
            Repeater {
                model: pictures.images.length
                delegate: Row {
                    id: pictureRow
                    required property int index
                    readonly property var entry: pictures.images[index] || ({})
                    width: pictures.width
                    spacing: 8
                    Rectangle {
                        width: 64
                        height: 64
                        radius: 4
                        color: Theme.fieldBackground
                        border.width: 1
                        border.color: Theme.inputBorder
                        ProfilePanelImage {
                            anchors.fill: parent
                            anchors.margins: 1
                            mediaKey: pictureRow.entry.mediaKey || ""
                            crop: true
                            radius: 3
                        }
                    }
                    Column {
                        width: parent.width - 72
                        spacing: 4
                        ProfileTextArea {
                            width: parent.width
                            multiLine: false
                            maximumLength: editorBlock.limits.caption
                            value: pictureRow.entry.caption || ""
                            placeholder: "Caption (optional)"
                            accessibleName: "Caption for picture " + (pictureRow.index + 1)
                            profiles: editorBlock.profiles
                            gestureKey: "caption:" + editorBlock.blockId + ":" + pictureRow.index
                            onEdited: text => editorBlock.draft.setImageCaption(editorBlock.blockId, pictureRow.index, text)
                        }
                        Row {
                            spacing: 2
                            SmallButton {
                                glyph: "back"
                                accessibleName: "Move picture earlier"
                                enabled: pictureRow.index > 0
                                onClicked: editorBlock.draft.moveImage(editorBlock.blockId, pictureRow.index, -1)
                            }
                            SmallButton {
                                glyph: "chevron"
                                accessibleName: "Move picture later"
                                enabled: pictureRow.index < pictures.images.length - 1
                                onClicked: editorBlock.draft.moveImage(editorBlock.blockId, pictureRow.index, 1)
                            }
                            SmallButton {
                                glyph: "cross"
                                accessibleName: "Remove picture"
                                onClicked: editorBlock.draft.removeImage(editorBlock.blockId, pictureRow.index)
                            }
                        }
                    }
                }
            }
            Progress {
                visible: editorBlock.importingHere
                label: editorBlock.profiles && editorBlock.profiles.panelImportQueued > 0
                       ? "Preparing pictures… (" + editorBlock.profiles.panelImportQueued + " more)"
                       : "Preparing your picture…"
            }
            ProfileEditorRail.Button {
                objectName: "profileAddPictures_" + editorBlock.blockId
                visible: !editorBlock.importingHere && editorBlock.info.canAddImage === true
                width: parent.width
                height: 30
                glyph: "image"
                label: pictures.images.length > 0 ? "Add more pictures…" : "Add pictures…"
                fontPixelSize: 13
                onClicked: pictureDialog.open()
            }
            Label {
                text: editorBlock.info.canAddImage === true
                      ? "Up to " + editorBlock.limits.maxImagesPerBlock + " pictures. Each is re-encoded to at most 1280 px."
                      : "This block is full. Add another picture block for more."
            }
        }
    }

    // --- Video
    Component {
        id: videoEditor
        Column {
            spacing: 8
            Rectangle {
                visible: editorBlock.info.hasVideo === true
                width: parent.width
                height: Math.round(width * 9 / 16)
                radius: 4
                color: "#1b1f27"
                ProfilePanelImage {
                    anchors.fill: parent
                    mediaKey: editorBlock.info.posterKey || ""
                    crop: false
                    radius: 4
                }
                Rectangle {
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.margins: 6
                    width: lengthText.implicitWidth + 10
                    height: lengthText.implicitHeight + 4
                    radius: 3
                    color: "#b0000000"
                    Text {
                        id: lengthText
                        anchors.centerIn: parent
                        text: {
                            const seconds = Math.round((editorBlock.info.durationMs || 0) / 1000);
                            return Math.floor(seconds / 60) + ":" + ("0" + seconds % 60).slice(-2);
                        }
                        color: "white"
                        font.family: Theme.uiFont
                        font.pixelSize: 11
                        renderType: Text.NativeRendering
                    }
                }
            }
            Progress {
                visible: editorBlock.importingHere
                label: "Preparing your video… this can take a little while."
            }
            Row {
                visible: !editorBlock.importingHere
                width: parent.width
                spacing: 6
                ProfileEditorRail.Button {
                    objectName: "profileChooseVideo_" + editorBlock.blockId
                    width: editorBlock.info.hasVideo === true ? (parent.width - 6) / 2 : parent.width
                    height: 30
                    glyph: "film"
                    label: editorBlock.info.hasVideo === true ? "Replace…" : "Choose video…"
                    fontPixelSize: 13
                    onClicked: videoDialog.open()
                }
                ProfileEditorRail.Button {
                    visible: editorBlock.info.hasVideo === true
                    width: (parent.width - 6) / 2
                    height: 30
                    glyph: "cross"
                    label: "Remove"
                    fontPixelSize: 13
                    onClicked: editorBlock.draft.removeVideo(editorBlock.blockId)
                }
            }
            Label {
                text: "The first 30 seconds of the video, with its sound. It plays when someone clicks it."
            }
            Item {
                width: parent.width
                height: 28
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Play on a loop"
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
                    accessibleName: "Play on a loop"
                    checked: editorBlock.info.loop === true
                    onToggled: checked => editorBlock.draft.setBlockLoop(editorBlock.blockId, checked)
                }
            }
            ProfileTextArea {
                width: parent.width
                multiLine: false
                maximumLength: editorBlock.limits.caption
                value: editorBlock.info.caption || ""
                placeholder: "Caption (optional)"
                accessibleName: "Video caption"
                profiles: editorBlock.profiles
                gestureKey: "videoCaption:" + editorBlock.blockId
                onEdited: text => editorBlock.draft.setBlockCaption(editorBlock.blockId, text)
            }
        }
    }

    // --- List
    Component {
        id: listEditor
        Column {
            id: list
            readonly property var items: editorBlock.info.items || []
            readonly property bool games: editorBlock.info.listStyle === Profile.GameList
            spacing: 8
            ProfileSegmented {
                width: parent.width
                options: ["Bullets", "Numbers", "Games", "Hearts"]
                currentIndex: editorBlock.info.listStyle || 0
                accessibleName: "List style"
                onActivated: index => editorBlock.draft.setListStyle(editorBlock.blockId, index)
            }
            Repeater {
                model: list.items.length
                delegate: Rectangle {
                    id: itemRow
                    required property int index
                    readonly property var entry: list.items[index] || ({})
                    width: list.width
                    height: itemColumn.implicitHeight + 12
                    radius: 4
                    color: Theme.fieldBackground
                    border.width: 1
                    border.color: Theme.inputBorder

                    Column {
                        id: itemColumn
                        x: 6
                        y: 6
                        width: parent.width - 12
                        spacing: 5
                        Row {
                            width: parent.width
                            spacing: 6
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                width: 18
                                text: (itemRow.index + 1) + "."
                                color: Theme.textSecondaryStrong
                                font.family: Theme.uiFont
                                font.pixelSize: 12
                                renderType: Text.NativeRendering
                            }
                            ProfileTextArea {
                                objectName: "profileItemTitle_" + editorBlock.blockId + "_" + itemRow.index
                                width: parent.width - 24 - itemButtons.width - 6
                                multiLine: false
                                maximumLength: editorBlock.limits.itemTitle
                                value: itemRow.entry.title || ""
                                placeholder: list.games ? "Game title" : "Item"
                                accessibleName: (list.games ? "Game " : "Item ") + (itemRow.index + 1)
                                profiles: editorBlock.profiles
                                gestureKey: "itemTitle:" + editorBlock.blockId + ":" + itemRow.index
                                onEdited: text => editorBlock.draft.setItemTitle(editorBlock.blockId, itemRow.index, text)
                            }
                            Row {
                                id: itemButtons
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 1
                                SmallButton {
                                    glyph: "up"
                                    accessibleName: "Move item up"
                                    enabled: itemRow.index > 0
                                    onClicked: editorBlock.draft.moveListItem(editorBlock.blockId, itemRow.index, -1)
                                }
                                SmallButton {
                                    glyph: "cross"
                                    accessibleName: "Remove item"
                                    onClicked: editorBlock.draft.removeListItem(editorBlock.blockId, itemRow.index)
                                }
                            }
                        }
                        ProfileTextArea {
                            width: parent.width
                            multiLine: false
                            maximumLength: editorBlock.limits.itemDetail
                            value: itemRow.entry.detail || ""
                            placeholder: list.games ? "Platform, hours played, a note…" : "A note (optional)"
                            accessibleName: "Note for item " + (itemRow.index + 1)
                            profiles: editorBlock.profiles
                            gestureKey: "itemDetail:" + editorBlock.blockId + ":" + itemRow.index
                            onEdited: text => editorBlock.draft.setItemDetail(editorBlock.blockId, itemRow.index, text)
                        }
                        // Stars: click one to rate, the same one again to clear.
                        Row {
                            spacing: 2
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: "Rating  "
                                color: Theme.textSecondaryStrong
                                font.family: Theme.uiFont
                                font.pixelSize: 12
                                renderType: Text.NativeRendering
                            }
                            Repeater {
                                model: 5
                                delegate: Item {
                                    required property int index
                                    width: 22
                                    height: 22
                                    activeFocusOnTab: true
                                    Accessible.role: Accessible.Button
                                    Accessible.name: (index + 1) + " stars"
                                    function rate() {
                                        const value = itemRow.entry.rating === index + 1 ? 0 : index + 1;
                                        editorBlock.draft.setItemRating(editorBlock.blockId, itemRow.index, value);
                                    }
                                    Keys.onSpacePressed: rate()
                                    Keys.onReturnPressed: rate()
                                    ProfileGlyph {
                                        anchors.centerIn: parent
                                        width: 17
                                        height: 17
                                        kind: "star"
                                        ink: index < (itemRow.entry.rating || 0) ? "#f2b01e" : Theme.inputBorder
                                    }
                                    Rectangle {
                                        visible: parent.activeFocus
                                        anchors.fill: parent
                                        radius: 3
                                        color: "transparent"
                                        border.width: 2
                                        border.color: Theme.focusBorder
                                    }
                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: parent.rate()
                                    }
                                }
                            }
                        }
                        // A game's status and cover.
                        Flow {
                            visible: list.games
                            width: parent.width
                            spacing: 4
                            Repeater {
                                model: editorBlock.profiles ? editorBlock.profiles.gameStatuses : []
                                delegate: ProfileChipButton {
                                    required property var modelData
                                    label: modelData.label
                                    fontPixelSize: 11
                                    height: 24
                                    checkable: true
                                    checked: (itemRow.entry.status || 0) === modelData.value
                                    onClicked: editorBlock.draft.setItemStatus(editorBlock.blockId, itemRow.index,
                                                                               modelData.value)
                                }
                            }
                        }
                        Row {
                            spacing: 6
                            Rectangle {
                                visible: itemRow.entry.hasCover === true
                                width: 30
                                height: 40
                                radius: 3
                                color: Theme.contentBackground
                                border.width: 1
                                border.color: Theme.inputBorder
                                ProfilePanelImage {
                                    anchors.fill: parent
                                    anchors.margins: 1
                                    mediaKey: itemRow.entry.coverKey || ""
                                    crop: true
                                    radius: 2
                                }
                            }
                            ProfileEditorRail.Button {
                                height: 26
                                glyph: "image"
                                label: itemRow.entry.hasCover === true ? "Change cover…" : (list.games ? "Add box art…" : "Add picture…")
                                fontPixelSize: 12
                                enabled: !(editorBlock.profiles && editorBlock.profiles.panelImporting)
                                onClicked: {
                                    coverDialog.item = itemRow.index;
                                    coverDialog.open();
                                }
                            }
                            ProfileEditorRail.Button {
                                visible: itemRow.entry.hasCover === true
                                height: 26
                                glyph: "cross"
                                label: "Remove"
                                fontPixelSize: 12
                                onClicked: editorBlock.draft.removeItemCover(editorBlock.blockId, itemRow.index)
                            }
                        }
                    }
                }
            }
            Progress {
                visible: editorBlock.importingHere
                label: "Preparing the picture…"
            }
            ProfileEditorRail.Button {
                objectName: "profileAddItem_" + editorBlock.blockId
                visible: editorBlock.info.canAddItem === true
                width: parent.width
                height: 30
                glyph: "plus"
                label: list.games ? "Add a game" : "Add an item"
                fontPixelSize: 13
                onClicked: editorBlock.draft.addListItem(editorBlock.blockId)
            }
        }
    }

    // --- Divider
    Component {
        id: dividerEditor
        ProfileSegmented {
            options: ["Line", "Dots", "Stars", "Hearts", "Space"]
            currentIndex: editorBlock.info.divider || 0
            accessibleName: "Divider style"
            onActivated: index => editorBlock.draft.setDivider(editorBlock.blockId, index)
        }
    }
}
