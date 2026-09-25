import QtQuick
import QtQuick.Dialogs
import QtQuick.Window
import OpenChat
import OpenChat.Native

Window {
    id: root
    objectName: "openChatWindow"
    required property var chatController
    property string dailyCaseAccount: "preview"
    property bool caseRequested: false
    DailyCaseController {
        id: dailyCase
        objectName: "dailyCaseController"
        accountKey: root.dailyCaseAccount
    }
    // Only what this account has unboxed can be worn: the case's authority
    // says what that is, and AppearanceSettings refuses anything else. An
    // unknown collection hands over nothing, rather than an empty one that
    // would take everything off.
    Binding {
        target: AppearanceSettings
        property: "ownedCosmetics"
        value: dailyCase.owned
        when: dailyCase.ownershipKnown
        restoreMode: Binding.RestoreNone
    }
    // What is worn lives with the account on the relay: its loadout is worn
    // here, and what is equipped here is sent there (a local stand-in keeps it
    // on this device instead).
    Binding {
        target: AppearanceSettings
        property: "loadout"
        value: dailyCase.loadout
        when: dailyCase.loadoutKnown
        restoreMode: Binding.RestoreNone
    }
    Connections {
        target: AppearanceSettings
        function onEquipRequested(slot, itemId) { dailyCase.equip(slot, itemId); }
    }
    onActiveChanged: { if (active) dailyCase.refresh(); }
    // Optional add-contact bridge; null in the default/capture paths. The visible
    // add-contact surface binds to it in a later change; declaring it here keeps the
    // live and --add-contact initial properties valid with no rendering change.
    property var contactController: null
    // Optional voice-call bridge; null in the default/capture paths. When it is
    // null (or reports no call) the conversation pane renders exactly as before.
    property var callController: null
    readonly property bool inCall: callController !== null && callController.inCall
    // A call belongs to one conversation. Its surface replaces that
    // conversation's header and no other; from anywhere else the call is a
    // strip under the header with the way back to it.
    readonly property bool callInView: inCall && callController.callInCurrentChat !== false
    // True while the call surface is the whole window: the sidebar and the
    // conversation are collapsed, nothing else is laid out or drawn, and the
    // call takes every pixel. Ends with the call, with leaving its
    // conversation, or from its corner chip.
    property bool callFullscreen: false
    readonly property int sidebarWidth: Math.round(
        Math.max(250, Math.min(300, width * Theme.sidebarWidth / 860)))
    // Where the pane to the right of the sidebar starts — at the sidebar's
    // edge, or at the window's when the sidebar is collapsed for a call.
    readonly property int paneX: callFullscreen ? 0 : sidebarWidth

    onCallInViewChanged: {
        if (!callInView)
            callFullscreen = false;
    }

    // OpenChat's icon in the notification area (a TrayIcon), when the
    // application has one; null in the previews and tests. Closing the
    // window then only hides it there (see CloseToTray).
    property var tray: null
    // What the notification-area icon shows: "" outside a call (the
    // application's icon), otherwise the orb for this user's microphone.
    // Deafened outranks muted, which outranks talking. There is no deafen yet:
    // `deafened` reads as undefined until CallController has one, and the grey
    // orb is ready for it.
    readonly property string trayCallState: {
        const calls = root.callController;
        if (calls === null || !calls.isActive)
            return "";
        if (calls.deafened === true)
            return "deafened";
        if (calls.muted)
            return "muted";
        return calls.localSpeaking ? "talking" : "call";
    }

    // Whether the window was maximised when last on screen, so that bringing
    // it back from the notification area or the taskbar keeps it that way.
    property bool shownMaximized: false
    onVisibilityChanged: {
        if (root.visibility === Window.Windowed || root.visibility === Window.Maximized)
            shownMaximized = root.visibility === Window.Maximized;
    }

    // Brings the window back from the notification area or the taskbar, in
    // front of the others. The tray, a clicked notification and a call that
    // rings while the window is hidden all come here.
    function bringToFront() {
        if (!root.visible || root.visibility === Window.Minimized) {
            if (root.shownMaximized)
                root.showMaximized();
            else
                root.showNormal();
        }
        root.raise();
        root.requestActivate();
    }

    // A call ringing while the window is in the notification area would
    // otherwise only be heard.
    readonly property bool callRinging: callController !== null && callController.isRinging
    onCallRingingChanged: {
        if (root.callRinging && !root.visible)
            root.bringToFront();
    }

    width: 860
    height: 680
    minimumWidth: 720
    minimumHeight: 560
    visible: true
    title: "OpenChat"
    color: Theme.contentBackground

    // Messages that arrive while a profile covers the chat stay unread.
    Binding {
        target: root.chatController
        property: "conversationVisible"
        value: root.active && root.visible && root.visibility !== Window.Minimized
               && !root.chatController.profiles.open
    }

    // A profile page covers the whole window (SPEC §1.1). It follows the
    // viewer's theme and their Plain style choice.
    readonly property var profiles: root.chatController.profiles
    Binding {
        target: root.profiles
        property: "darkMode"
        value: Theme.darkMode
    }
    Binding {
        target: root.profiles
        property: "plainStyle"
        value: AppearanceSettings.plainProfiles
    }
    // Whether the page exists: from the moment a profile opens until its
    // 120 ms close fade has run after the last one is popped (at once
    // without animations). Only this handler changes it, so the page is
    // never torn down and rebuilt in the middle of a close.
    property bool profileShown: root.profiles.open
    Timer {
        id: profileCloseTimer
        interval: 130
        onTriggered: root.profileShown = root.profiles.open
    }
    // Where the keyboard was before a profile opened, so closing it puts the
    // keyboard back (a typed Item property is nulled if that item goes away).
    property Item focusBeforeProfile: null
    onActiveFocusItemChanged: {
        if (!root.profiles.open && !root.profileShown)
            root.focusBeforeProfile = root.activeFocusItem;
    }
    Connections {
        target: root.profiles
        function onNavigationChanged() {
            if (root.profiles.open) {
                // The page covers the chat's enlarged picture too.
                chatMediaViewer.close(true);
                profileCloseTimer.stop();
                root.profileShown = true;
                return;
            }
            if (ProfileRenderPolicy.animationsAllowed)
                profileCloseTimer.restart();
            else
                root.profileShown = false;
            // After this signal has re-enabled the chat surface: a disabled
            // item cannot take the keyboard.
            Qt.callLater(root.restoreFocusAfterProfile);
        }
    }
    // Only when the keyboard is still on the closing page (or nowhere): an
    // action that left the page for the chat has already put it in the
    // composer, and that is where it belongs.
    function restoreFocusAfterProfile() {
        if (root.profiles.open || !root.focusBeforeProfile)
            return;
        for (let item = root.activeFocusItem; item !== null; item = item.parent) {
            if (item === profileLoader) {
                root.focusBeforeProfile.forceActiveFocus();
                return;
            }
        }
        if (root.activeFocusItem === null || root.activeFocusItem === root.contentItem)
            root.focusBeforeProfile.forceActiveFocus();
    }
    // The Safety Number dialog floats above the page; the ✓ appears once the
    // contact is verified.
    Connections {
        target: root.contactController
        ignoreUnknownSignals: true
        function onSafetyNumberOpenChanged() {
            if (!root.contactController.safetyNumberOpen && root.profiles.open)
                root.profiles.refresh();
        }
    }

    Item {
        id: applicationSurface
        anchors.fill: parent
        clip: true
        // Under an open profile the chat takes no input at all, and once the
        // page has faded in it stops rendering: no scene, frame or call video
        // is painted behind a page that covers it.
        enabled: !root.profiles.open
        visible: !(root.profiles.open && profileLoader.item && profileLoader.item.settled)

        ContactSidebar {
            id: sidebar
            width: root.sidebarWidth
            height: parent.height
            visible: !root.callFullscreen
            controller: root.chatController
            contactController: root.contactController
            dailyCaseController: dailyCase
            onCaseClicked: {
                dailyCase.refresh()
                root.caseRequested = true
            }
        }

        // The three navigation sections share the pane to the right of the
        // sidebar and are mutually exclusive. Chat is the default and renders
        // the approved conversation interface exactly as before; Call and
        // Settings are shown only when selected.
        Item {
            id: conversationPane
            objectName: "conversationPane"
            x: root.paneX
            width: parent.width - root.paneX
            height: parent.height
            visible: root.chatController.navSection === ChatController.NavSection.Chat
                     || root.callFullscreen

            Rectangle {
                anchors.fill: parent
                gradient: Gradient {
                    GradientStop { position: 0; color: Theme.contentBackground }
                    GradientStop { position: 1; color: Theme.contentBottom }
                }
            }

            // The top of the conversation pane is one slot with two occupants.
            // Out of a call it holds the conversation header — the contact's
            // name, picture, presence and call buttons. In a call that header is
            // gone entirely and the call surface takes the slot, showing both
            // people at once. They are never both present, so the contact is
            // never pictured twice. When the call fills the window the slot is
            // the whole pane, and the conversation below it is not laid out.
            Item {
                id: headerSlot
                objectName: "conversationHeaderSlot"
                width: parent.width
                height: root.callFullscreen ? parent.height
                        : root.callInView && callHeaderLoader.item
                          ? callHeaderLoader.item.implicitHeight : Theme.conversationHeaderHeight
                visible: root.chatController.hasCurrentContact || root.callInView
                clip: true

                ConversationHeader {
                    id: conversationHeader
                    anchors.fill: parent
                    controller: root.chatController
                    callController: root.callController
                    visible: !root.callInView
                }

                // Built only while there is a call (ringing, live or just
                // ended), so an idle window neither holds nor binds against it.
                Component {
                    id: callHeaderComponent

                    CallHeader {
                        maxVideoHeight: Math.min(230, root.height * 0.34)
                        maxShareHeight: Math.min(300, root.height * 0.32)
                        controller: root.callController
                        fullscreen: root.callFullscreen
                        availableHeight: root.height
                        zoom: mediaZoom
                        onFullscreenToggled: root.callFullscreen = !root.callFullscreen
                        onEnlargeRequested: videoItem => mediaZoom.enlarge(videoItem)
                        onProfileRequested: (accountId, name, avatarKey, self) => self
                            ? root.profiles.openOwn(Profile.FromCall)
                            : root.profiles.openPerson(accountId, name, avatarKey)
                    }
                }

                Loader {
                    id: callHeaderLoader
                    anchors.fill: parent
                    active: root.inCall
                    visible: root.callInView
                    sourceComponent: callHeaderComponent
                }
            }

            // The call, from any other conversation: one line, and the way
            // back. Built only while there is a call, like the surface itself.
            Loader {
                id: callStripLoader
                objectName: "callStripSlot"
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: headerSlot.bottom
                active: root.inCall
                readonly property bool shown: root.inCall && !root.callInView && !root.callFullscreen
                visible: shown
                height: shown && item ? item.implicitHeight : 0
                sourceComponent: CallStrip {
                    controller: root.callController
                    onReturnRequested: root.callController.showCallChat()
                }
            }

            // Shown instead of the conversation while no chat exists yet (a fresh
            // profile before its first accepted friend request).
            Column {
                objectName: "noConversation"
                anchors.centerIn: parent
                width: Math.min(360, parent.width - 48)
                spacing: 10
                visible: !root.chatController.hasCurrentContact && !root.callInView

                Text {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: "No chats yet"
                    color: Theme.textPrimary
                    font.family: Theme.uiFont
                    font.pixelSize: 16
                    font.bold: true
                    renderType: Text.NativeRendering
                }
                Text {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    text: "Search a username in the sidebar and send a friend request. "
                          + "Once they accept, your chat opens here."
                    color: Theme.textSecondary
                    font.family: Theme.uiFont
                    font.pixelSize: 14
                    renderType: Text.NativeRendering
                }
            }

            // Connection/security posture strip. Collapses to zero height and is
            // invisible while the session is Ready, so the approved interface
            // renders unchanged; it expands only when a state needs explaining.
            Item {
                id: securityBanner
                objectName: "securityBanner"
                readonly property bool active: root.chatController.sessionStateText.length > 0
                                               && !root.callFullscreen
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: callStripLoader.bottom
                height: active ? 30 : 0
                visible: active
                clip: true

                Rectangle {
                    anchors.fill: parent
                    color: Theme.warningBackground

                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: 1
                        color: Theme.warningBorder
                    }
                    Text {
                        objectName: "securityBannerText"
                        anchors.left: parent.left
                        anchors.leftMargin: 18
                        anchors.right: parent.right
                        anchors.rightMargin: 18
                        anchors.verticalCenter: parent.verticalCenter
                        elide: Text.ElideRight
                        text: root.chatController.sessionStateText
                        color: Theme.warningText
                        font.family: Theme.uiFont
                        font.pixelSize: 13
                        renderType: Text.NativeRendering
                    }
                }
            }

            MessageHistory {
                id: history
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: securityBanner.bottom
                anchors.bottom: messageComposer.top
                controller: root.chatController
                callActive: root.inCall
                visible: root.chatController.hasCurrentContact && !root.callFullscreen
                onTyped: (text) => messageComposer.takeTyping(text)
                onMediaRequested: (info, source) => chatMediaViewer.show(info, source)
                onSaveRequested: stableId => attachmentSaveDialog.saveFor(stableId)
            }

            Composer {
                id: messageComposer
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: implicitHeight
                // A long message may take up to two fifths of the pane before
                // it scrolls, leaving the conversation most of the room.
                maxInputHeight: Math.max(112, Math.round(conversationPane.height * 0.4))
                controller: root.chatController
                visible: root.chatController.hasCurrentContact && !root.callFullscreen
                onMessageSent: history.positionAtEnd()
            }

            // Files dragged in from the desktop go to the composer's tray,
            // while the chat is there to take them.
            AttachDropOverlay {
                anchors.fill: parent
                accepting: conversationPane.visible && root.chatController.hasCurrentContact
                           && !root.callFullscreen && messageComposer.canAttach && !root.profiles.open
                           && !root.contactDialogOpen && !root.caseRequested
                           && !chatMediaViewer.open && !mediaZoom.expanded
                onFilesDropped: files => messageComposer.attach(files)
            }
        }

        // Neutral no-selection pane for the Call section. Call history lives in
        // the sidebar; until a call is picked there is nothing to show here, so
        // this stays an empty gradient pane matching the conversation backdrop.
        Item {
            id: callView
            objectName: "callView"
            x: root.sidebarWidth
            width: parent.width - root.sidebarWidth
            height: parent.height
            visible: root.chatController.navSection === ChatController.NavSection.Call
                     && !root.callFullscreen

            Rectangle {
                anchors.fill: parent
                gradient: Gradient {
                    GradientStop { position: 0; color: Theme.contentBackground }
                    GradientStop { position: 1; color: Theme.contentBottom }
                }
            }
        }

        // Settings: the sidebar lists the categories and this pane shows the
        // open one's sections, each a working control.
        Item {
            id: settingsView
            objectName: "settingsView"
            x: root.sidebarWidth
            width: parent.width - root.sidebarWidth
            height: parent.height
            visible: root.chatController.navSection === ChatController.NavSection.Settings
                     && !root.callFullscreen

            Rectangle {
                anchors.fill: parent
                gradient: Gradient {
                    GradientStop { position: 0; color: Theme.contentBackground }
                    GradientStop { position: 1; color: Theme.contentBottom }
                }
            }

            SettingsPage {
                id: settingsDetail
                anchors.fill: parent
                controller: root.chatController
                restartAllowed: !root.inCall
            }
        }
    }

    // The profile page: over the sidebar and every pane, under the Add
    // Contact and Safety Number dialogs (declared after it, so they float
    // above), the screen-share picker and an enlarged picture. Built only
    // while a profile is open or fading out.
    Loader {
        id: profileLoader
        objectName: "profileLoader"
        anchors.fill: parent
        active: root.profileShown
        sourceComponent: ProfilePage {
            profiles: root.profiles
            chatController: root.chatController
            contactController: root.contactController
            callController: root.callController
            onPlainStyleChangeRequested: plain => AppearanceSettings.plainProfiles = plain
        }
    }

    Loader {
        active: root.caseRequested
        sourceComponent: DailyCaseModal {
            controller: dailyCase
            returnFocus: sidebar.caseButton
            onClosed: root.caseRequested = false
        }
        onLoaded: item.open()
    }

    // Add-contact overlay: floats above every section, filling the window. It binds
    // to the optional contactController and is built only while its dialog is
    // open, so the default and capture paths render unchanged.
    Loader {
        anchors.fill: parent
        active: root.contactController !== null && root.contactController.dialogOpen
        sourceComponent: AddContactDialog {
            contactController: root.contactController
        }
    }

    // Safety-number overlay: the contact-verification surface, shown at the natural
    // verify moment. Like the add-contact overlay it binds to the optional
    // contactController and is built only while its safety-number surface is open,
    // so the default and capture paths render unchanged.
    Loader {
        anchors.fill: parent
        active: root.contactController !== null && root.contactController.safetyNumberOpen
        sourceComponent: SafetyNumberDialog {
            contactController: root.contactController
        }
    }

    // Screen-source picker: raised by the call surface's share button, which
    // never guesses what to capture. It floats above every section like the
    // other overlays and, like the call surface, is built only during a call.
    Loader {
        id: screenSharePickerLoader
        anchors.fill: parent
        active: root.inCall
        z: 20

        sourceComponent: ScreenSharePicker {
            controller: root.callController
        }
    }

    // One camera or shared screen, enlarged over the whole window. Above the
    // picker, which cannot be opened while it is up anyway: the button that
    // opens the picker is under the scrim.
    MediaZoomOverlay {
        id: mediaZoom
        z: 30
    }

    // A photo or a video from the chat, opened large; above everything else,
    // like the enlarged call picture.
    ChatMediaViewer {
        id: chatMediaViewer
        z: 31
        controller: root.chatController
        callActive: root.inCall
        onSaveRequested: stableId => attachmentSaveDialog.saveFor(stableId)
        onClosed: {
            if (!root.profiles.open)
                messageComposer.takeTyping("");
        }
    }
    // Where a photo or a file from the chat is saved: the Downloads folder,
    // under the name it came with.
    FileDialog {
        id: attachmentSaveDialog
        objectName: "saveAttachmentDialog"
        property string stableId: ""
        title: "Save"
        fileMode: FileDialog.SaveFile
        function saveFor(stableId) {
            const controller = root.chatController;
            if (typeof controller.saveAttachment !== "function")
                return;
            attachmentSaveDialog.stableId = stableId;
            const folder = String(controller.attachmentFolderUrl());
            attachmentSaveDialog.currentFolder = folder;
            attachmentSaveDialog.selectedFile = folder.replace(/\/+$/, "") + "/"
                + encodeURIComponent(controller.suggestedSaveName(stableId));
            attachmentSaveDialog.open();
        }
        onAccepted: {
            root.chatController.saveAttachment(attachmentSaveDialog.stableId, selectedFile);
            attachmentSaveDialog.giveBackKeyboard();
        }
        onRejected: attachmentSaveDialog.giveBackKeyboard()
        // To the viewer it was asked from, or else the composer.
        function giveBackKeyboard() {
            if (chatMediaViewer.open)
                chatMediaViewer.forceActiveFocus();
            else if (!root.profiles.open)
                messageComposer.takeTyping("");
        }
    }

    Binding {
        target: root.tray
        property: "callState"
        value: root.trayCallState
        when: root.tray !== null
    }

    Connections {
        target: root.tray

        function onOpenRequested() {
            root.bringToFront();
        }

        function onCloseRequested() {
            Qt.quit();
        }
    }

    // Escape hands the window back when the call fills it. The enlarged
    // picture and the chat's viewer have their own Escape and go first, so one
    // press closes one thing.
    // An open profile covers the call and takes Escape itself.
    readonly property bool contactDialogOpen: root.contactController !== null
        && (root.contactController.safetyNumberOpen || root.contactController.dialogOpen)
    Shortcut {
        sequences: ["Escape"]
        enabled: root.callFullscreen && !mediaZoom.expanded && !root.profiles.open
                 && !root.contactDialogOpen && !chatMediaViewer.open
        onActivated: root.callFullscreen = false
    }
    // The Safety Number and Add Contact dialogs close on Escape too, and
    // before anything under them (a profile they were opened from).
    Shortcut {
        sequences: ["Escape"]
        enabled: root.contactDialogOpen && !mediaZoom.expanded
        onActivated: {
            if (root.contactController.safetyNumberOpen)
                root.contactController.closeSafetyNumber();
            else
                root.contactController.closeDialog();
        }
    }

    // The page's keys, one gated place for all of them: nothing reaches the
    // page while something floats above it (an enlarged picture, the Safety
    // Number or Add Contact dialog, the case, or a menu, popover or dialog of
    // the page itself), so one press of Escape closes one thing.
    readonly property bool profileKeysEnabled: root.profiles.open && profileLoader.item !== null
        && !mediaZoom.expanded
        && !root.contactDialogOpen
        && !root.caseRequested
        && !profileLoader.item.popupOpen
    Shortcut {
        sequences: ["Escape"]
        enabled: root.profileKeysEnabled
        onActivated: profileLoader.item.handleEscape()
    }
    Shortcut {
        sequences: ["Alt+Left"]
        enabled: root.profileKeysEnabled && !root.profiles.editing
        onActivated: profileLoader.item.handleBack()
    }
    // The selected one-to-one chat's profile, from anywhere in the window
    // (the composer included). A group has none.
    Shortcut {
        sequence: "Ctrl+I"
        enabled: !root.profiles.open && root.chatController.hasCurrentContact
                 && !root.chatController.currentIsGroup && !chatMediaViewer.open
        onActivated: root.profiles.openContact(root.chatController.currentContactId)
    }

    // Closing the window while your own page has unsaved changes asks first
    // (SPEC §14.12). Only without a notification-area icon: with one, a close
    // only hides the window (the autosaved draft is safe), and the closes
    // that do arrive belong to a Quit, which must not be held up.
    property bool closeConfirmed: false
    onClosing: close => {
        if (root.closeConfirmed) {
            root.closeConfirmed = false;
            return;
        }
        if (root.tray === null && root.profiles.editing && root.profiles.draftDirty
                && profileLoader.item) {
            close.accepted = false;
            profileLoader.item.requestWindowClose(() => {
                root.closeConfirmed = true;
                root.close();
            });
        }
    }

    Connections {
        target: root.callController
        ignoreUnknownSignals: true

        function onScreenSourcePickRequested() {
            if (screenSharePickerLoader.item)
                screenSharePickerLoader.item.show();
        }

        // A call that ends while the picker is open takes the picker with it,
        // and an enlarged picture of it as well.
        function onCallChanged() {
            if (root.inCall)
                return;
            if (screenSharePickerLoader.item)
                screenSharePickerLoader.item.dismiss();
            mediaZoom.dismiss(true);
        }
    }
}
