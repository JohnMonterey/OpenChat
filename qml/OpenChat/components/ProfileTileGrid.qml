import QtQuick
import OpenChat

// A grid of picker tiles with CosmeticPicker's keyboard model (SPEC §16.3):
// the whole grid is one Tab stop that lands on the chosen tile, the arrows
// move the keyboard focus from tile to tile, and Enter or Space picks the
// tile that has it. The focus is really on the tile (each is a radio button
// or a button to screen readers, so every arrow press is announced); only
// that one tile is a Tab stop, so Tab and Shift+Tab leave the grid.
//
// The tiles themselves are the caller's `delegate` (with the usual required
// `modelData` and `index`, and an Accessible role and name); they draw their
// own chosen and hover states and call activate(index) when clicked. The grid
// draws the keyboard ring around the focused tile and reports `highlighted`
// whenever an arrow moves the focus, so a tab can preview what it is on (the
// Themes tab's try-on). `submitted` follows Enter or Return (after
// `activated`), for a grid whose Enter also means "done" (the colour picker).
FocusScope {
    id: grid

    property var model
    property Component delegate
    property int columns: 1
    property real columnSpacing: 5
    property real rowSpacing: 5
    property int currentIndex: -1 // the chosen tile
    property int focusIndex: 0    // the tile with the keyboard focus
    property real ringRadius: 5
    property string accessibleName: ""
    readonly property int count: repeater.count
    // The one tile Tab lands on: the chosen one (the first when none is),
    // or the one the keyboard is on while the grid has the focus.
    readonly property int tabIndex: activeFocus ? focusIndex : Math.max(0, Math.min(count - 1, currentIndex))
    signal activated(int index)
    signal highlighted(int index)
    signal submitted(int index)

    function itemAt(index) {
        return repeater.itemAt(index);
    }
    function activate(index) {
        if (index < 0 || index >= count)
            return;
        focusIndex = index;
        activated(index);
    }
    function arrow(event, index) {
        if (event.modifiers & Qt.AltModifier) {
            event.accepted = false;
            return;
        }
        moveFocus(index);
    }
    function moveFocus(index) {
        if (index < 0 || index >= count || index === focusIndex)
            return;
        focusIndex = index;
        highlighted(index);
    }
    // The focus follows focusIndex while the grid has it.
    function focusTile() {
        const tile = itemAt(focusIndex);
        if (tile && grid.activeFocus && !tile.activeFocus)
            tile.forceActiveFocus(Qt.TabFocusReason);
    }
    function focusedTile() {
        for (let i = 0; i < count; ++i) {
            const tile = itemAt(i);
            if (tile && tile.activeFocus)
                return i;
        }
        return -1;
    }

    implicitWidth: layout.implicitWidth
    implicitHeight: layout.implicitHeight
    Accessible.role: Accessible.Grouping
    Accessible.name: accessibleName
    onFocusIndexChanged: focusTile()
    onActiveFocusChanged: {
        if (!activeFocus) {
            // Next time the grid is given the focus it starts from the
            // chosen tile again, not from wherever the keyboard left it.
            const left = itemAt(focusIndex);
            if (left)
                left.focus = false;
            return;
        }
        // Tab landed on a tile, or the grid itself was given the focus.
        const landed = focusedTile();
        if (landed >= 0) {
            focusIndex = landed;
            return;
        }
        focusIndex = Math.max(0, Math.min(count - 1, currentIndex));
        focusTile();
    }
    // Alt+arrows are left to the grid's user (reordering its tiles): they
    // reach its Keys.onPressed, which the plain arrows here never do.
    Keys.onLeftPressed: event => arrow(event, focusIndex - 1)
    Keys.onRightPressed: event => arrow(event, focusIndex + 1)
    Keys.onUpPressed: event => arrow(event, focusIndex - columns)
    Keys.onDownPressed: event => arrow(event, focusIndex + columns)
    Keys.onReturnPressed: {
        activate(focusIndex);
        submitted(focusIndex);
    }
    Keys.onEnterPressed: {
        activate(focusIndex);
        submitted(focusIndex);
    }
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
            onItemAdded: (index, item) => {
                // The focused tile stays a Tab stop until the focus has left
                // it (Qt refuses to take that from the active focus item).
                item.activeFocusOnTab = Qt.binding(() => index === grid.tabIndex || item.activeFocus);
                // A tile rebuilt under the keyboard takes the focus back.
                if (grid.activeFocus && index === grid.focusIndex)
                    refocus.restart();
            }
        }
    }

    Timer {
        id: refocus
        interval: 0
        onTriggered: grid.focusTile()
    }

    // The keyboard ring: 2 px, 2 px outside the tile that has the focus.
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
