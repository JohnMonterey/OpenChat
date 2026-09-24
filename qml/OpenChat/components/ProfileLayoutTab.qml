import QtQuick
import OpenChat
import OpenChat.Native

// Layout (SPEC §14.10, `final-editor-layout.png`): Classic, Flipped or One
// column, then each column's boxes in order. The app-owned boxes are locked
// in place ("Name & photo: always first", "Contacting …: always shown",
// "In your contacts: always first"); the others move by their grip (the row
// lifts and follows the pointer, and the preview reorders live), by the ↑ ↓
// ⇄ chips of the row under the pointer or keyboard, or by Alt+↑/↓ (Alt+←/→
// to the other column), and hide or show with their eye (or Space).
Item {
    id: tab
    objectName: "profileLayoutTab"

    property var profiles: null
    property Item editor: null

    readonly property var draft: profiles ? profiles.draft : null
    readonly property var arrangement: draft ? draft.moduleArrangement : []
    readonly property var narrowRows: arrangement.filter(entry => entry.column === Profile.NarrowColumn)
    readonly property var wideRows: arrangement.filter(entry => entry.column === Profile.WideColumn)
    readonly property int layout: draft ? draft.layout : Profile.ClassicLayout
    readonly property bool flipped: layout === Profile.FlippedLayout
    readonly property string firstName: profiles ? profiles.personFirstName : ""
    // The module being dragged by its grip, and the pointer, in tab coordinates.
    property int dragModule: -1
    property point dragPoint: Qt.point(0, 0)
    property string dragName: ""

    function columnRows(column) {
        return column === Profile.NarrowColumn ? narrowRows : wideRows;
    }
    function indexIn(column, module) {
        return columnRows(column).findIndex(entry => entry.module === module);
    }
    function move(module, column, index) {
        profiles.moveModule(module, column, index);
    }
    function nudge(module, column, step) {
        const at = indexIn(column, module);
        const to = at + step;
        if (at >= 0 && to >= 0 && to < columnRows(column).length)
            move(module, column, to);
    }
    function swapColumn(module, column) {
        const other = column === Profile.NarrowColumn ? Profile.WideColumn : Profile.NarrowColumn;
        move(module, other, Math.min(indexIn(column, module), columnRows(other).length));
    }
    function toggle(module, visible) {
        profiles.setModuleVisible(module, !visible);
    }
    // Where a dragged row lands: the list under the pointer (or the nearer
    // one), before every row whose middle is below the pointer.
    function dropTarget(point) {
        let best = null;
        for (const list of [firstList, secondList]) {
            const top = list.mapToItem(tab, 0, 0).y;
            const bottom = top + list.height;
            const distance = point.y < top ? top - point.y : point.y > bottom ? point.y - bottom : 0;
            if (best === null || distance < best.distance)
                best = { list: list, distance: distance };
        }
        const rows = best.list.rows.filter(entry => entry.module !== dragModule);
        let index = 0;
        for (let i = 0; i < best.list.count; ++i) {
            const row = best.list.rowAt(i);
            if (!row || row.module === dragModule)
                continue;
            if (row.mapToItem(tab, 0, row.height / 2).y < point.y)
                ++index;
        }
        return { column: best.list.column, index: Math.min(index, rows.length) };
    }
    function dragTo(point) {
        dragPoint = point;
        const target = dropTarget(point);
        const column = arrangement.find(entry => entry.module === dragModule).column;
        if (target.column !== column || target.index !== indexIn(column, dragModule))
            move(dragModule, target.column, target.index);
    }
    function focusField(field) {
        layouts.forceActiveFocus(Qt.OtherFocusReason);
    }

    implicitHeight: column.y + column.implicitHeight + 16

    // A locked, app-owned row.
    component LockedRow: Rectangle {
        property string label: ""
        property string note: ""
        width: column.width
        height: 34
        radius: 5
        color: Theme.buttonBackground
        border.width: 1
        border.color: Theme.buttonBorder
        Accessible.role: Accessible.ListItem
        Accessible.name: label + ", " + note
        Rectangle { x: 4; y: 1; width: parent.width - 8; height: 1; color: Theme.gloss }
        ProfileEditorRail.Glyph {
            x: 6
            anchors.verticalCenter: parent.verticalCenter
            width: 16
            height: 16
            kind: "lock"
            ink: Theme.iconDisabled
        }
        Text {
            x: 28
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - 28 - noteText.implicitWidth - 20
            elide: Text.ElideRight
            text: parent.label
            textFormat: Text.PlainText
            color: Theme.textPrimary
            font.family: Theme.uiFont
            font.pixelSize: 13
            renderType: Text.NativeRendering
        }
        Text {
            id: noteText
            anchors.right: parent.right
            anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            text: parent.note
            color: Theme.textSecondaryStrong
            font.family: Theme.uiFont
            font.pixelSize: 12
            renderType: Text.NativeRendering
        }
    }

    // A column's movable boxes: one Tab stop, ↑/↓ to walk, Alt+arrows to
    // move, Space to hide or show.
    component ModuleList: ProfileTileGrid {
        id: list
        property int column: Profile.NarrowColumn
        property ProfileTileGrid otherList: null
        readonly property var rows: tab.columnRows(column)
        function rowAt(index) {
            return itemAt(index);
        }
        model: rows
        columns: 1
        rowSpacing: 4
        ringRadius: 5
        currentIndex: 0
        onActivated: index => tab.toggle(rows[index].module, rows[index].visible)
        Keys.onPressed: event => {
            const entry = rows[focusIndex];
            if (!entry || !(event.modifiers & Qt.AltModifier))
                return;
            if (event.key === Qt.Key_Up || event.key === Qt.Key_Down) {
                const step = event.key === Qt.Key_Up ? -1 : 1;
                tab.nudge(entry.module, column, step);
                focusIndex = Math.max(0, tab.indexIn(column, entry.module));
                event.accepted = true;
            } else if (event.key === Qt.Key_Left || event.key === Qt.Key_Right) {
                // ← and → move toward that side of the page.
                const leftward = event.key === Qt.Key_Left;
                const onLeft = (column === Profile.NarrowColumn) !== tab.flipped;
                if (leftward !== onLeft) {
                    tab.swapColumn(entry.module, column);
                    if (otherList) {
                        otherList.forceActiveFocus(Qt.TabFocusReason);
                        otherList.focusIndex = Math.max(0, tab.indexIn(otherList.column, entry.module));
                    }
                }
                event.accepted = true;
            }
        }
        delegate: Item {
            id: row
            required property var modelData
            required property int index
            readonly property int module: modelData.module
            readonly property bool shown: modelData.visible
            readonly property bool hot: rowMouse.containsMouse || (list.activeFocus && list.focusIndex === index)
            readonly property bool lifted: tab.dragModule === module
            objectName: "profileLayoutRow_" + module
            width: column.width
            height: 34
            Accessible.role: Accessible.ListItem
            Accessible.name: modelData.name + (shown ? "" : ", hidden") + (modelData.hasContent ? "" : ", empty")

            MouseArea {
                id: rowMouse
                anchors.fill: parent
                hoverEnabled: true
                onClicked: list.focusIndex = row.index
            }
            Rectangle {
                anchors.fill: parent
                radius: 5
                opacity: row.lifted ? 0.4 : 1
                color: row.hot ? Theme.buttonHover : Theme.buttonBackground
                border.width: 1
                border.color: row.hot || row.lifted ? Theme.focusBorder : Theme.buttonBorder
                Rectangle { x: 4; y: 1; width: parent.width - 8; height: 1; color: Theme.gloss }
            }
            // Where the dragged row now sits.
            Item {
                visible: row.lifted
                y: -3
                width: parent.width
                height: 2
                Rectangle { x: 6; width: parent.width - 6; height: 2; radius: 1; color: Theme.focusBorder }
                Rectangle { y: -3; width: 8; height: 8; radius: 4; color: Theme.focusBorder }
            }
            ProfileEditorRail.Glyph {
                id: grip
                x: 6
                anchors.verticalCenter: parent.verticalCenter
                width: 16
                height: 16
                kind: "grip"
                ink: Theme.iconInk
                MouseArea {
                    objectName: "profileLayoutGrip"
                    anchors.fill: parent
                    anchors.margins: -6
                    cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                    onPressed: mouse => {
                        tab.dragName = row.modelData.name;
                        tab.dragModule = row.module;
                        tab.dragPoint = mapToItem(tab, mouse.x, mouse.y);
                        tab.profiles.beginGesture("layout:drag");
                    }
                    onPositionChanged: mouse => {
                        if (pressed && tab.dragModule >= 0)
                            tab.dragTo(mapToItem(tab, mouse.x, mouse.y));
                    }
                    onReleased: {
                        tab.dragModule = -1;
                        tab.profiles.endGesture();
                    }
                    onCanceled: {
                        tab.dragModule = -1;
                        tab.profiles.endGesture();
                    }
                }
            }
            Text {
                x: 28
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - 28 - actions.width - 12
                elide: Text.ElideRight
                text: row.modelData.name
                textFormat: Text.PlainText
                color: row.shown ? Theme.textPrimary : Theme.buttonDisabledText
                opacity: row.shown ? 1 : 0.55
                font.family: Theme.uiFont
                font.pixelSize: 13
                renderType: Text.NativeRendering
            }
            Row {
                id: actions
                anchors.right: parent.right
                anchors.rightMargin: 6
                anchors.verticalCenter: parent.verticalCenter
                spacing: 2
                Repeater {
                    model: row.hot && tab.dragModule < 0 ? ["up", "down", "swap"] : []
                    Rectangle {
                        required property string modelData
                        objectName: "profileLayoutRowAction_" + modelData
                        width: 24
                        height: 24
                        radius: 3
                        color: chipMouse.containsMouse ? Theme.buttonHover : "transparent"
                        border.width: 1
                        border.color: Theme.buttonBorder
                        Accessible.role: Accessible.Button
                        Accessible.name: modelData === "up" ? "Move up" : modelData === "down" ? "Move down" : "Move to the other column"
                        ProfileEditorRail.Glyph {
                            anchors.centerIn: parent
                            width: 14
                            height: 14
                            kind: parent.modelData
                            ink: Theme.iconInk
                            stroke: 1.5
                        }
                        MouseArea {
                            id: chipMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                if (parent.modelData === "up")
                                    tab.nudge(row.module, list.column, -1);
                                else if (parent.modelData === "down")
                                    tab.nudge(row.module, list.column, 1);
                                else
                                    tab.swapColumn(row.module, list.column);
                            }
                        }
                    }
                }
                Rectangle {
                    objectName: "profileLayoutEye_" + row.module
                    width: 26
                    height: 24
                    radius: 3
                    color: row.shown ? (eyeMouse.containsMouse ? Theme.buttonHover : "transparent") : Theme.buttonDisabled
                    border.width: 1
                    border.color: row.shown ? Theme.buttonBorder : Theme.buttonDisabledBorder
                    Accessible.role: Accessible.CheckBox
                    Accessible.name: "Show " + row.modelData.name
                    Accessible.checkable: true
                    Accessible.checked: row.shown
                    ProfileEditorRail.Glyph {
                        anchors.centerIn: parent
                        width: 16
                        height: 16
                        kind: row.shown ? "eye" : "eyeOff"
                        ink: row.shown ? Theme.iconInk : Theme.iconDisabled
                        stroke: 1.4
                    }
                    MouseArea {
                        id: eyeMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: tab.toggle(row.module, row.shown)
                    }
                }
            }
        }
    }

    Column {
        id: column
        x: 16
        y: 14
        width: tab.width - 32

        Text {
            text: "Layout"
            color: Theme.categoryText
            font.family: Theme.uiFont
            font.pixelSize: 17
            renderType: Text.NativeRendering
            Accessible.role: Accessible.Heading
            Accessible.name: text
        }
        Item { width: 1; height: 12 }
        ProfileTileGrid {
            id: layouts
            objectName: "profileLayoutCards"
            accessibleName: "Layout"
            model: ["Classic", "Flipped", "One column"]
            columns: 3
            columnSpacing: 8
            ringRadius: 5
            currentIndex: tab.layout
            onActivated: index => tab.draft.layout = index
            delegate: Item {
                id: card
                required property var modelData
                required property int index
                readonly property bool chosen: index === layouts.currentIndex
                objectName: "profileLayoutCard_" + index
                width: Math.floor((column.width - 16) / 3)
                height: 84
                Accessible.role: Accessible.RadioButton
                Accessible.name: modelData
                Accessible.checkable: true
                Accessible.checked: chosen
                Rectangle {
                    width: parent.width
                    height: 60
                    radius: 5
                    color: card.chosen ? Theme.navSelected : cardMouse.containsMouse ? Theme.buttonHover : Theme.fieldBackground
                    border.width: card.chosen ? 2 : 1
                    border.color: card.chosen ? Theme.focusBorder : Theme.inputBorder
                    // The layout drawn in miniature.
                    Item {
                        id: diagram
                        anchors.fill: parent
                        anchors.margins: 9
                        readonly property color ink: card.chosen ? Theme.focusBorder : Theme.buttonBorder
                        Rectangle {
                            visible: card.index !== Profile.SingleLayout
                            x: card.index === Profile.ClassicLayout ? 0 : parent.width * 0.6
                            width: parent.width * 0.38
                            height: parent.height
                            radius: 2
                            color: "transparent"
                            border.width: 1
                            border.color: diagram.ink
                            Rectangle { x: 3; y: 3; width: parent.width - 6; height: 9; radius: 1; color: diagram.ink; opacity: 0.55 }
                        }
                        Rectangle {
                            visible: card.index !== Profile.SingleLayout
                            x: card.index === Profile.ClassicLayout ? parent.width * 0.44 : 0
                            width: parent.width * 0.56
                            height: parent.height * 0.44
                            radius: 2
                            color: "transparent"
                            border.width: 1
                            border.color: diagram.ink
                        }
                        Rectangle {
                            visible: card.index !== Profile.SingleLayout
                            x: card.index === Profile.ClassicLayout ? parent.width * 0.44 : 0
                            y: parent.height * 0.52
                            width: parent.width * 0.56
                            height: parent.height * 0.48
                            radius: 2
                            color: "transparent"
                            border.width: 1
                            border.color: diagram.ink
                        }
                        Column {
                            visible: card.index === Profile.SingleLayout
                            anchors.horizontalCenter: parent.horizontalCenter
                            spacing: 3
                            Repeater {
                                model: 3
                                Rectangle {
                                    width: 34
                                    height: 11
                                    radius: 2
                                    color: "transparent"
                                    border.width: 1
                                    border.color: diagram.ink
                                }
                            }
                        }
                    }
                }
                Text {
                    y: 65
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: card.modelData
                    color: card.chosen ? Theme.textPrimary : Theme.textSecondaryStrong
                    font.family: Theme.uiFont
                    font.pixelSize: 12
                    renderType: Text.NativeRendering
                }
                MouseArea {
                    id: cardMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: layouts.activate(card.index)
                }
            }
        }

        Item { width: 1; height: 12 }
        Text {
            text: tab.layout === Profile.SingleLayout ? "Narrow column" : "Left column"
            color: Theme.textSecondaryStrong
            font.family: Theme.uiFont
            font.pixelSize: 12
            renderType: Text.NativeRendering
        }
        Item { width: 1; height: 6 }
        // The first list: the page's left column (the narrow one unless flipped).
        Column {
            width: parent.width
            spacing: 4
            LockedRow {
                visible: !tab.flipped
                label: "Name & photo"
                note: "always first"
            }
            LockedRow {
                visible: !tab.flipped
                label: "Contacting " + tab.firstName
                note: "always shown"
            }
            LockedRow {
                visible: tab.flipped
                label: "“In your contacts”"
                note: "always first"
            }
            ModuleList {
                id: firstList
                objectName: "profileLayoutFirstList"
                accessibleName: "Left column"
                column: tab.flipped ? Profile.WideColumn : Profile.NarrowColumn
                otherList: secondList
            }
        }
        Item { width: 1; height: 12 }
        Text {
            text: tab.layout === Profile.SingleLayout ? "Wide column" : "Right column"
            color: Theme.textSecondaryStrong
            font.family: Theme.uiFont
            font.pixelSize: 12
            renderType: Text.NativeRendering
        }
        Item { width: 1; height: 6 }
        Column {
            width: parent.width
            spacing: 4
            LockedRow {
                visible: tab.flipped
                label: "Name & photo"
                note: "always first"
            }
            LockedRow {
                visible: tab.flipped
                label: "Contacting " + tab.firstName
                note: "always shown"
            }
            LockedRow {
                visible: !tab.flipped
                label: "“In your contacts”"
                note: "always first"
            }
            ModuleList {
                id: secondList
                objectName: "profileLayoutSecondList"
                accessibleName: "Right column"
                column: tab.flipped ? Profile.NarrowColumn : Profile.WideColumn
                otherList: firstList
            }
        }
        Item { width: 1; height: 12 }
        Text {
            width: parent.width
            wrapMode: Text.Wrap
            text: (tab.layout === Profile.SingleLayout
                   ? "One column shows the wide column's boxes first, then the narrow column's. " : "")
                  + "Drag by the grip, or select a row and press Alt+↑/↓ (Alt+←/→ moves it to the other column). "
                  + "Boxes with nothing in them are hidden from contacts."
            color: Theme.textSecondaryStrong
            font.family: Theme.uiFont
            font.pixelSize: 12
            lineHeight: 1.05
            renderType: Text.NativeRendering
        }
    }

    // The row in flight, following the pointer.
    Rectangle {
        objectName: "profileLayoutDragRow"
        visible: tab.dragModule >= 0
        z: 5
        x: column.x + 8
        y: tab.dragPoint.y - height / 2
        width: column.width
        height: 34
        radius: 5
        color: Theme.fieldBackground
        border.width: 1
        border.color: Theme.focusBorder
        Rectangle { z: -1; x: 2; y: 4; width: parent.width; height: parent.height; radius: 5; color: "#26000000" }
        ProfileEditorRail.Glyph {
            x: 6
            anchors.verticalCenter: parent.verticalCenter
            width: 16
            height: 16
            kind: "grip"
            ink: Theme.iconInk
        }
        Text {
            x: 28
            anchors.verticalCenter: parent.verticalCenter
            text: tab.dragName
            textFormat: Text.PlainText
            color: Theme.textPrimary
            font.family: Theme.uiFont
            font.pixelSize: 13
            renderType: Text.NativeRendering
        }
    }
}
