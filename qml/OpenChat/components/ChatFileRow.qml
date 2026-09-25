import QtQuick
import QtQuick.Shapes
import OpenChat

// A file in its bubble: a page in the file colour with its corner turned down
// and its type on it ("PDF"), the name (elided in the middle, so the type at
// its end still shows) and its size, with Save… once it is all here. While it
// travels the line under the name says how far it has come and a thin bar
// fills; when it stopped for good the line says why.
Item {
    id: file
    objectName: "chatFileRow"
    required property MessageDelegate row
    readonly property bool complete: row.transferState === 1
    readonly property bool skinned: row.bubbleItem.skinned
    readonly property color ink: skinned ? row.bubbleItem.skinTextColor : Theme.textPrimary
    readonly property color secondaryInk: skinned ? row.bubbleItem.skinSecondaryTextColor : Theme.textSecondary
    readonly property color linkInk: skinned ? row.bubbleItem.skinTextColor : Theme.focusBorder
    // Up to four letters of the name's extension, for the page.
    readonly property string extension: {
        const name = row.fileName;
        const dot = name.lastIndexOf(".");
        const ext = dot > 0 ? name.slice(dot + 1) : "";
        return ext.length > 0 && ext.length <= 4 && /^[A-Za-z0-9]+$/.test(ext) ? ext.toUpperCase() : "";
    }
    readonly property string title: row.fileName.length > 0 ? row.fileName
        : row.attachmentKind === 0 ? "Attachment" : "File"

    Accessible.role: Accessible.StaticText
    Accessible.name: "File: " + title + ", " + (complete ? row.sizeText : row.transferText)

    // The page.
    Item {
        id: page
        width: 34
        height: 42
        y: 1
        readonly property color hue: Theme.attachFile

        Shape {
            preferredRendererType: Shape.CurveRenderer // smooth on the GPU too (see ProfileGlyph)
            anchors.fill: parent
            ShapePath {
                strokeColor: Qt.darker(page.hue, 1.3)
                strokeWidth: 1
                joinStyle: ShapePath.RoundJoin
                fillGradient: LinearGradient {
                    x1: 0; y1: 0
                    x2: 0; y2: 42
                    GradientStop { position: 0; color: Qt.lighter(page.hue, 1.3) }
                    GradientStop { position: 0.5; color: page.hue }
                    GradientStop { position: 1; color: Qt.darker(page.hue, 1.1) }
                }
                PathSvg { path: "M 3.5 0.5 H 23.5 L 33.5 10.5 V 38.5 Q 33.5 41.5 30.5 41.5 H 3.5 Q 0.5 41.5 0.5 38.5 V 3.5 Q 0.5 0.5 3.5 0.5 Z" }
            }
            // The turned-down corner.
            ShapePath {
                strokeColor: Qt.darker(page.hue, 1.3)
                strokeWidth: 1
                joinStyle: ShapePath.RoundJoin
                fillColor: Qt.lighter(page.hue, 1.55)
                PathSvg { path: "M 23.5 0.5 V 8.5 Q 23.5 10.5 25.5 10.5 H 33.5 Z" }
            }
        }
        // A gloss down the page's left half.
        Rectangle {
            x: 3
            y: 3
            width: 12
            height: parent.height - 6
            radius: 2
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0; color: "#50ffffff" }
                GradientStop { position: 1; color: "#00ffffff" }
            }
        }
        // The type and, lacking one, the glyph: both centred in the page below
        // its turned-down corner (y 10.5 to 41.5), the type on its capitals.
        FontMetrics {
            id: extensionMetrics
            font: extensionLabel.font
        }
        Text {
            id: extensionLabel
            visible: file.extension.length > 0
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            y: Math.round(26 - extensionMetrics.capitalHeight / 2
                          - (extensionMetrics.ascent - extensionMetrics.capitalHeight))
            text: file.extension
            textFormat: Text.PlainText
            color: Theme.attachChipGlyph
            font.family: Theme.uiFont
            font.pixelSize: file.extension.length > 3 ? 8 : 9
            font.bold: true
            renderType: Text.NativeRendering
        }
        ProfileGlyph {
            visible: file.extension.length === 0
            anchors.horizontalCenter: parent.horizontalCenter
            y: 18
            width: 16
            height: 16
            kind: "file"
            ink: Theme.attachChipGlyph
        }
    }

    Text {
        id: name
        objectName: "chatFileName"
        x: page.width + 11
        y: 3
        width: parent.width - x
        text: file.title
        textFormat: Text.PlainText
        elide: Text.ElideMiddle
        color: file.ink
        style: file.skinned ? Text.Raised : Text.Normal
        styleColor: file.skinned ? file.row.bubbleItem.skinTextShadowColor : "transparent"
        font.family: Theme.uiFont
        font.pixelSize: 14
        renderType: Text.NativeRendering
    }
    Row {
        id: details
        x: name.x
        y: name.y + name.implicitHeight + 2
        width: parent.width - x
        spacing: 0

        Text {
            id: status
            objectName: "chatFileStatus"
            width: Math.min(implicitWidth, details.width - (save.visible ? save.width : 0)
                            - (cancel.visible ? cancel.width + 4 : 0))
            text: file.complete || file.row.transferText.length === 0 ? file.row.sizeText : file.row.transferText
            textFormat: Text.PlainText
            elide: Text.ElideRight
            color: file.row.transferState >= 2 && !file.skinned ? Theme.errorText : file.secondaryInk
            style: file.skinned ? Text.Raised : Text.Normal
            styleColor: file.skinned ? file.row.bubbleItem.skinTextShadowColor : "transparent"
            font.family: Theme.uiFont
            font.pixelSize: 12
            renderType: Text.NativeRendering
        }
        // Save…: the one thing to do with a file that is here.
        Text {
            id: save
            objectName: "chatFileSave"
            visible: file.row.canSave
            text: (status.text.length > 0 ? "  ·  " : "") + "Save…"
            textFormat: Text.PlainText
            color: file.linkInk
            font.family: Theme.uiFont
            font.pixelSize: 12
            font.underline: saveMouse.containsMouse
            renderType: Text.NativeRendering
            Accessible.role: Accessible.Button
            Accessible.name: "Save " + file.title
            Accessible.onPressAction: file.row.saveRequested()
            MouseArea {
                id: saveMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: file.row.saveRequested()
            }
        }
    }
    ChatCancelCross {
        id: cancel
        anchors.right: parent.right
        anchors.verticalCenter: details.verticalCenter
        visible: file.row.canCancel
        ink: file.secondaryInk
        onClicked: file.row.cancelRequested()
    }

    // How far the transfer has come, under the line that says so.
    Rectangle {
        objectName: "chatFileProgress"
        visible: file.row.transferState === 0
        x: name.x
        y: parent.height - 3
        width: parent.width - x
        height: 3
        radius: 1.5
        color: file.skinned ? "#30ffffff" : Theme.progressTrack
        Rectangle {
            width: Math.max(parent.height, parent.width * Math.max(0, Math.min(1, file.row.transferProgress)))
            height: parent.height
            radius: parent.radius
            color: file.skinned ? file.row.bubbleItem.skinTextColor : Theme.progressFill
        }
    }
}
