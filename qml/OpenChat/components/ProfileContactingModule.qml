import QtQuick
import OpenChat
import OpenChat.Native

// "Contacting <First>" (SPEC §5.2, ARCH §8.4): the app's own box of actions
// that really work today. The theme recolours it and never hides or fakes
// it. A contact's page offers Send Message, Voice and Video Call (only with a
// call service; disabled with a reason during a call with someone else, one
// "Return to call" during a call with them) and Safety Number (✓ once
// verified). Your own page swaps in "Your Profile": Edit Profile, Change
// Picture and Copy Invite Link. Chat-bound actions close the profile and
// switch to the Chat section before they act.
//
// The grid is one Tab stop: the arrows move the keyboard focus from cell to
// cell (each cell takes it, so a screen reader announces the action), Enter
// or Space activates. In the editor preview it shows the contact view at full
// strength and is inert.
ProfileBox {
    id: module
    objectName: "profileContactingBox"
    property var view: null
    readonly property var profiles: view ? view.profiles : null
    readonly property bool inert: view ? view.preview : false
    readonly property bool own: profiles !== null && profiles.isOwnProfile && !inert
    readonly property string first: view ? view.ownerFirstName : ""
    readonly property var actions: module.actionList()
    readonly property int cellWidth: Math.max(0, Math.floor((innerWidth - 4) / 2))
    property bool invitePending: false

    render: view ? view.render : null
    pad: 7
    title: own ? "Your Profile" : "Contacting " + first

    // The cells this person and these services allow, in SPEC order.
    function actionList() {
        if (!module.view || !module.profiles)
            return [];
        const view = module.view;
        const who = module.first;
        if (module.own) {
            const mine = [
                { id: "edit", glyph: "pencil", label: "Edit Profile", compact: "Edit", name: "Edit your profile" },
                { id: "picture", glyph: "camera", label: "Change Picture", compact: "Picture",
                  name: "Change your picture" }
            ];
            if (view.contactController)
                mine.push({ id: "invite", glyph: "link", label: "Copy Invite Link", compact: "Invite Link",
                            name: "Copy your invite link" });
            return mine;
        }
        const list = [{ id: "message", glyph: "envelope", label: "Send Message", compact: "Message",
                        name: "Send Message to " + who }];
        const calls = view.callController;
        if (module.inert || (calls && calls.callsAvailable)) {
            const inCall = !module.inert && calls.inCall === true;
            const withThem = inCall && module.profiles.personId.length > 0
                             && calls.callChatId === module.profiles.personId;
            if (withThem) {
                list.push({ id: "return", glyph: "phone", label: "Return to call", compact: "Return",
                            name: "Return to your call with " + who });
            } else {
                const reason = inCall ? "You're already in a call" : "";
                list.push({ id: "voice", glyph: "phone", label: "Voice Call", compact: "Call",
                            name: "Voice call " + who, unavailable: inCall, reason: reason });
                list.push({ id: "video", glyph: "video", label: "Video Call", compact: "Video",
                            name: "Video call " + who, unavailable: inCall, reason: reason });
            }
        }
        if (module.inert || view.contactController)
            list.push({ id: "safety", glyph: "shield", label: "Safety Number", compact: "Verify",
                        name: "Safety Number for " + who,
                        badge: !module.inert && module.profiles.personVerified });
        return list;
    }

    // What each cell does (ARCH §8.4). Everything leaving the page reads the
    // person first: closing the stack forgets who was on screen.
    function run(action) {
        if (!action || action.unavailable || module.inert)
            return;
        const view = module.view;
        const profiles = module.profiles;
        const person = profiles.personId;
        const chat = view.chatController;
        const calls = view.callController;
        switch (action.id) {
        case "message":
        case "voice":
        case "video":
            profiles.closeAll();
            if (chat) {
                chat.setNavSection(ChatController.NavSection.Chat);
                chat.selectContact(person);
            }
            if (action.id !== "message" && calls)
                calls.callCurrentContact(action.id === "video");
            break;
        case "return":
            profiles.closeAll();
            if (calls)
                calls.showCallChat();
            break;
        case "safety":
            if (view.contactController)
                view.contactController.openSafetyNumber(person);
            break;
        case "edit":
            profiles.beginEditing();
            break;
        case "picture":
            view.requestChangePicture();
            break;
        case "invite":
            module.copyInvite();
            break;
        }
    }

    // Each press asks for a new invite and copies that one, never one made
    // before (an invite works once): it arrives at once in the mock and from
    // the relay otherwise, through myInviteChanged either way.
    function copyInvite() {
        const contacts = module.view ? module.view.contactController : null;
        if (!contacts)
            return;
        module.invitePending = true;
        contacts.createMyInvite();
    }
    function finishInvite() {
        const contacts = module.view ? module.view.contactController : null;
        if (!module.invitePending || !contacts || !contacts.inviteReady)
            return;
        module.invitePending = false;
        module.profiles.copyText(contacts.myInvite, "Copied invite link");
    }
    // No invite came: say so, and stop waiting, so an invite made later
    // (the Add Contact dialog's) is never copied unasked.
    function inviteFailed() {
        if (!module.invitePending)
            return;
        module.invitePending = false;
        module.profiles.showNotice("Couldn't create an invite. Try again.");
    }

    Connections {
        target: module.view ? module.view.contactController : null
        ignoreUnknownSignals: true
        function onMyInviteChanged() { module.finishInvite(); }
        function onInviteFailed() { module.inviteFailed(); }
    }

    FocusScope {
        id: grid
        objectName: "profileContactingGrid"
        width: parent.width
        height: cells.height
        property int current: 0
        readonly property bool reachable: !module.inert && module.actions.length > 0
        // The cell Tab lands on, and the one with the focus while it is here.
        readonly property int tabIndex: Math.max(0, Math.min(current, module.actions.length - 1))
        // From `current` itself: tabIndex may not have caught up yet.
        function focusCurrent() {
            const cell = cellRepeater.itemAt(Math.max(0, Math.min(grid.current, module.actions.length - 1)));
            if (cell && grid.activeFocus && !cell.activeFocus)
                cell.forceActiveFocus(Qt.TabFocusReason);
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
            const count = module.actions.length;
            if (count === 0)
                return;
            let next = grid.current;
            switch (event.key) {
            case Qt.Key_Left: next = Math.max(0, grid.current - 1); break;
            case Qt.Key_Right: next = Math.min(count - 1, grid.current + 1); break;
            case Qt.Key_Up: next = grid.current >= 2 ? grid.current - 2 : grid.current; break;
            case Qt.Key_Down: next = grid.current + 2 < count ? grid.current + 2 : grid.current; break;
            case Qt.Key_Return:
            case Qt.Key_Enter:
            case Qt.Key_Space:
                module.run(module.actions[grid.current]);
                event.accepted = true;
                return;
            default:
                return;
            }
            grid.current = next;
            event.accepted = true;
        }

        Grid {
            id: cells
            columns: 2
            rowSpacing: 2
            columnSpacing: 4
            Repeater {
                id: cellRepeater
                model: module.actions
                // A cell rebuilt under the keyboard (a call began) takes it back.
                onItemAdded: (index, item) => {
                    if (grid.activeFocus && index === grid.tabIndex)
                        refocus.restart();
                }
                ProfileActionCell {
                    required property var modelData
                    required property int index
                    objectName: "profileAction_" + modelData.id
                    width: module.cellWidth
                    render: module.render
                    glyph: modelData.glyph
                    label: modelData.label
                    compactLabel: modelData.compact
                    accessibleName: modelData.name
                    available: !modelData.unavailable
                    reason: modelData.reason || ""
                    badge: modelData.badge === true
                    inert: module.inert
                    // The focused cell stays a Tab stop until the focus has left it.
                    activeFocusOnTab: (grid.reachable && index === grid.tabIndex) || activeFocus
                    keyboardFocus: activeFocus
                    onActivated: {
                        grid.current = index;
                        module.run(modelData);
                    }
                }
            }
        }
    }
}
