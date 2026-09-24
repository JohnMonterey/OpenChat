import QtQuick
import QtQuick.Controls.Basic
import OpenChat
import OpenChat.Native

// One block of a custom panel (docs/profile-panels.md), drawn in the
// panel's render: text in four styles, pictures in a grid, a stack or a
// sideways strip with four frames, a video, a list (bullets, numbers,
// hearts, or games with covers, a status and stars) or a divider. Nothing
// here is markup: every string is plain text. Pictures open in the page's
// lightbox; in the editor's preview a click opens the block's editor.
Item {
    id: block
    property var view: null
    property Item panel: null
    property int blockId: 0

    readonly property var page: view ? view.page : null
    readonly property bool preview: view ? view.preview : false
    readonly property var render: panel ? panel.render : null
    property var data: ({})
    function refresh() {
        block.data = block.page && block.blockId > 0 ? block.page.block(block.blockId) : {};
    }
    onBlockIdChanged: refresh()
    Component.onCompleted: refresh()
    Connections {
        target: block.page
        function onBlockChanged(id) {
            if (id === block.blockId)
                block.refresh();
        }
    }

    readonly property int kind: block.data.kind || 0
    readonly property bool filled: block.data.hasContent === true
    // A divider shows between content; an empty block only in the preview.
    visible: block.kind === Profile.DividerBlock || block.filled || block.preview
    implicitHeight: visible && loader.item ? loader.item.implicitHeight : 0
    height: implicitHeight

    readonly property color bodyInk: render ? render.bodyColor : "black"
    readonly property color mutedInk: render ? render.mutedColor : "gray"
    readonly property color linkInk: render ? render.linkColor : "#1f6fa3"
    readonly property color subheadInk: render ? render.blurbSubheadColor : "black"
    readonly property string bodyFamily: render && render.bodyFamily.length > 0 ? render.bodyFamily : Theme.uiFont
    readonly property string headingFamily: render && render.headingFamily.length > 0 ? render.headingFamily
                                                                                      : Theme.uiFont
    readonly property int bodySize: render ? render.bodyPixelSize : 13
    readonly property int captionSize: render ? render.captionPixelSize : 11
    readonly property bool halo: render ? render.textHalo : false
    readonly property color haloColor: render ? render.haloColor : "transparent"
    readonly property color ruleColor: render ? render.tableRuleColor : "#cccccc"

    function openPicture(index) {
        if (block.preview) {
            block.view.editRequested("panel:" + block.data.panelId);
            return;
        }
        if (block.view && typeof block.view.openLightbox === "function")
            block.view.openLightbox(block.data.images || [], index);
    }

    component BodyText: Text {
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
        color: block.bodyInk
        style: block.halo ? Text.Outline : Text.Normal
        styleColor: block.haloColor
        font.family: block.bodyFamily
        font.pixelSize: block.bodySize
        lineHeight: 1.18
        renderType: Text.NativeRendering
    }
    component Hint: Text {
        width: parent ? parent.width : 0
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
        color: block.mutedInk
        font.family: Theme.uiFont
        font.pixelSize: block.captionSize + 1
        font.italic: true
        renderType: Text.NativeRendering
    }
    component Caption: Text {
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
        horizontalAlignment: Text.AlignHCenter
        color: block.mutedInk
        style: block.halo ? Text.Outline : Text.Normal
        styleColor: block.haloColor
        font.family: block.bodyFamily
        font.pixelSize: block.captionSize + 1
        renderType: Text.NativeRendering
    }
    // A picture not here yet: it is on its way from its owner.
    component Waiting: Rectangle {
        color: Qt.rgba(block.mutedInk.r, block.mutedInk.g, block.mutedInk.b, 0.12)
        border.width: 1
        border.color: Qt.rgba(block.mutedInk.r, block.mutedInk.g, block.mutedInk.b, 0.3)
        ProfileGlyph {
            anchors.centerIn: parent
            width: Math.min(28, parent.width / 3)
            height: width
            kind: "image"
            ink: block.mutedInk
            opacity: 0.7
        }
    }

    Loader {
        id: loader
        width: parent.width
        sourceComponent: {
            switch (block.kind) {
            case Profile.TextBlock: return textBlock;
            case Profile.ImageBlock: return pictureBlock;
            case Profile.VideoBlock: return videoBlock;
            case Profile.ListBlock: return listBlock;
            case Profile.DividerBlock: return dividerBlock;
            }
            return null;
        }
    }

    // --- Text
    Component {
        id: textBlock
        Item {
            readonly property int style: block.data.textStyle || 0
            readonly property int align: block.data.align === Profile.CenterAlign ? Text.AlignHCenter
                                       : block.data.align === Profile.EndAlign ? Text.AlignRight : Text.AlignLeft
            readonly property string words: block.data.text || ""
            implicitHeight: words.length === 0 ? hint.implicitHeight
                            : style === Profile.CalloutText ? callout.height
                            : style === Profile.QuoteText ? quote.height : plain.implicitHeight

            Hint {
                id: hint
                visible: parent.words.length === 0
                text: "Write something here."
            }
            BodyText {
                id: plain
                objectName: "profilePanelText_" + block.blockId
                visible: parent.words.length > 0 && (parent.style === Profile.ParagraphText
                                                     || parent.style === Profile.HeadingText)
                width: parent.width
                horizontalAlignment: parent.align
                text: parent.words
                color: parent.style === Profile.HeadingText ? block.subheadInk : block.bodyInk
                font.family: parent.style === Profile.HeadingText ? block.headingFamily : block.bodyFamily
                font.pixelSize: parent.style === Profile.HeadingText
                                ? (block.render ? block.render.subheadPixelSize + 3 : 17) : block.bodySize
                font.bold: parent.style === Profile.HeadingText && (block.render ? block.render.headingBold : true)
            }
            Item {
                id: quote
                visible: parent.words.length > 0 && parent.style === Profile.QuoteText
                width: parent.width
                height: quoteText.implicitHeight + 8
                Rectangle {
                    width: 3
                    height: parent.height
                    radius: 1.5
                    color: block.linkInk
                }
                BodyText {
                    id: quoteText
                    x: 14
                    y: 4
                    width: parent.width - 14
                    horizontalAlignment: quote.parent.align
                    text: quote.parent.words
                    font.italic: true
                    font.pixelSize: block.bodySize + 1
                }
            }
            Rectangle {
                id: callout
                visible: parent.words.length > 0 && parent.style === Profile.CalloutText
                width: parent.width
                height: calloutText.implicitHeight + 20
                radius: 6
                color: block.render ? block.render.cellLabelFill : "#eef4fb"
                border.width: 1
                border.color: block.ruleColor
                BodyText {
                    id: calloutText
                    x: 12
                    y: 10
                    width: parent.width - 24
                    horizontalAlignment: callout.parent.align
                    text: callout.parent.words
                    color: block.render ? block.render.cellLabelInk : "black"
                    style: Text.Normal
                    font.bold: true
                }
            }
        }
    }

    // --- Pictures
    Component {
        id: pictureBlock
        Column {
            id: pictures
            readonly property var images: block.data.images || []
            readonly property int gallery: block.data.gallery || 0
            readonly property int frame: block.data.frame === undefined ? Profile.RoundedFrame : block.data.frame
            readonly property int columns: images.length <= 1 ? 1 : images.length === 2 || images.length === 4 ? 2 : 3
            readonly property real gap: 8
            spacing: 6

            Hint {
                visible: pictures.images.length === 0
                text: "Add pictures in the editor."
            }

            // Grid and stack.
            Flow {
                visible: pictures.images.length > 0 && pictures.gallery !== Profile.StripGallery
                width: parent.width
                spacing: pictures.gap
                Repeater {
                    model: pictures.gallery === Profile.StripGallery ? [] : pictures.images
                    delegate: ProfilePanelPicture {
                        required property var modelData
                        required property int index
                        entry: modelData
                        frame: pictures.frame
                        stack: pictures.gallery === Profile.StackGallery
                        tileWidth: stack ? pictures.width
                                   : (pictures.width - (pictures.columns - 1) * pictures.gap) / pictures.columns
                        single: pictures.images.length === 1
                        tilt: pictures.frame === Profile.PolaroidFrame ? (index % 2 === 0 ? -1.4 : 1.2) : 0
                        panelBlock: block
                        onActivated: block.openPicture(index)
                    }
                }
            }

            // Strip: one row, scrolled sideways.
            Flickable {
                visible: pictures.images.length > 0 && pictures.gallery === Profile.StripGallery
                width: parent.width
                height: visible ? stripRow.height : 0
                contentWidth: stripRow.width
                contentHeight: stripRow.height
                flickableDirection: Flickable.HorizontalFlick
                boundsBehavior: Flickable.StopAtBounds
                clip: true
                Row {
                    id: stripRow
                    spacing: pictures.gap
                    Repeater {
                        model: pictures.gallery === Profile.StripGallery ? pictures.images : []
                        delegate: ProfilePanelPicture {
                            required property var modelData
                            required property int index
                            entry: modelData
                            frame: pictures.frame
                            strip: true
                            tileWidth: Math.round(150 * Math.max(0.6, Math.min(1.8, (modelData.width || 1)
                                                                              / Math.max(1, modelData.height || 1))))
                            panelBlock: block
                            onActivated: block.openPicture(index)
                        }
                    }
                }
            }
        }
    }

    // --- Video
    Component {
        id: videoBlock
        Column {
            spacing: 6
            Hint {
                visible: block.data.hasVideo !== true
                text: "Add a video in the editor."
            }
            ProfilePanelVideo {
                visible: block.data.hasVideo === true
                width: parent.width
                entry: block.data
                panelBlock: block
            }
            Caption {
                visible: (block.data.caption || "").length > 0
                width: parent.width
                text: block.data.caption || ""
            }
        }
    }

    // --- List
    Component {
        id: listBlock
        Column {
            id: list
            readonly property var items: block.data.items || []
            readonly property int listStyle: block.data.listStyle || 0
            readonly property bool games: listStyle === Profile.GameList
            spacing: games ? 8 : 5

            Hint {
                visible: !block.filled
                text: list.games ? "Add your favorite games in the editor." : "Add items in the editor."
            }
            Repeater {
                model: list.items
                delegate: Item {
                    id: row
                    required property var modelData
                    required property int index
                    // Numbers count the items a viewer sees.
                    readonly property int number: {
                        let n = 0;
                        for (let i = 0; i <= index; ++i) {
                            if (list.items[i].filled)
                                ++n;
                        }
                        return n;
                    }
                    visible: modelData.filled
                    width: list.width
                    height: visible ? Math.max(marker.height, texts.height, cover.visible ? cover.height : 0) : 0

                    // The marker: a bullet, a number, a heart, or a game's cover.
                    Item {
                        id: marker
                        visible: !list.games
                        width: list.listStyle === Profile.NumberedList ? 26 : 16
                        height: block.bodySize + 6
                        Rectangle {
                            visible: list.listStyle === Profile.BulletList
                            anchors.centerIn: parent
                            width: 6
                            height: 6
                            radius: 3
                            color: block.linkInk
                        }
                        Text {
                            visible: list.listStyle === Profile.NumberedList
                            anchors.verticalCenter: parent.verticalCenter
                            text: row.number + "."
                            color: block.linkInk
                            font.family: block.headingFamily
                            font.pixelSize: block.bodySize + 1
                            font.bold: true
                            renderType: Text.NativeRendering
                        }
                        ProfileGlyph {
                            visible: list.listStyle === Profile.HeartList
                            anchors.centerIn: parent
                            width: 13
                            height: 13
                            kind: "heart"
                            ink: block.linkInk
                        }
                    }
                    Rectangle {
                        id: cover
                        visible: list.games
                        width: 46
                        height: 60
                        radius: 4
                        color: Qt.rgba(block.mutedInk.r, block.mutedInk.g, block.mutedInk.b, 0.12)
                        border.width: 1
                        border.color: Qt.rgba(block.mutedInk.r, block.mutedInk.g, block.mutedInk.b, 0.3)
                        ProfileGlyph {
                            visible: !coverImage.ready
                            anchors.centerIn: parent
                            width: 24
                            height: 24
                            kind: "gamepad"
                            ink: block.mutedInk
                        }
                        ProfilePanelImage {
                            id: coverImage
                            anchors.fill: parent
                            anchors.margins: 1
                            mediaKey: row.modelData.coverKey
                            crop: true
                            radius: 3
                        }
                    }
                    Column {
                        id: texts
                        x: list.games ? cover.width + 10 : marker.width + 4
                        width: parent.width - x
                        spacing: 2
                        Row {
                            width: parent.width
                            spacing: 8
                            BodyText {
                                id: itemTitle
                                width: Math.min(implicitWidth, parent.width - (stars.visible ? stars.width + 8 : 0))
                                text: row.modelData.title
                                font.bold: list.games
                                font.pixelSize: list.games ? block.bodySize + 1 : block.bodySize
                            }
                            Row {
                                id: stars
                                visible: row.modelData.rating > 0
                                anchors.verticalCenter: itemTitle.lineCount === 1 ? itemTitle.verticalCenter : undefined
                                spacing: 1
                                Repeater {
                                    model: row.modelData.rating > 0 ? 5 : 0
                                    ProfileGlyph {
                                        required property int index
                                        width: 12
                                        height: 12
                                        kind: "star"
                                        ink: index < row.modelData.rating ? "#f2b01e" : block.mutedInk
                                        opacity: index < row.modelData.rating ? 1 : 0.35
                                    }
                                }
                                Accessible.role: Accessible.StaticText
                                Accessible.name: row.modelData.rating + " out of 5 stars"
                            }
                        }
                        Text {
                            visible: row.modelData.detail.length > 0
                            width: parent.width
                            wrapMode: Text.Wrap
                            textFormat: Text.PlainText
                            text: row.modelData.detail
                            color: block.mutedInk
                            font.family: block.bodyFamily
                            font.pixelSize: block.bodySize - 1
                            renderType: Text.NativeRendering
                        }
                        Rectangle {
                            visible: list.games && row.modelData.statusName.length > 0
                            width: statusText.implicitWidth + 12
                            height: statusText.implicitHeight + 4
                            radius: height / 2
                            color: Qt.rgba(block.linkInk.r, block.linkInk.g, block.linkInk.b, 0.14)
                            border.width: 1
                            border.color: Qt.rgba(block.linkInk.r, block.linkInk.g, block.linkInk.b, 0.45)
                            Text {
                                id: statusText
                                anchors.centerIn: parent
                                text: row.modelData.statusName
                                color: block.linkInk
                                font.family: Theme.uiFont
                                font.pixelSize: block.captionSize
                                font.bold: true
                                renderType: Text.NativeRendering
                            }
                        }
                    }
                }
            }
        }
    }

    // --- Divider
    Component {
        id: dividerBlock
        Item {
            readonly property int style: block.data.divider || 0
            implicitHeight: style === Profile.SpaceDivider ? 14 : 16
            Rectangle {
                visible: parent.style === Profile.LineDivider
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width
                height: 1
                color: block.ruleColor
            }
            Row {
                visible: parent.style === Profile.DotsDivider || parent.style === Profile.StarsDivider
                         || parent.style === Profile.HeartsDivider
                anchors.centerIn: parent
                spacing: parent.style === Profile.DotsDivider ? 8 : 10
                Repeater {
                    model: parent.visible ? (parent.parent.style === Profile.DotsDivider ? 9 : 5) : 0
                    Item {
                        width: 12
                        height: 12
                        Rectangle {
                            visible: dividerKind() === Profile.DotsDivider
                            anchors.centerIn: parent
                            width: 4
                            height: 4
                            radius: 2
                            color: block.mutedInk
                        }
                        ProfileGlyph {
                            visible: dividerKind() !== Profile.DotsDivider
                            anchors.fill: parent
                            kind: dividerKind() === Profile.HeartsDivider ? "heart" : "star"
                            ink: block.linkInk
                            opacity: 0.8
                        }
                        function dividerKind() { return block.data.divider || 0; }
                    }
                }
            }
            Hint {
                visible: block.preview && parent.style === Profile.SpaceDivider
                anchors.verticalCenter: parent.verticalCenter
                horizontalAlignment: Text.AlignHCenter
                text: "(space)"
                opacity: 0.6
            }
        }
    }
}
