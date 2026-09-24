import QtQuick
import OpenChat

// "<First>'s Friend Space (Top N)" (SPEC §5.8): four tiles to a row, the
// name above the picture. The grid is one Tab stop: the arrows move between
// tiles and Enter or Space opens one. In the page view a tile opens that
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

    Item {
        id: grid
        objectName: "profileFriendGrid"
        width: parent.width
        height: tiles.height
        activeFocusOnTab: module.friends.length > 0
        property int current: 0
        onActiveFocusChanged: if (activeFocus) current = Math.min(current, Math.max(0, module.friends.length - 1))

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
                model: module.friends
                ProfileFriendTile {
                    required property var modelData
                    required property int index
                    objectName: "profileFriendTile"
                    friend: modelData
                    render: module.render
                    size: module.tileSize
                    pictureRadius: module.pictureRadius
                    keyboardFocus: grid.activeFocus && grid.current === index
                    onActivated: {
                        grid.current = index;
                        module.open(index);
                    }
                }
            }
        }
    }
}
