import QtQuick
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

    Binding {
        target: root.chatController
        property: "conversationVisible"
        value: root.active && root.visible && root.visibility !== Window.Minimized
    }

    Item {
        id: applicationSurface
        anchors.fill: parent
        clip: true

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
                visible: root.chatController.hasCurrentContact && !root.callFullscreen
                onTyped: (text) => messageComposer.takeTyping(text)
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
    // picture has its own Escape and goes first, so one press closes one thing.
    Shortcut {
        sequences: ["Escape"]
        enabled: root.callFullscreen && !mediaZoom.expanded
        onActivated: root.callFullscreen = false
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
