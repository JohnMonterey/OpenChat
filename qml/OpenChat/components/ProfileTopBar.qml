import QtQuick
import OpenChat
import OpenChat.Native

// The app-owned 48 px bar over every profile (SPEC §2). It never takes the
// page's theme, so the way out and who this is can't be skinned, hidden or
// spoofed: Back, the picture, the roster name, real presence (never on a
// stub) and the relay-confirmed @handle ("looking up…" meanwhile). The right
// side depends on the page: a contact's custom page offers the viewer's Plain
// style switch; your own page "Edit profile"; a stub nothing; the editor
// replaces the whole bar with its own (ProfileEditorBar).
Item {
    id: bar
    objectName: "profileTopBar"
    property var profiles: null
    // The ProfilePage this bar belongs to (the editor bar reaches the page
    // through it: handleEscape, the editor's state).
    property Item page: null
    signal plainStyleChangeRequested(bool plain)
    signal backRequested()
    signal historyChosen(int index)

    readonly property string mode: !profiles ? "stub"
                                    : profiles.editing ? "edit"
                                    : profiles.pageState === Profile.StubPage ? "stub"
                                    : profiles.isOwnProfile ? "own" : "contact"
    readonly property bool popupOpen: backChip.menuOpen
                                      || (editorBarLoader.item !== null && editorBarLoader.item.popupOpen === true)
    readonly property Item editorBar: editorBarLoader.item
    // "Online Now!" only from real presence; an unreachable contact is offline.
    readonly property int presence: !profiles ? 2 : (profiles.personPresence === 0 && !profiles.personOnline)
                                                    ? 2 : profiles.personPresence

    implicitHeight: 48
    height: 48

    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0; color: Theme.headerTop }
            GradientStop { position: 1; color: Theme.headerBottom }
        }
    }
    Rectangle {
        width: parent.width
        height: 1
        color: Theme.gloss
    }
    Rectangle {
        y: parent.height - 1
        width: parent.width
        height: 1
        color: Theme.rule
    }

    Item {
        id: viewerSide
        anchors.fill: parent
        visible: bar.mode !== "edit"

        ProfileBackChip {
            id: backChip
            x: 10
            anchors.verticalCenter: parent.verticalCenter
            profiles: bar.profiles
            onBackRequested: bar.backRequested()
            onHistoryChosen: index => bar.historyChosen(index)
        }
        Rectangle {
            id: divider
            x: backChip.x + backChip.width + 11
            y: 12
            width: 1
            height: 24
            color: Theme.rule
        }

        // Who this is, as the app knows them (never from their page).
        Item {
            id: identity
            x: divider.x + 13
            width: rightSide.x - x - 12
            height: parent.height
            Avatar {
                id: picture
                anchors.verticalCenter: parent.verticalCenter
                width: 30
                height: 30
                cornerRadius: 4
                avatarKey: bar.profiles ? bar.profiles.personAvatarKey : "userpfp_none"
                Accessible.ignored: true
            }
            Text {
                id: nameText
                objectName: "profileTopBarName"
                x: picture.width + 9
                y: handleText.visible ? 6 : Math.round((parent.height - height) / 2)
                width: Math.min(implicitWidth, identity.width - x - (bead.visible ? bead.width + 7 : 0))
                elide: Text.ElideRight
                text: bar.profiles ? bar.profiles.personName : ""
                textFormat: Text.PlainText
                color: Theme.textPrimary
                font.family: Theme.uiFont
                font.pixelSize: 15
                renderType: Text.NativeRendering
            }
            PresenceBead {
                id: bead
                objectName: "profileTopBarPresence"
                visible: bar.mode === "contact" || bar.mode === "own"
                x: nameText.x + nameText.width + 7
                anchors.verticalCenter: nameText.verticalCenter
                anchors.verticalCenterOffset: 1
                beadSize: 10
                presence: bar.presence
            }
            Text {
                id: handleText
                objectName: "profileTopBarHandle"
                visible: text.length > 0
                x: nameText.x
                y: 26
                width: identity.width - x
                elide: Text.ElideRight
                text: !bar.profiles ? ""
                      : bar.profiles.personHandle.length > 0 ? "@" + bar.profiles.personHandle
                      : bar.profiles.personHandlePending ? "looking up…" : ""
                textFormat: Text.PlainText
                color: Theme.textSecondaryStrong
                font.family: Theme.uiFont
                font.pixelSize: 12
                renderType: Text.NativeRendering
            }
        }

        Row {
            id: rightSide
            anchors.right: parent.right
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8

            // A contact's custom page: the viewer's own, persistent setting.
            Item {
                id: plainStyle
                visible: bar.mode === "contact" && bar.profiles.pageState === Profile.CustomPage
                width: plainRow.implicitWidth
                height: 26
                readonly property bool hovered: plainHover.hovered
                HoverHandler { id: plainHover }
                Row {
                    id: plainRow
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 9
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: "Plain style"
                        textFormat: Text.PlainText
                        color: Theme.textSecondaryStrong
                        font.family: Theme.uiFont
                        font.pixelSize: 13
                        renderType: Text.NativeRendering
                    }
                    AeroSwitch {
                        id: plainSwitch
                        objectName: "profilePlainStyleSwitch"
                        anchors.verticalCenter: parent.verticalCenter
                        width: 48
                        height: 26
                        checked: bar.profiles ? bar.profiles.plainStyle : false
                        accessibleName: "Show profiles in plain style"
                        onToggled: checked => bar.plainStyleChangeRequested(checked)
                    }
                }
            }

            // Your own page: the way into the editor (Ctrl+E does the same).
            ProfileDefaultButton {
                objectName: "profileEditButton"
                visible: bar.mode === "own"
                anchors.verticalCenter: parent.verticalCenter
                glyph: "pencil"
                label: "Edit profile"
                accessibleName: "Edit your profile"
                onClicked: bar.profiles.beginEditing()
            }
        }
    }

    ProfileTip {
        objectName: "profilePlainStyleTip"
        parent: bar
        visible: plainStyle.visible && plainStyle.hovered
        maxWidth: 292
        width: 292
        x: bar.width - width - 8
        y: bar.height - 4
        // At the switch: the bar's right margin (12) and half the switch in
        // from the bar's edge, which is 8 px past the tip's.
        noseX: width + 8 - 12 - plainSwitch.width / 2
        title: "Show profiles in plain style"
        text: "Shows everyone's profile in OpenChat's own look, with the same words, pictures and friends. "
              + "Applies to every profile until you turn it off. Also in Settings › Appearance."
    }

    // The editor's bar (ProfileEditorBar, SPEC §2 edit mode) over the whole
    // bar. Its inputs are set when they exist, so the page and the editor can
    // grow apart without a load failure.
    Loader {
        id: editorBarLoader
        objectName: "profileEditorBarLoader"
        anchors.fill: parent
        active: bar.mode === "edit"
        sourceComponent: ProfileEditorBar {}
        onLoaded: {
            const editorBar = editorBarLoader.item;
            if ("profiles" in editorBar)
                editorBar.profiles = Qt.binding(() => bar.profiles);
            if ("page" in editorBar)
                editorBar.page = Qt.binding(() => bar.page);
            if ("editor" in editorBar)
                editorBar.editor = Qt.binding(() => bar.page && bar.page.editor !== undefined ? bar.page.editor : null);
        }
    }
}
