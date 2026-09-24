import QtQuick
import QtQuick.Controls
import OpenChat
import OpenChat.Native

// Top Friends (SPEC §14.11, `final-editor-friends.png`): the privacy line
// ("Your contacts will see who you put here."), eight numbered slots (slot 1
// in gold: "#1 is your #1"), the next free one inviting "Add", and your
// accepted contacts to fill them from, searchable, each placed one marked
// "#n ✓" and the one under the pointer or keyboard offering "+ Add as #n".
// A slot's context menu or Delete removes it; dragging a picture, or Alt+←/→,
// moves it.
Item {
    id: tab
    objectName: "profileFriendsTab"

    property var profiles: null
    property Item editor: null

    readonly property var draft: profiles ? profiles.draft : null
    readonly property var friends: draft ? draft.topFriends : []
    readonly property int maxFriends: profiles ? profiles.limits.maxTopFriends || 8 : 8
    readonly property int slotSize: Math.floor((width - 32 - 24) / 4)
    readonly property var candidates: {
        const all = profiles ? Array.from(profiles.topFriendCandidates) : [];
        const needle = search.text.trim().toLowerCase();
        const shown = needle.length > 0 ? all.filter(person => person.name.toLowerCase().includes(needle)) : all;
        // Sorted by name; ties (the same name twice) keep the roster's order.
        const order = new Map(all.map((person, index) => [person.contactId, index]));
        return shown.sort((a, b) => a.name.localeCompare(b.name) || order.get(a.contactId) - order.get(b.contactId));
    }
    readonly property bool full: friends.length >= maxFriends
    readonly property bool popupOpen: slotMenu.opened
    // The preview marks the box this tab edits while a control here has focus.
    readonly property bool focusInside: {
        for (let at = Window.activeFocusItem; at; at = at.parent) {
            if (at === tab)
                return true;
        }
        return false;
    }
    readonly property string editingTarget: focusInside ? "friends" : ""
    // A slot being dragged, and where it would land.
    property int dragFrom: -1
    property int dragTo: -1

    function focusField(field) {
        slots.forceActiveFocus(Qt.OtherFocusReason);
    }
    function add(contactId) {
        profiles.addTopFriend(contactId);
    }
    // The drop of a dragged picture. The slots are rebuilt by the move, so
    // this runs here rather than in the slot that started the drag.
    function finishDrag() {
        if (dragFrom >= 0) {
            if (dragTo >= 0 && dragTo !== dragFrom)
                profiles.moveTopFriend(dragFrom, dragTo);
            profiles.endGesture("friends:drag");
        }
        dragFrom = -1;
        dragTo = -1;
    }
    function slotAt(point) {
        for (let i = 0; i < slots.count; ++i) {
            const item = slots.itemAt(i);
            const local = item.mapFromItem(tab, point.x, point.y);
            if (local.x >= 0 && local.y >= 0 && local.x < item.width && local.y < item.height)
                return i;
        }
        return -1;
    }

    implicitHeight: column.y + column.implicitHeight + 16

    Column {
        id: column
        x: 16
        y: 14
        width: tab.width - 32

        Text {
            text: "Top Friends"
            color: Theme.categoryText
            font.family: Theme.uiFont
            font.pixelSize: 17
            renderType: Text.NativeRendering
            Accessible.role: Accessible.Heading
            Accessible.name: text
        }
        Item { width: 1; height: 4 }
        Row {
            spacing: 6
            ProfileEditorRail.Glyph {
                anchors.verticalCenter: parent.verticalCenter
                width: 13
                height: 13
                kind: "eye"
                ink: Theme.textSecondaryStrong
                stroke: 1.2
            }
            Text {
                objectName: "profileFriendsPrivacyNote"
                text: "Your contacts will see who you put here."
                color: Theme.textSecondaryStrong
                font.family: Theme.uiFont
                font.pixelSize: 13
                renderType: Text.NativeRendering
            }
        }
        Item { width: 1; height: 12 }

        ProfileTileGrid {
            id: slots
            objectName: "profileFriendSlots"
            accessibleName: "Top Friends"
            // One delegate per slot, kept while friends come and go, so a
            // slot keeps its hover and a drag its grab.
            model: tab.maxFriends
            columns: 4
            columnSpacing: 8
            rowSpacing: 8
            ringRadius: 5
            currentIndex: 0
            // Enter on a slot: a filled one says nothing new, the next free
            // one goes to the search field to pick someone.
            onActivated: index => {
                if (index >= tab.friends.length)
                    search.focusInput();
            }
            Keys.onDeletePressed: {
                if (focusIndex < tab.friends.length)
                    tab.profiles.removeTopFriend(focusIndex);
            }
            Keys.onPressed: event => {
                if (!(event.modifiers & Qt.AltModifier) || focusIndex >= tab.friends.length)
                    return;
                const step = event.key === Qt.Key_Left ? -1 : event.key === Qt.Key_Right ? 1
                           : event.key === Qt.Key_Up ? -columns : event.key === Qt.Key_Down ? columns : 0;
                const to = Math.max(0, Math.min(tab.friends.length - 1, focusIndex + step));
                if (step !== 0 && to !== focusIndex) {
                    tab.profiles.moveTopFriend(focusIndex, to);
                    focusIndex = to;
                }
                if (step !== 0)
                    event.accepted = true;
            }

            delegate: Item {
                id: slot
                required property int index
                readonly property var entry: index < tab.friends.length ? tab.friends[index] : null
                readonly property bool filled: entry !== null
                readonly property bool nextFree: index === tab.friends.length
                objectName: "profileFriendSlot_" + index
                width: tab.slotSize
                height: tab.slotSize + 18
                opacity: tab.dragFrom === index ? 0.4 : 1
                Accessible.role: Accessible.ListItem
                Accessible.name: filled ? "Number " + (index + 1) + ", " + entry.name
                                        : nextFree ? "Number " + (index + 1) + ", empty. Add someone" : "Number " + (index + 1) + ", empty"

                Avatar {
                    visible: slot.filled && slot.entry.avatarKey.length > 0
                    width: tab.slotSize
                    height: tab.slotSize
                    cornerRadius: 5
                    avatarKey: slot.filled && slot.entry.avatarKey.length > 0 ? slot.entry.avatarKey : "userpfp_none"
                }
                // Someone this viewer has no picture for: their initials.
                Rectangle {
                    visible: slot.filled && slot.entry.avatarKey.length === 0
                    width: tab.slotSize
                    height: tab.slotSize
                    radius: 5
                    color: Theme.panelBackground
                    border.width: 1
                    border.color: Theme.inputBorder
                    Text {
                        anchors.centerIn: parent
                        text: slot.filled ? slot.entry.initials : ""
                        textFormat: Text.PlainText
                        color: Theme.categoryText
                        font.family: Theme.uiFont
                        font.pixelSize: Math.round(tab.slotSize * 0.34)
                        renderType: Text.NativeRendering
                    }
                }
                // Empty: dashed, the next free one in focusBorder with "Add".
                Canvas {
                    id: dashes
                    visible: !slot.filled
                    width: tab.slotSize
                    height: tab.slotSize
                    property color ink: slot.nextFree ? Theme.focusBorder : Theme.buttonBorder
                    onInkChanged: requestPaint()
                    onVisibleChanged: requestPaint()
                    onPaint: {
                        const c = getContext("2d");
                        c.reset();
                        c.strokeStyle = ink;
                        c.lineWidth = slot.nextFree ? 2 : 1.2;
                        c.setLineDash([3, 2.5]);
                        const r = 5, o = 1, w = width - 2, h = height - 2;
                        c.beginPath();
                        c.moveTo(o + r, o);
                        c.lineTo(o + w - r, o);
                        c.arcTo(o + w, o, o + w, o + r, r);
                        c.lineTo(o + w, o + h - r);
                        c.arcTo(o + w, o + h, o + w - r, o + h, r);
                        c.lineTo(o + r, o + h);
                        c.arcTo(o, o + h, o, o + h - r, r);
                        c.lineTo(o, o + r);
                        c.arcTo(o, o, o + r, o, r);
                        c.stroke();
                    }
                    ProfileEditorRail.Glyph {
                        anchors.centerIn: parent
                        width: 18
                        height: 18
                        kind: "plus"
                        ink: slot.nextFree ? Theme.focusBorder : Theme.iconDisabled
                    }
                }
                // The slot's number; #1 in gold.
                Rectangle {
                    x: -4
                    y: -4
                    width: 18
                    height: 18
                    radius: 9
                    color: slot.index === 0 ? Theme.warningBackground : Theme.badgeBackground
                    border.width: 1
                    border.color: slot.index === 0 ? Theme.warningBorder : Theme.inputBorder
                    Text {
                        anchors.centerIn: parent
                        text: slot.index + 1
                        color: slot.index === 0 ? Theme.warningText : Theme.textSecondaryStrong
                        font.family: Theme.uiFont
                        font.pixelSize: 10
                        font.bold: true
                        renderType: Text.NativeRendering
                    }
                }
                Text {
                    y: tab.slotSize + 3
                    width: tab.slotSize
                    horizontalAlignment: Text.AlignHCenter
                    elide: Text.ElideRight
                    text: slot.filled ? slot.entry.name : slot.nextFree ? "Add" : ""
                    textFormat: Text.PlainText
                    color: slot.filled ? Theme.textPrimary : Theme.focusBorder
                    font.family: Theme.uiFont
                    font.pixelSize: 11
                    renderType: Text.NativeRendering
                }
                // A drop marker on the slot a dragged picture would land on.
                Rectangle {
                    visible: tab.dragTo === slot.index && tab.dragFrom !== slot.index
                    width: tab.slotSize
                    height: tab.slotSize
                    radius: 5
                    color: "transparent"
                    border.width: 2
                    border.color: Theme.focusBorder
                }
                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    cursorShape: slot.filled ? (pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor) : Qt.PointingHandCursor
                    property point pressPoint
                    onPressed: mouse => pressPoint = Qt.point(mouse.x, mouse.y)
                    onPositionChanged: mouse => {
                        if (!slot.filled || !pressed)
                            return;
                        if (tab.dragFrom < 0 && Math.abs(mouse.x - pressPoint.x) + Math.abs(mouse.y - pressPoint.y) > 6) {
                            tab.dragFrom = slot.index;
                            tab.profiles.beginGesture("friends:drag");
                        }
                        if (tab.dragFrom >= 0) {
                            const at = tab.slotAt(mapToItem(tab, mouse.x, mouse.y));
                            tab.dragTo = at >= 0 && at < tab.friends.length ? at : -1;
                        }
                    }
                    onReleased: tab.finishDrag()
                    onCanceled: tab.finishDrag()
                    onClicked: mouse => {
                        slots.focusIndex = slot.index;
                        if (mouse.button === Qt.RightButton && slot.filled)
                            slotMenu.openFor(slot.index, slot);
                        else if (slot.nextFree)
                            search.focusInput();
                    }
                }
            }
        }
        Item { width: 1; height: 8 }
        Text {
            width: parent.width
            wrapMode: Text.Wrap
            text: "Drag pictures to reorder. Slot 1 is your #1."
            color: Theme.textSecondaryStrong
            font.family: Theme.uiFont
            font.pixelSize: 12
            renderType: Text.NativeRendering
        }
        Item { width: 1; height: 12 }
        Rectangle { width: parent.width; height: 1; color: Theme.softRule }
        Item { width: 1; height: 12 }
        Text {
            text: "Your contacts"
            color: Theme.textSecondaryStrong
            font.family: Theme.uiFont
            font.pixelSize: 12
            renderType: Text.NativeRendering
        }
        Item { width: 1; height: 6 }
        AeroTextField {
            id: search
            objectName: "profileFriendSearch"
            width: parent.width
            height: 32
            placeholder: "Search your contacts"
            accessibleName: "Search your contacts"
            fontPixelSize: 13
            onAccepted: {
                const first = tab.candidates.find(person => person.placedIndex < 0);
                if (first && !tab.full)
                    tab.add(first.contactId);
            }
        }
        Item { width: 1; height: 6 }
        ProfileTileGrid {
            id: contacts
            objectName: "profileFriendCandidates"
            accessibleName: "Your contacts"
            // Stable delegates (a row keeps its hover as people are placed).
            model: tab.candidates.length
            columns: 1
            columnSpacing: 0
            rowSpacing: 0
            ringRadius: 4
            currentIndex: 0
            onActivated: index => {
                const person = tab.candidates[index];
                if (person.placedIndex < 0 && !tab.full)
                    tab.add(person.contactId);
            }
            delegate: Item {
                id: row
                required property int index
                readonly property var entry: tab.candidates[index] || ({ contactId: "", name: "", avatarKey: "", placedIndex: -1 })
                readonly property bool placed: entry.placedIndex >= 0
                // A HoverHandler, not the row's MouseArea: pressing "Add as
                // #n" must not end the row's hover (and hide the button).
                readonly property bool hot: !placed && !tab.full
                                            && (rowHover.hovered || (contacts.activeFocus && contacts.focusIndex === index))
                objectName: "profileFriendCandidate_" + entry.contactId
                width: column.width
                height: 38
                Accessible.role: Accessible.ListItem
                Accessible.name: entry.name + (placed ? ", number " + (entry.placedIndex + 1) : "")

                Rectangle {
                    visible: row.hot
                    anchors.fill: parent
                    radius: 4
                    gradient: Gradient {
                        orientation: Gradient.Horizontal
                        GradientStop { position: 0; color: Theme.selectedTop }
                        GradientStop { position: 1; color: Theme.selectedBottom }
                    }
                }
                Rectangle {
                    visible: row.index > 0 && !row.hot
                    x: 44
                    width: parent.width - 50
                    height: 1
                    color: Theme.softRule
                }
                Avatar {
                    x: 6
                    anchors.verticalCenter: parent.verticalCenter
                    width: 28
                    height: 28
                    cornerRadius: 4
                    avatarKey: row.entry.avatarKey.length > 0 ? row.entry.avatarKey : "userpfp_none"
                }
                Text {
                    x: 44
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - 44 - 110
                    elide: Text.ElideRight
                    text: row.entry.name
                    textFormat: Text.PlainText
                    color: Theme.textPrimary
                    font.family: Theme.uiFont
                    font.pixelSize: 14
                    renderType: Text.NativeRendering
                }
                Row {
                    visible: row.placed
                    anchors.right: parent.right
                    anchors.rightMargin: 6
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 6
                    Text {
                        objectName: "profileFriendCandidatePlace"
                        anchors.verticalCenter: parent.verticalCenter
                        text: "#" + (row.entry.placedIndex + 1)
                        color: Theme.textSecondaryStrong
                        font.family: Theme.uiFont
                        font.pixelSize: 12
                        renderType: Text.NativeRendering
                    }
                    Rectangle {
                        width: 20
                        height: 20
                        radius: 10
                        color: Theme.successFill
                        ProfileEditorRail.Glyph {
                            anchors.centerIn: parent
                            width: 12
                            height: 12
                            kind: "check"
                            ink: "#ffffff"
                        }
                    }
                }
                HoverHandler {
                    id: rowHover
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: contacts.focusIndex = row.index
                }
                ProfileEditorRail.Button {
                    objectName: "profileFriendAddButton"
                    visible: row.hot
                    anchors.right: parent.right
                    anchors.rightMargin: 6
                    anchors.verticalCenter: parent.verticalCenter
                    height: 26
                    glyph: "plus"
                    glyphSize: 12
                    label: "Add as #" + (tab.friends.length + 1)
                    fontPixelSize: 12
                    activeFocusOnTab: false
                    onClicked: tab.add(row.entry.contactId)
                }
            }
        }
        Text {
            visible: tab.full
            width: parent.width
            topPadding: 6
            wrapMode: Text.Wrap
            text: "Your Friend Space holds eight people. Remove someone to add another."
            color: Theme.textSecondaryStrong
            font.family: Theme.uiFont
            font.pixelSize: 12
            renderType: Text.NativeRendering
        }
    }

    AeroMenu {
        id: slotMenu
        objectName: "profileFriendSlotMenu"
        width: 220
        property int slotIndex: -1
        function openFor(index, item) {
            slotIndex = index;
            popup(item, item.width / 2, item.height / 2);
        }
        AeroMenuItem {
            objectName: "profileFriendRemove"
            text: "Remove from Top Friends"
            onTriggered: tab.profiles.removeTopFriend(slotMenu.slotIndex)
        }
        AeroMenuItem {
            text: "Move earlier"
            enabled: slotMenu.slotIndex > 0
            onTriggered: tab.profiles.moveTopFriend(slotMenu.slotIndex, slotMenu.slotIndex - 1)
        }
        AeroMenuItem {
            text: "Move later"
            enabled: slotMenu.slotIndex >= 0 && slotMenu.slotIndex < tab.friends.length - 1
            onTriggered: tab.profiles.moveTopFriend(slotMenu.slotIndex, slotMenu.slotIndex + 1)
        }
    }
}
