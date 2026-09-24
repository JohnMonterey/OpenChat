import QtQuick
import QtQuick.Shapes
import OpenChat
import OpenChat.Native

// The editor preview's affordances over one module (SPEC §14.4). Hover: a
// 2 px focusBorder outline 4 px outside with a faint focusBorder tint, and an
// "✎ Edit" chip on the top edge; clicking anywhere in the module (or the
// chip) asks for its tab and field. The module whose field is being edited
// keeps the outline with an "Editing" chip. App-owned boxes (Contacting, the
// banner, the handle) stay at full strength but inert: hover shows a quiet
// outline and a "Shown by OpenChat" lock chip that explains itself. An empty
// module shows as a dashed placeholder that invites filling it in.
Item {
    id: decor
    objectName: "profilePreviewDecor"
    // The module's own target, and finer ones inside it: [{target, item}].
    property string target: ""
    property var regions: []
    // The targets that count as "this module" for the Editing chip.
    property var targets: target.length > 0 ? [target] : []
    property string editingTarget: ""
    // App-owned boxes: never editable, and they say why.
    property bool inert: false
    property string inertTip: ""
    property real radius: 0
    property bool placeholder: false
    property string placeholderText: ""
    signal activated(string target)

    readonly property bool hovered: area.containsMouse || chipArea.containsMouse
    readonly property bool editing: !inert && editingTarget.length > 0 && targets.indexOf(editingTarget) >= 0
    readonly property bool outlined: hovered || editing
    readonly property int fadeMs: ProfileRenderPolicy.animationsAllowed ? 120 : 0

    // The finest target under a point (in this item's coordinates).
    function targetAt(x, y) {
        // An empty module is one invitation: all of it opens its field.
        if (decor.placeholder)
            return decor.target;
        for (let i = 0; i < decor.regions.length; ++i) {
            const region = decor.regions[i];
            if (!region.item || !region.item.visible)
                continue;
            const p = region.item.mapFromItem(decor, x, y);
            if (p.x >= 0 && p.y >= 0 && p.x < region.item.width && p.y < region.item.height)
                return region.target;
        }
        return decor.target;
    }

    // The empty module, as a placeholder to fill in.
    Rectangle {
        objectName: "profilePreviewPlaceholder"
        visible: decor.placeholder
        anchors.fill: parent
        radius: decor.radius
        color: Theme.panelBackground
        opacity: 0.94
        Shape {
            anchors.fill: parent
            ShapePath {
                strokeColor: Theme.focusBorder
                strokeWidth: 1.5
                fillColor: "transparent"
                strokeStyle: ShapePath.DashLine
                dashPattern: [4 / 1.5, 3 / 1.5]
                PathSvg {
                    path: "M 1 1 L " + (decor.width - 1) + " 1 L " + (decor.width - 1) + " " + (decor.height - 1)
                          + " L 1 " + (decor.height - 1) + " Z"
                }
            }
        }
        Text {
            anchors.centerIn: parent
            width: parent.width - 24
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            text: decor.placeholderText
            textFormat: Text.PlainText
            color: Theme.categoryText
            font.family: Theme.uiFont
            font.pixelSize: 13
            renderType: Text.NativeRendering
        }
    }

    Item {
        anchors.fill: parent
        opacity: decor.outlined ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: decor.fadeMs; easing.type: Easing.InOutQuad } }

        Rectangle {
            objectName: "profilePreviewOutline"
            anchors.fill: parent
            anchors.margins: -4
            radius: decor.radius + 3
            color: decor.inert ? "transparent" : Qt.rgba(Theme.focusBorder.r, Theme.focusBorder.g, Theme.focusBorder.b, 0.08)
            border.width: 2
            border.color: decor.inert ? Theme.textSecondary : Theme.focusBorder
        }
        Rectangle {
            id: chip
            objectName: "profilePreviewChip"
            anchors.right: parent.right
            anchors.rightMargin: 6
            y: -12
            width: chipRow.implicitWidth + 16
            height: 22
            radius: 4
            border.width: 1
            border.color: decor.inert ? Theme.buttonBorder : Theme.focusBorder
            gradient: Gradient {
                GradientStop { position: 0; color: Theme.buttonTop }
                GradientStop { position: 1; color: Theme.buttonBottom }
            }
            Rectangle {
                x: 3
                y: 1
                width: parent.width - 6
                height: 1
                color: Theme.gloss
            }
            Row {
                id: chipRow
                anchors.centerIn: parent
                spacing: 5
                ProfileGlyph {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 12
                    height: 12
                    kind: decor.inert ? "lock" : "pencil"
                    ink: Theme.iconInk
                    stroke: 1.3
                }
                Text {
                    objectName: "profilePreviewChipText"
                    anchors.verticalCenter: parent.verticalCenter
                    text: decor.inert ? "Shown by OpenChat" : decor.editing ? "Editing" : "Edit"
                    textFormat: Text.PlainText
                    color: Theme.buttonText
                    font.family: Theme.uiFont
                    font.pixelSize: 12
                    renderType: Text.NativeRendering
                }
            }
        }
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: decor.inert ? Qt.ArrowCursor : Qt.PointingHandCursor
        onClicked: mouse => {
            const target = decor.inert ? "" : decor.targetAt(mouse.x, mouse.y);
            if (target.length > 0)
                decor.activated(target);
        }
    }
    // The chip reaches above the module, so it takes its own clicks.
    MouseArea {
        id: chipArea
        x: chip.x
        y: chip.y
        width: chip.width
        height: chip.height
        visible: decor.outlined && !decor.inert
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: decor.activated(decor.target)
    }

    ProfileTip {
        parent: chip
        visible: decor.inert && decor.hovered && decor.inertTip.length > 0
        text: decor.inertTip
        // Above the chip, so it never covers the box it explains.
        y: -height - 4
        x: chip.width - width
    }
}
