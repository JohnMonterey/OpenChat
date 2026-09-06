import QtQuick
import QtQuick.Window
import OpenChat
import OpenChat.Native

Window {
    id: root
    objectName: "openChatWindow"
    required property var chatController
    // Optional add-contact bridge; null in the default/capture paths. The visible
    // add-contact surface binds to it in a later change; declaring it here keeps the
    // live and --add-contact initial properties valid with no rendering change.
    property var contactController: null
    // Optional voice-call bridge; null in the default/capture paths. When it is
    // null (or reports no call) the conversation pane renders exactly as before.
    property var callController: null
    readonly property bool inCall: callController !== null && callController.inCall
    // True while the call surface is the whole window: the sidebar and the
    // conversation are collapsed, nothing else is laid out or drawn, and the
    // call takes every pixel. Ends with the call, or from its corner chip.
    property bool callFullscreen: false
    readonly property int sidebarWidth: Math.round(
        Math.max(250, Math.min(300, width * Theme.sidebarWidth / 860)))
    // Where the pane to the right of the sidebar starts — at the sidebar's
    // edge, or at the window's when the sidebar is collapsed for a call.
    readonly property int paneX: callFullscreen ? 0 : sidebarWidth

    onInCallChanged: {
        if (!inCall)
            callFullscreen = false;
    }

    width: 860
    height: 680
    minimumWidth: 720
    minimumHeight: 560
    visible: true
    title: "OpenChat"
    color: Theme.contentBackground

    Item {
        id: applicationSurface
        anchors.fill: parent
        clip: true

        ContactSidebar {
            width: root.sidebarWidth
            height: parent.height
            visible: !root.callFullscreen
            controller: root.chatController
            contactController: root.contactController
        }

        // The three navigation sections share the pane to the right of the
        // sidebar and are mutually exclusive. Chat is the default and renders
        // the approved conversation interface exactly as before; Call and
        // Settings are placeholder stubs shown only when selected.
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
                        : root.inCall && callHeaderLoader.item
                          ? callHeaderLoader.item.implicitHeight : Theme.conversationHeaderHeight
                visible: root.chatController.hasCurrentContact || root.inCall
                clip: true

                ConversationHeader {
                    id: conversationHeader
                    anchors.fill: parent
                    controller: root.chatController
                    callController: root.callController
                    visible: !root.inCall
                }

                // Loaded only when a call bridge exists at all, so the default
                // and capture paths never instantiate (or bind against) it.
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
                    active: root.callController !== null
                    visible: root.inCall
                    sourceComponent: callHeaderComponent
                }
            }

            // Shown instead of the conversation while no chat exists yet (a fresh
            // profile before its first accepted friend request).
            Column {
                objectName: "noConversation"
                anchors.centerIn: parent
                width: Math.min(360, parent.width - 48)
                spacing: 10
                visible: !root.chatController.hasCurrentContact && !root.inCall

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
                anchors.top: headerSlot.bottom
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
            }

            Composer {
                id: messageComposer
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: implicitHeight
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

        // Settings detail pane: the title and element rows of the category
        // selected in the sidebar. The element rows are visual stubs — a label
        // and a muted disclosure chevron — until the individual controls are
        // wired to real preferences.
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

            Item {
                id: settingsDetail
                objectName: "settingsDetail"
                anchors.fill: parent
                anchors.leftMargin: 34
                anchors.rightMargin: 34
                anchors.topMargin: 28

                Text {
                    id: settingsDetailTitle
                    objectName: "settingsDetailTitle"
                    anchors.left: parent.left
                    anchors.right: parent.right
                    text: root.chatController.currentSettingsCategoryName
                    color: Theme.textPrimary
                    font.family: Theme.uiFont
                    font.pixelSize: 22
                    elide: Text.ElideRight
                    renderType: Text.NativeRendering
                }

                Rectangle {
                    id: settingsTitleRule
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: settingsDetailTitle.bottom
                    anchors.topMargin: 16
                    height: 1
                    color: Theme.rule
                }

                Column {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: settingsTitleRule.bottom
                    anchors.topMargin: 6

                    Repeater {
                        model: root.chatController.currentSettingsElements

                        Item {
                            id: settingsElementRow
                            property string elementLabel: modelData
                            readonly property bool themeSetting:
                                root.chatController.currentSettingsCategoryName === "Appearance"
                                && elementLabel === "Theme"
                            width: parent.width
                            height: themeSetting ? 70 : 48

                            Text {
                                anchors.left: parent.left
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.verticalCenterOffset: settingsElementRow.themeSetting ? -10 : 0
                                text: settingsElementRow.themeSetting ? "Dark mode" : settingsElementRow.elementLabel
                                color: Theme.textPrimary
                                font.family: Theme.uiFont
                                font.pixelSize: 15
                                renderType: Text.NativeRendering
                            }

                            Text {
                                visible: settingsElementRow.themeSetting
                                anchors.left: parent.left
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.verticalCenterOffset: 12
                                width: parent.width - darkModeSwitch.width - 16
                                text: "Use dark colors throughout OpenChat"
                                elide: Text.ElideRight
                                color: Theme.textSecondary
                                font.family: Theme.uiFont
                                font.pixelSize: 12
                            }
                            AeroSwitch {
                                id: darkModeSwitch
                                objectName: settingsElementRow.themeSetting ? "darkModeSwitch" : ""
                                visible: settingsElementRow.themeSetting
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                checked: Theme.darkMode
                                onToggled: checked => Theme.setDarkMode(checked)
                            }

                            Item {
                                visible: !settingsElementRow.themeSetting
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                width: 14
                                height: 14

                                Image {
                                    anchors.centerIn: parent
                                    width: 13
                                    height: 8
                                    opacity: 0.45
                                    rotation: -90
                                    source: Qt.resolvedUrl("../../assets/icons/chevron-down" + (Theme.darkMode ? "-dark.svg" : ".svg"))
                                    sourceSize: Qt.size(width * 2, height * 2)
                                }
                            }

                            Rectangle {
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                height: 1
                                color: Theme.softRule
                            }
                        }
                    }
                }
            }
        }
    }

    // Add-contact overlay: floats above every section, filling the window. It binds
    // to the optional contactController and self-hides when that is null or its
    // dialog is closed, so the default and capture paths render unchanged.
    AddContactDialog {
        contactController: root.contactController
    }

    // Safety-number overlay: the contact-verification surface, shown at the natural
    // verify moment. Like the add-contact overlay it binds to the optional
    // contactController and self-hides when that is null or its safety-number surface
    // is closed, so the default and capture paths render unchanged.
    SafetyNumberDialog {
        contactController: root.contactController
    }

    // Screen-source picker: raised by the call surface's share button, which
    // never guesses what to capture. It floats above every section like the
    // other overlays and, with no call bridge attached, never loads at all.
    Loader {
        id: screenSharePickerLoader
        anchors.fill: parent
        active: root.callController !== null
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
