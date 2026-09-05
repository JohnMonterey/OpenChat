import QtQuick
import QtQuick.Dialogs
import OpenChat
import OpenChat.Native

Item {
    id: sidebar
    objectName: "contactSidebar"
    required property var controller
    // Optional add-contact bridge; null in the default/capture paths. When it is
    // absent the requests panel below collapses and the favorites category renders
    // exactly as before.
    property var contactController: null
    readonly property real contactRowHeight: Math.max(44, Math.min(65, (height - 290) / 6))

    // Navigation artwork uses a fixed slot, independent of font glyph metrics.
    component NavigationIcon: Item {
        width: 26
        height: 26
        anchors.horizontalCenter: parent.horizontalCenter
        y: 10
    }

    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0; color: Theme.sidebarTop }
            GradientStop { position: 1; color: Theme.sidebarBottom }
        }
    }

    Rectangle {
        anchors.right: parent.right
        width: 1
        height: parent.height
        color: Theme.rule
    }

    Item {
        id: localUser
        width: parent.width
        height: 88
        // Above the search field so a profile notice can overhang it.
        z: 1

        // The text block starts one avatar-margin right of the avatar, the same
        // gap the contact rows below use, so names and status lines line up.
        readonly property int textLeft: localAvatar.x + localAvatar.width + 14
        // The right edge the status field may grow to: clear of the add-contact
        // "+" when it is shown, otherwise of the sidebar's own margin.
        readonly property int statusRight: width - 48

        Avatar {
            id: localAvatar
            objectName: "localUserAvatar"
            x: 13
            y: 22
            width: 44
            height: 44
            avatarKey: sidebar.controller.localAvatarKey
        }

        // Hovering the picture darkens it and shows a "+"; clicking opens the
        // platform file chooser for a new picture. Invisible until hovered, so
        // the default rendering is untouched.
        Item {
            id: avatarChanger
            objectName: "localAvatarButton"
            anchors.fill: localAvatar
            readonly property bool hovered: avatarMouse.containsMouse

            Rectangle {
                objectName: "localAvatarHoverShade"
                anchors.fill: parent
                radius: localAvatar.cornerRadius
                color: "#66000000"
                visible: avatarChanger.hovered
            }
            Rectangle {
                anchors.centerIn: parent
                width: 16
                height: 3
                radius: 1
                color: "white"
                visible: avatarChanger.hovered
            }
            Rectangle {
                anchors.centerIn: parent
                width: 3
                height: 16
                radius: 1
                color: "white"
                visible: avatarChanger.hovered
            }

            MouseArea {
                id: avatarMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: avatarFileDialog.open()
            }
        }

        // The native picker on every desktop (macOS/Windows/GTK or the portal
        // on Linux, with Qt's own dialog as the fallback). The chosen file is
        // scaled and compressed locally before anything is stored or sent.
        FileDialog {
            id: avatarFileDialog
            objectName: "localAvatarFileDialog"
            title: "Choose a profile picture"
            fileMode: FileDialog.OpenFile
            nameFilters: ["Images (*.png *.jpg *.jpeg *.bmp *.gif *.webp *.tif *.tiff)",
                          "All files (*)"]
            onAccepted: sidebar.controller.setLocalAvatarFromFile(selectedFile)
        }

        Text {
            id: localName
            objectName: "localUserName"
            x: localUser.textLeft
            y: 22
            // Leaves room for the bead after the name; a long name elides.
            width: Math.min(implicitWidth, localUser.statusRight - x - localBead.width - 8)
            elide: Text.ElideRight
            text: sidebar.controller.localUserName
            color: Theme.textPrimary
            font.family: Theme.uiFont
            font.pixelSize: 17
            renderType: Text.NativeRendering
        }

        // The bead sits after the name, on its line, so the status line below
        // can use the full width from the avatar's margin.
        PresenceBead {
            id: localBead
            x: localName.x + localName.width + 8
            anchors.verticalCenter: localName.verticalCenter
            anchors.verticalCenterOffset: 1
            beadSize: 11
            presence: sidebar.controller.localOnline ? sidebar.controller.localPresence : 2
        }

        // Hovering the bead darkens it; clicking opens the presence picker.
        Item {
            id: presenceButton
            objectName: "localPresenceButton"
            x: localBead.x - 3
            y: localBead.y - 3
            width: localBead.width + 6
            height: localBead.height + 6

            Rectangle {
                objectName: "localPresenceHoverShade"
                anchors.fill: parent
                radius: width / 2
                color: "#33000000"
                visible: presenceMouse.containsMouse || presenceMenu.visible
            }
            MouseArea {
                id: presenceMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: presenceMenu.visible = !presenceMenu.visible
            }
        }

        // The status line: a hover tint marks it as a field; a click turns it
        // into an editor holding the current text. Enter or leaving the field
        // commits, Escape puts the old text back.
        Item {
            id: statusEditor
            objectName: "localStatusEditor"
            x: localUser.textLeft - 4
            y: 43
            width: Math.max(60, localUser.statusRight - x)
            height: 22
            property bool editing: false

            function beginEditing() {
                // Edit the custom text when there is one; otherwise start from
                // the presence name so it can be replaced or kept as-is.
                statusInput.text = sidebar.controller.localStatusText.length > 0
                        ? sidebar.controller.localStatusText
                        : sidebar.controller.localStatusLine;
                editing = true;
                statusInput.forceActiveFocus();
                statusInput.selectAll();
            }
            function commit() {
                if (!editing)
                    return;
                editing = false;
                var text = statusInput.text.trim();
                // Keeping the presence name as-is means "no custom status".
                if (sidebar.controller.localStatusText.length === 0
                        && text === sidebar.controller.localStatusLine)
                    text = "";
                sidebar.controller.setLocalStatusText(text);
            }
            function cancel() {
                editing = false;
            }

            Rectangle {
                objectName: "localStatusHoverShade"
                anchors.fill: parent
                radius: 3
                color: statusEditor.editing ? Theme.fieldBackground : Theme.profileHover
                border.width: statusEditor.editing ? 1 : 0
                border.color: Theme.inputBorder
                visible: statusMouse.containsMouse || statusEditor.editing
            }

            Text {
                id: statusLabel
                objectName: "localStatusText"
                x: 4
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - 8
                elide: Text.ElideRight
                visible: !statusEditor.editing
                text: sidebar.controller.localStatusLine
                color: Theme.textSecondary
                font.family: Theme.uiFont
                font.pixelSize: 14
                renderType: Text.NativeRendering
            }

            TextInput {
                id: statusInput
                objectName: "localStatusInput"
                x: 4
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - 8
                visible: statusEditor.editing
                clip: true
                selectByMouse: true
                selectionColor: Theme.selectionBackground
                selectedTextColor: Theme.selectionText
                maximumLength: 80
                color: Theme.textPrimary
                font.family: Theme.uiFont
                font.pixelSize: 14
                onAccepted: statusEditor.commit()
                onActiveFocusChanged: {
                    if (!activeFocus)
                        statusEditor.commit();
                }
                Keys.onEscapePressed: statusEditor.cancel()
            }

            MouseArea {
                id: statusMouse
                anchors.fill: parent
                hoverEnabled: true
                enabled: !statusEditor.editing
                cursorShape: Qt.IBeamCursor
                onClicked: statusEditor.beginEditing()
            }
        }

        // Why a profile change was refused, shown in place of the status line
        // for a few seconds. Collapsed and invisible while there is nothing.
        Rectangle {
            objectName: "profileNotice"
            x: statusEditor.x
            y: statusEditor.y + statusEditor.height + 2
            width: localUser.width - x - 12
            height: noticeText.implicitHeight + 6
            radius: 3
            color: Theme.warningBackground
            border.width: 1
            border.color: Theme.noticeBorder
            visible: sidebar.controller.profileNotice.length > 0
            z: 5

            Text {
                id: noticeText
                x: 5
                y: 3
                width: parent.width - 10
                wrapMode: Text.WordWrap
                text: sidebar.controller.profileNotice
                color: Theme.noticeText
                font.family: Theme.uiFont
                font.pixelSize: 12
                renderType: Text.NativeRendering
            }
            Timer {
                running: sidebar.controller.profileNotice.length > 0
                interval: 6000
                onTriggered: sidebar.controller.clearProfileNotice()
            }
        }

        // Primitive "+" affordance drawn from two crossed strokes, opening the
        // add-contact dialog (invite codes). Present only with a contact bridge.
        Item {
            objectName: "addContactButton"
            anchors.right: parent.right
            anchors.rightMargin: 18
            y: 34
            width: 20
            height: 20
            visible: sidebar.contactController && sidebar.contactController.enabled

            Rectangle {
                anchors.centerIn: parent
                width: 14
                height: 2
                radius: 1
                color: Theme.categoryText
            }
            Rectangle {
                anchors.centerIn: parent
                width: 2
                height: 14
                radius: 1
                color: Theme.categoryText
            }

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    if (sidebar.contactController)
                        sidebar.contactController.openDialog();
                }
            }
        }
    }

    Item {
        id: searchArea
        anchors.top: localUser.bottom
        width: parent.width
        height: 46

        Rectangle {
            x: 13
            y: 7
            width: parent.width - 26
            height: 32
            radius: 4
            color: Theme.searchBackground
            border.width: 1
            border.color: Theme.inputBorder

            TextInput {
                id: searchInput
                objectName: "contactSearch"
                x: 7
                y: 5
                width: parent.width - 38
                height: parent.height - 10
                color: Theme.textPrimary
                font.family: Theme.uiFont
                font.pixelSize: 14
                clip: true
                selectByMouse: true
                selectionColor: Theme.selectionBackground
                selectedTextColor: Theme.selectionText
                onTextEdited: {
                    sidebar.controller.setSearchQuery(text);
                    if (sidebar.contactController)
                        sidebar.contactController.lookup(text);
                }

                Text {
                    anchors.fill: parent
                    visible: !searchInput.text && !searchInput.activeFocus
                    text: "Search Chats and Users"
                    color: Theme.placeholderText
                    font: searchInput.font
                    verticalAlignment: Text.AlignVCenter
                }
            }

            Item {
                anchors.right: parent.right
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                width: 15
                height: 15

                Rectangle {
                    x: 1; y: 1; width: 8; height: 8; radius: 4
                    color: "transparent"; border.width: 2; border.color: Theme.searchIcon
                }
                Rectangle {
                    x: 9; y: 9; width: 7; height: 2
                    rotation: 45; color: Theme.searchIcon; transformOrigin: Item.Left
                }
            }
        }
    }

    // The region between the search field and the bottom navigation swaps its
    // content with the active section. Exactly one container is visible at a
    // time; each fills the same middle band so the panes line up. Chat keeps the
    // approved contact list unchanged; Call shows the (currently empty) call
    // history list.
    Item {
        id: chatContactArea
        objectName: "chatContactArea"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: searchArea.bottom
        anchors.bottom: bottomNav.top
        visible: sidebar.controller.navSection === ChatController.NavSection.Chat

        Column {
            anchors.top: parent.top
            width: parent.width

            // Search Chats and Users: the exact-handle directory match for the search
            // text, with a grey "add person" affordance that sends a friend
            // request. Collapses to zero height while there is nothing to show.
            Item {
                id: directoryResult
                objectName: "directoryResult"
                width: parent.width
                readonly property bool shown:
                    sidebar.contactController && sidebar.contactController.enabled
                    && sidebar.contactController.lookupVisible
                readonly property bool canRequest:
                    shown && sidebar.contactController.lookupCanRequest
                visible: shown
                height: shown ? 40 + sidebar.contactRowHeight : 0
                clip: true

                Rectangle {
                    id: directoryHeader
                    width: parent.width
                    height: 40
                    color: Theme.sectionHighlight

                    Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.rule }
                    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule }

                    Text {
                        x: 15
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.verticalCenterOffset: 1
                        text: "Search Chats and Users"
                        color: Theme.categoryText
                        font.family: Theme.uiFont
                        font.pixelSize: 17
                        renderType: Text.NativeRendering
                    }
                }

                Item {
                    id: directoryRow
                    objectName: "directoryResultRow"
                    anchors.top: directoryHeader.bottom
                    width: parent.width
                    height: sidebar.contactRowHeight
                    readonly property bool compact: height < 55

                    Avatar {
                        id: directoryAvatar
                        x: 13
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.min(44, directoryRow.height - 6)
                        height: width
                        avatarKey: "userpfp_none"
                    }

                    Text {
                        objectName: "directoryResultName"
                        x: 70
                        y: directoryRow.compact ? 3 : 11
                        width: sendRequestButton.x - x - 8
                        text: sidebar.contactController
                              ? "@" + sidebar.contactController.lookupHandle : ""
                        color: Theme.textPrimary
                        font.family: Theme.uiFont
                        font.pixelSize: 16
                        elide: Text.ElideRight
                        renderType: Text.NativeRendering
                    }

                    Text {
                        objectName: "directoryResultSubtitle"
                        x: 70
                        y: directoryRow.compact ? 26 : 34
                        width: sendRequestButton.x - x - 8
                        text: sidebar.contactController
                              ? sidebar.contactController.lookupSubtitle : ""
                        color: Theme.textSecondary
                        font.family: Theme.uiFont
                        font.pixelSize: 14
                        elide: Text.ElideRight
                        renderType: Text.NativeRendering
                    }

                    // Grey "add person" icon: a head, shoulders and a small plus,
                    // drawn from primitives. Enabled only while a request can be
                    // sent; afterwards it dims to show the request went out.
                    Item {
                        id: sendRequestButton
                        objectName: "sendRequestButton"
                        anchors.right: parent.right
                        anchors.rightMargin: 14
                        anchors.verticalCenter: parent.verticalCenter
                        width: 30
                        height: 30
                        readonly property bool sent:
                            sidebar.contactController
                            && (sidebar.contactController.lookupState
                                    === ContactController.LookupState.RequestSent
                                || sidebar.contactController.lookupState
                                    === ContactController.LookupState.RequestPending)
                        readonly property color ink: directoryResult.canRequest
                            ? (sendRequestMouse.containsMouse ? Theme.iconHover : Theme.searchIcon)
                            : Theme.iconDisabled
                        visible: directoryResult.canRequest || sent

                        Rectangle {
                            anchors.fill: parent
                            radius: 15
                            color: directoryResult.canRequest
                                   ? (sendRequestMouse.containsMouse ? Theme.navSelected : Theme.requestButton)
                                   : "transparent"
                            border.width: 1
                            border.color: sendRequestButton.ink
                        }
                        // Head.
                        Rectangle {
                            x: 9; y: 6; width: 8; height: 8; radius: 4
                            color: sendRequestButton.ink
                        }
                        // Shoulders.
                        Rectangle {
                            x: 6; y: 15; width: 14; height: 8; radius: 4
                            color: sendRequestButton.ink
                        }
                        Rectangle {
                            x: 6; y: 19; width: 14; height: 4
                            color: sendRequestButton.ink
                        }
                        // Plus (or a check once the request went out).
                        Rectangle {
                            visible: !sendRequestButton.sent
                            x: 19; y: 9; width: 6; height: 2; radius: 1
                            color: sendRequestButton.ink
                        }
                        Rectangle {
                            visible: !sendRequestButton.sent
                            x: 21; y: 7; width: 2; height: 6; radius: 1
                            color: sendRequestButton.ink
                        }
                        Rectangle {
                            visible: sendRequestButton.sent
                            x: 18; y: 12; width: 4; height: 2; radius: 1; rotation: 45
                            color: Theme.requestSuccess
                        }
                        Rectangle {
                            visible: sendRequestButton.sent
                            x: 20; y: 10; width: 7; height: 2; radius: 1; rotation: -50
                            color: Theme.requestSuccess
                        }

                        MouseArea {
                            id: sendRequestMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            enabled: directoryResult.canRequest
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                if (sidebar.contactController)
                                    sidebar.contactController.requestLookup();
                            }
                        }
                    }
                }
            }

            // Inbound friend requests, shown as a category above Chats only while
            // one is pending. Present only when a live/preview ContactController is
            // attached and enabled; otherwise it collapses to zero height and the
            // Column skips it, so the categories below render exactly as before.
            Item {
                id: requestsPanel
                objectName: "requestsPanel"
                width: parent.width
                readonly property bool hasRequests:
                    sidebar.contactController && sidebar.contactController.requests.count > 0
                visible: sidebar.contactController && sidebar.contactController.enabled
                         && hasRequests
                height: visible ? requestsHeader.height + requestsColumn.height : 0
                clip: true

                Rectangle {
                    id: requestsHeader
                    width: parent.width
                    height: 40
                    color: Theme.sectionHighlight

                    Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.rule }
                    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule }

                    Text {
                        x: 15
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.verticalCenterOffset: 1
                        text: "Friend requests"
                        color: Theme.categoryText
                        font.family: Theme.uiFont
                        font.pixelSize: 17
                        renderType: Text.NativeRendering
                    }

                    Rectangle {
                        objectName: "requestsBadge"
                        anchors.right: parent.right
                        anchors.rightMargin: 16
                        anchors.verticalCenter: parent.verticalCenter
                        height: 18
                        width: Math.max(18, requestsBadgeLabel.implicitWidth + 10)
                        radius: height / 2
                        color: "#e0503d"

                        Text {
                            id: requestsBadgeLabel
                            anchors.centerIn: parent
                            text: sidebar.contactController
                                  ? sidebar.contactController.requests.count : ""
                            color: "white"
                            font.family: Theme.uiFont
                            font.pixelSize: 11
                            font.bold: true
                        }
                    }
                }

                Column {
                    id: requestsColumn
                    anchors.top: requestsHeader.bottom
                    width: parent.width

                    Repeater {
                        objectName: "requestsList"
                        model: sidebar.contactController ? sidebar.contactController.requests : null

                        RequestRow {
                            id: requestRowDelegate
                            objectName: "requestRow_" + requestRowDelegate.requestId
                            width: requestsColumn.width
                            controller: sidebar.contactController
                        }
                    }
                }
            }

            ContactCategory {
                objectName: "favoritesCategory"
                // Superseded by the requests panel above once the contact bridge is
                // enabled; otherwise it keeps its original visibility.
                visible: !(sidebar.contactController && sidebar.contactController.enabled)
                width: parent.width
                label: "Requests"
                favoriteCategory: true
                contactModel: sidebar.controller.contacts
                controller: sidebar.controller
                rowHeight: sidebar.contactRowHeight
            }

            ContactCategory {
                objectName: "contactsCategory"
                width: parent.width
                label: "Chats"
                favoriteCategory: false
                contactModel: sidebar.controller.contacts
                controller: sidebar.controller
                rowHeight: sidebar.contactRowHeight
            }
        }

        Text {
            objectName: "noContactsFound"
            anchors.top: parent.top
            anchors.topMargin: 30
            anchors.horizontalCenter: parent.horizontalCenter
            width: parent.width - 40
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            readonly property bool liveEmpty:
                sidebar.contactController && sidebar.contactController.enabled
                && sidebar.controller.searchQuery.length === 0
            visible: sidebar.controller.contacts.favoriteCount
                     + sidebar.controller.contacts.regularCount === 0
                     && !directoryResult.shown && !requestsPanel.visible
            text: liveEmpty ? "No chats yet. Search a username above to add a friend."
                            : "No contacts found"
            color: Theme.textSecondary
            font.family: Theme.uiFont
            font.pixelSize: 14
            renderType: Text.NativeRendering
        }
    }

    Item {
        id: sidebarCallList
        objectName: "sidebarCallList"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: searchArea.bottom
        anchors.bottom: bottomNav.top
        visible: sidebar.controller.navSection === ChatController.NavSection.Call

        // Empty state until call history exists. When callCount grows, a call
        // ListView slots in here in place of this placeholder.
        Text {
            objectName: "noCallsYet"
            anchors.top: parent.top
            anchors.topMargin: 30
            anchors.horizontalCenter: parent.horizontalCenter
            visible: sidebar.controller.callCount === 0
            text: "No calls yet."
            color: Theme.textSecondary
            font.family: Theme.uiFont
            font.pixelSize: 14
            renderType: Text.NativeRendering
        }
    }

    // Settings section: a selectable list of setting categories in the sidebar
    // list style. The selected category drives the detail pane on the right.
    Item {
        id: settingsCategoryList
        objectName: "settingsCategoryList"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: searchArea.bottom
        anchors.bottom: bottomNav.top
        visible: sidebar.controller.navSection === ChatController.NavSection.Settings

        Column {
            anchors.top: parent.top
            width: parent.width

            Repeater {
                model: sidebar.controller.settingsCategories

                Item {
                    id: settingsCategoryRow
                    property int rowIndex: index
                    objectName: "settingsCategoryRow_" + rowIndex
                    width: settingsCategoryList.width
                    height: sidebar.contactRowHeight
                    readonly property bool selected:
                        rowIndex === sidebar.controller.currentSettingsCategory

                    Rectangle {
                        anchors.fill: parent
                        visible: settingsCategoryRow.selected
                        color: Theme.navSelected
                    }

                    Text {
                        x: 15
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData
                        color: settingsCategoryRow.selected ? Theme.categoryText
                                                            : Theme.textPrimary
                        font.family: Theme.uiFont
                        font.pixelSize: 16
                        renderType: Text.NativeRendering
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: sidebar.controller.setCurrentSettingsCategory(
                                       settingsCategoryRow.rowIndex)
                    }
                }
            }
        }
    }

    // Bottom navigation. Three equal columns switch the main pane between the
    // conversation view (Chat) and the Call / Settings placeholders. The active
    // section is owned by the controller so this highlight and the pane shown on
    // the right stay in agreement; the badge counts are model-backed and a badge
    // only appears while its count is greater than zero.
    Rectangle {
        id: bottomNav
        objectName: "bottomNav"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 64
        color: Theme.contentBottom

        Row {
            anchors.fill: parent

            Item {
                id: callTab
                objectName: "callTab"
                width: bottomNav.width / 3
                height: bottomNav.height
                readonly property bool active:
                    sidebar.controller.navSection === ChatController.NavSection.Call

                Rectangle {
                    anchors.fill: parent
                    color: callTab.active ? Theme.navSelected : "transparent"
                }

                NavigationIcon {
                    id: callIcon
                    objectName: "callIcon"

                    Image {
                        anchors.centerIn: parent
                        width: 24
                        height: 24
                        source: Qt.resolvedUrl("../../../assets/icons/phone-call" + (Theme.darkMode ? "-dark.svg" : ".svg"))
                        sourceSize: Qt.size(width * 2, height * 2)
                    }

                    Rectangle {
                        objectName: "callBadge"
                        anchors.right: parent.right
                        anchors.rightMargin: -4
                        anchors.top: parent.top
                        anchors.topMargin: -3
                        visible: sidebar.controller.callMissedCount > 0
                        height: 15
                        width: Math.max(15, callBadgeLabel.implicitWidth + 8)
                        radius: height / 2
                        color: "#e0503d"

                        Text {
                            id: callBadgeLabel
                            objectName: "callBadgeLabel"
                            anchors.centerIn: parent
                            text: sidebar.controller.callMissedCount
                            color: "white"
                            font.family: Theme.uiFont
                            font.pixelSize: 10
                            font.bold: true
                        }
                    }
                }

                Text {
                    objectName: "callTabLabel"
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 10
                    text: "Call"
                    color: callTab.active ? Theme.categoryText : Theme.navText
                    font.family: Theme.uiFont
                    font.pixelSize: 13
                    renderType: Text.NativeRendering
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: sidebar.controller.setNavSection(ChatController.NavSection.Call)
                }
            }

            Item {
                id: chatTab
                objectName: "chatTab"
                width: bottomNav.width / 3
                height: bottomNav.height
                readonly property bool active:
                    sidebar.controller.navSection === ChatController.NavSection.Chat

                Rectangle {
                    anchors.fill: parent
                    color: chatTab.active ? Theme.navSelected : "transparent"
                }

                NavigationIcon {
                    id: chatIcon
                    objectName: "chatIcon"

                    // Speech bubble drawn from primitives, matching the house
                    // style of the search magnifier and presence beads.
                    Rectangle {
                        x: 2
                        y: 2
                        width: 22
                        height: 16
                        radius: 6
                        color: Theme.iconInk
                    }
                    Rectangle {
                        x: 5
                        y: 13
                        width: 7
                        height: 7
                        rotation: 45
                        color: Theme.iconInk
                    }

                    Rectangle {
                        objectName: "chatBadge"
                        anchors.right: parent.right
                        anchors.rightMargin: -4
                        anchors.top: parent.top
                        anchors.topMargin: -3
                        visible: sidebar.controller.chatUnreadCount > 0
                        height: 15
                        width: Math.max(15, chatBadgeLabel.implicitWidth + 8)
                        radius: height / 2
                        color: "#e0503d"

                        Text {
                            id: chatBadgeLabel
                            objectName: "chatBadgeLabel"
                            anchors.centerIn: parent
                            text: sidebar.controller.chatUnreadCount
                            color: "white"
                            font.family: Theme.uiFont
                            font.pixelSize: 10
                            font.bold: true
                        }
                    }
                }

                Text {
                    objectName: "chatTabLabel"
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 10
                    text: "Chat"
                    color: chatTab.active ? Theme.categoryText : Theme.navText
                    font.family: Theme.uiFont
                    font.pixelSize: 13
                    renderType: Text.NativeRendering
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: sidebar.controller.setNavSection(ChatController.NavSection.Chat)
                }
            }

            Item {
                id: settingsTab
                objectName: "settingsTab"
                width: bottomNav.width / 3
                height: bottomNav.height
                readonly property bool active:
                    sidebar.controller.navSection === ChatController.NavSection.Settings

                Rectangle {
                    anchors.fill: parent
                    color: settingsTab.active ? Theme.navSelected : "transparent"
                }

                NavigationIcon {
                    objectName: "settingsIcon"

                    Image {
                        anchors.centerIn: parent
                        width: 24
                        height: 24
                        source: Qt.resolvedUrl("../../../assets/icons/settings" + (Theme.darkMode ? "-dark.svg" : ".svg"))
                        sourceSize: Qt.size(width * 2, height * 2)
                    }
                }

                Text {
                    objectName: "settingsTabLabel"
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 10
                    text: "Settings"
                    color: settingsTab.active ? Theme.categoryText : Theme.navText
                    font.family: Theme.uiFont
                    font.pixelSize: 13
                    renderType: Text.NativeRendering
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: sidebar.controller.setNavSection(ChatController.NavSection.Settings)
                }
            }
        }

        Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.rule }
    }

    // Click anywhere else in the window to close the presence picker. Both
    // this catcher and the picker live on the window's content item, above
    // the sidebar and the conversation pane alike, so "anywhere else" really
    // is anywhere; their z keeps the picker above the catcher.
    MouseArea {
        parent: sidebar.Window.contentItem
        anchors.fill: parent
        visible: presenceMenu.visible
        z: 19
        onClicked: presenceMenu.visible = false
    }

    // The presence picker: every presence a user can choose, the current one
    // highlighted. Opens under the status line, its beads lined up under the
    // one after the name, and is drawn over everything.
    Item {
        id: presenceMenu
        objectName: "localPresenceMenu"
        parent: sidebar.Window.contentItem
        visible: false
        z: 20
        x: sidebar.x + Math.min(localBead.x - 14, sidebar.width - width - 8)
        y: sidebar.y + localUser.y + statusEditor.y + statusEditor.height + 4
        width: 156
        height: presenceColumn.height + 12

        Rectangle {
            anchors.fill: parent
            radius: 5
            color: Theme.contentBackground
            border.width: 1
            border.color: Theme.inputBorder
        }
        // Aero inset along the top edge, matching the app's framed surfaces.
        Rectangle { x: 6; y: 1; width: parent.width - 12; height: 1; color: Theme.gloss }

        Column {
            id: presenceColumn
            x: 6
            y: 6
            width: parent.width - 12

            Repeater {
                model: [
                    { value: 0, label: "Available" },
                    { value: 1, label: "Away" },
                    { value: 3, label: "Busy" },
                    { value: 2, label: "Appear offline" }
                ]

                Item {
                    id: presenceOption
                    required property var modelData
                    objectName: "presenceOption_" + modelData.value
                    width: presenceColumn.width
                    height: 28
                    readonly property bool current:
                        sidebar.controller.localPresence === modelData.value

                    Rectangle {
                        anchors.fill: parent
                        radius: 3
                        color: optionMouse.containsMouse ? Theme.selectedTop
                             : presenceOption.current ? Theme.navSelected : "transparent"
                    }
                    PresenceBead {
                        x: 8
                        anchors.verticalCenter: parent.verticalCenter
                        beadSize: 11
                        presence: presenceOption.modelData.value
                    }
                    Text {
                        x: 30
                        anchors.verticalCenter: parent.verticalCenter
                        text: presenceOption.modelData.label
                        color: Theme.textPrimary
                        font.family: Theme.uiFont
                        font.pixelSize: 14
                        renderType: Text.NativeRendering
                    }
                    MouseArea {
                        id: optionMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            sidebar.controller.setLocalPresence(presenceOption.modelData.value);
                            presenceMenu.visible = false;
                        }
                    }
                }
            }
        }
    }
}
