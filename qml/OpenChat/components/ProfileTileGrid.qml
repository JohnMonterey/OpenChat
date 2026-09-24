import QtQuick
import OpenChat

// A grid of picker tiles with CosmeticPicker's keyboard model (SPEC §16.3):
// the whole grid is one Tab stop that lands on the chosen tile, the arrows
// walk a keyboard cursor over the tiles, and Enter or Space picks the tile
// under it. The tiles themselves are the caller's `delegate` (with the usual
// required `modelData` and `index`); they draw their own chosen and hover
// states and call activate(index) when clicked. The grid draws the keyboard
// ring and reports `highlighted` whenever the cursor moves, so a tab can
// preview what the cursor is on (the Themes tab's try-on).
Item {
    id: grid

    property var model
    property Component delegate
    property int columns: 1
    property real columnSpacing: 5
    property real rowSpacing: 5
    property int currentIndex: -1 // the chosen tile
    property int focusIndex: 0    // the keyboard cursor
    property real ringRadius: 5
    property string accessibleName: ""
    readonly property int count: repeater.count
    signal activated(int index)
    signal highlighted(int index)

    function itemAt(index) {
        return repeater.itemAt(index);
    }
    function activate(index) {
        if (index < 0 || index >= count)
            return;
        focusIndex = index;
        activated(index);
    }
    function moveFocus(index) {
        if (index < 0 || index >= count || index === focusIndex)
            return;
        focusIndex = index;
        highlighted(index);
    }

    implicitWidth: layout.implicitWidth
    implicitHeight: layout.implicitHeight
    activeFocusOnTab: true
    Accessible.role: Accessible.Grouping
    Accessible.name: accessibleName
    onActiveFocusChanged: {
        if (activeFocus) {
            focusIndex = Math.max(0, Math.min(count - 1, currentIndex));
            highlighted(focusIndex);
        }
    }
    Keys.onLeftPressed: moveFocus(focusIndex - 1)
    Keys.onRightPressed: moveFocus(focusIndex + 1)
    Keys.onUpPressed: moveFocus(focusIndex - columns)
    Keys.onDownPressed: moveFocus(focusIndex + columns)
    Keys.onReturnPressed: activate(focusIndex)
    Keys.onEnterPressed: activate(focusIndex)
    Keys.onSpacePressed: activate(focusIndex)
    Keys.onPressed: event => {
        if (event.key === Qt.Key_Home) {
            moveFocus(0);
            event.accepted = true;
        } else if (event.key === Qt.Key_End) {
            moveFocus(count - 1);
            event.accepted = true;
        }
    }

    Grid {
        id: layout
        columns: grid.columns
        columnSpacing: grid.columnSpacing
        rowSpacing: grid.rowSpacing

        Repeater {
            id: repeater
            model: grid.model
            delegate: grid.delegate
        }
    }

    // The keyboard cursor: a 2 px ring 2 px outside the tile it is on.
    Rectangle {
        readonly property Item target: grid.activeFocus ? grid.itemAt(grid.focusIndex) : null
        visible: target !== null
        x: target ? target.x - 3 : 0
        y: target ? target.y - 3 : 0
        width: target ? target.width + 6 : 0
        height: target ? target.height + 6 : 0
        radius: grid.ringRadius + 3
        color: "transparent"
        border.width: 2
        border.color: Theme.focusBorder
    }
}
