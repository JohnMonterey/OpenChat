import QtQuick
import OpenChat

// "<First>'s Friend Space (Top N)" (SPEC §5.8): four tiles to a row, the
// name above the picture. The grid is one Tab stop: the arrows move the
// keyboard focus between tiles (each takes it, so a screen reader announces
// the friend) and Enter or Space opens one. In the page view a tile opens that
// friend's page (or a stub); in the editor preview it opens the Top Friends
// tab instead.
ProfileBox {
    id: module
    objectName: "profileFriendSpaceBox"
    property var view: null
    readonly property var friends: view ? view.page.topFriends : []
    readonly property int tileSize: Math.max(0, Math.floor((innerWidth - 30) / 4))
    readonly property real pictureRadius: Math.min(4, radius + 1)

    render: view ? view.render : null
    alt: true
    pad: 12
    title: view ? view.possessive(view.ownerFirstName) + " Friend Space" : ""
    suffix: friends.length > 0 ? "(Top " + friends.length + ")" : ""

    function open(index) {
        if (module.view && index >= 0 && index < module.friends.length)
            module.view.activateTopFriend(index);
    }

    FocusScope {
        id: grid
        objectName: "profileFriendGrid"
        width: parent.width
        height: tiles.height
        property int current: 0
        // The tile Tab lands on, and the one with the focus while it is here.
        readonly property int tabIndex: Math.max(0, Math.min(current, module.friends.length - 1))
        // From `current` itself: tabIndex may not have caught up yet.
        function focusCurrent() {
            const tile = tileRepeater.itemAt(Math.max(0, Math.min(grid.current, module.friends.length - 1)));
            if (tile && grid.activeFocus && !tile.activeFocus)
                tile.forceActiveFocus(Qt.TabFocusReason);
        }
        onActiveFocusChanged: {
            if (activeFocus) {
                current = tabIndex;
                focusCurrent();
            }
        }
        onCurrentChanged: focusCurrent()
        Timer {
            id: refocus
            interval: 0
            onTriggered: grid.focusCurrent()
        }

        Accessible.role: Accessible.List
        Accessible.name: module.title

        Keys.onPressed: event => {
            const count = module.friends.length;
            if (count === 0)
                return;
            let next = grid.current;
            switch (event.key) {
            case Qt.Key_Left: next = Math.max(0, grid.current - 1); break;
            case Qt.Key_Right: next = Math.min(count - 1, grid.current + 1); break;
            case Qt.Key_Up: next = grid.current >= 4 ? grid.current - 4 : grid.current; break;
            case Qt.Key_Down: next = grid.current + 4 < count ? grid.current + 4 : grid.current; break;
            case Qt.Key_Home: next = 0; break;
            case Qt.Key_End: next = count - 1; break;
            case Qt.Key_Return:
            case Qt.Key_Enter:
            case Qt.Key_Space:
                module.open(grid.current);
                event.accepted = true;
                return;
            default:
                return;
            }
            grid.current = next;
            event.accepted = true;
        }

        Grid {
            id: tiles
            width: parent.width
            columns: 4
            columnSpacing: 10
            rowSpacing: 12
            Repeater {
                id: tileRepeater
                model: module.friends
                // A tile rebuilt under the keyboard (a refresh) takes it back.
                onItemAdded: (index, item) => {
                    if (grid.activeFocus && index === grid.tabIndex)
                        refocus.restart();
                }
                ProfileFriendTile {
                    required property var modelData
                    required property int index
                    objectName: "profileFriendTile"
                    friend: modelData
                    render: module.render
                    size: module.tileSize
                    pictureRadius: module.pictureRadius
                    // The focused tile stays a Tab stop until the focus has left it.
                    activeFocusOnTab: index === grid.tabIndex || activeFocus
                    keyboardFocus: activeFocus
                    onActivated: {
                        grid.current = index;
                        module.open(index);
                    }
                }
            }
        }
    }
}
