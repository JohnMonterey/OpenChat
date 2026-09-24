import QtQuick
import QtQuick.Dialogs
import QtQuick.Window
import OpenChat
import OpenChat.Native

// A profile, as a full-window page over the chat (SPEC §1-§3): the app-owned
// top bar, the CallStrip while a call rings or runs (a profile never hides a
// call), then the body: the editor while you edit your own page, the stub
// card for anyone who is not a contact, else the themed page.
//
// The page owns what must outlive a refresh or a change of layout: the one
// SongPlayer (its song never restarts because a module was rebuilt), the
// picture dialog every "Change picture" opens, the notice line and the Save
// toast. It takes the keyboard when it opens and swallows every pointer
// event, so nothing reaches the chat hidden underneath. Main.qml routes
// Escape, Alt+Left and closing the window here (handleEscape, handleBack,
// requestWindowClose) while no popup of the page is open (popupOpen).
FocusScope {
    id: root
    objectName: "profilePage"

    required property var profiles
    property var chatController: null
    property var contactController: null
    property var callController: null
    // The top bar's Plain style switch: the setting lives in
    // AppearanceSettings, which Main.qml writes.
    signal plainStyleChangeRequested(bool plain)

    // Any menu, popover, picker or dialog of the page or the editor.
    readonly property bool popupOpen: topBar.popupOpen
                                      || (bodyStub.item !== null && bodyStub.item.popupOpen === true)
                                      || (bodyEditor.item !== null && bodyEditor.item.popupOpen === true)
                                      || avatarDialog.visible
    // The open fade (SPEC §17) has finished; at once without animations.
    readonly property bool settled: d.settled
    readonly property bool animationsAllowed: ProfileRenderPolicy.animationsAllowed
    // What the body shows: while editing, the preview's page (the draft, or
    // the draft with the preset being tried on), else the page on screen.
    readonly property var shownPage: profiles.editing ? (profiles.tryOnPreset >= 0 ? profiles.tryOn : profiles.draft)
                                                      : profiles.view
    readonly property alias songPlayer: player
    readonly property alias localAvatarFileDialog: avatarDialog
    readonly property Item editor: bodyEditor.item
    readonly property Item editorBar: topBar.editorBar
    readonly property Item pageView: bodyView.item
    readonly property bool windowShown: root.Window.window !== null
                                        && root.Window.window.visibility !== Window.Hidden
                                        && root.Window.window.visibility !== Window.Minimized

    // SPEC §1.4 steps 3-4: in the editor, unsaved changes ask first and an
    // unchanged draft simply closes the editor; otherwise Back.
    function handleEscape() {
        if (root.profiles.editing) {
            root.leaveEditor(() => root.profiles.endEditing());
            return;
        }
        root.handleBack();
    }
    // Esc outside the editor, Alt+Left and the mouse's Back button.
    function handleBack() {
        if (root.profiles.editing) {
            root.handleEscape();
            return;
        }
        if (root.profiles.depth <= 1)
            root.profiles.back(); // closes: the page's close fade takes over
        else
            root.navigate(() => root.profiles.back());
    }
    // Closing the window: with unsaved changes the leave dialog decides
    // (Keep editing cancels), else `proceed` runs now.
    function requestWindowClose(proceed) {
        if (root.profiles.editing && root.profiles.draftDirty)
            root.leaveEditor(proceed);
        else
            proceed();
    }
    // The editor's leave dialog stands between a dirty draft and `proceed`.
    // Without one (an editor that offers none) the draft is kept anyway: it
    // is autosaved and offered again next time.
    function leaveEditor(proceed) {
        const editor = bodyEditor.item;
        if (root.profiles.draftDirty && editor && typeof editor.requestLeave === "function") {
            editor.requestLeave(proceed);
            return;
        }
        proceed();
    }
    function openChangePicture() {
        avatarDialog.open();
    }
    // A move to another person fades the body out (60 ms), moves, and fades
    // the new person in (60 ms). Moves asked for during the fade queue up.
    function navigate(action) {
        if (!root.animationsAllowed || !d.settled) {
            action();
            return;
        }
        d.pending = d.pending.concat([action]);
        if (fadeOut.running)
            return;
        fadeIn.stop();
        fadeOut.from = body.opacity;
        fadeOut.start();
    }

    QtObject {
        id: d
        property bool settled: false
        property var pending: []
        // 0 → 1 while opening, back to 0 while closing.
        property real reveal: 0

        function runPending() {
            const actions = d.pending;
            d.pending = [];
            for (let i = 0; i < actions.length; ++i)
                actions[i]();
        }
    }

    opacity: d.reveal
    transform: Translate { y: 12 * (1 - d.reveal) }

    Component.onCompleted: {
        root.forceActiveFocus();
        root.reveal();
    }

    // The open motion: fade in and rise 12 px over 140 ms (at once without
    // animations).
    function reveal() {
        closeAnimation.stop();
        if (root.animationsAllowed) {
            openAnimation.from = d.reveal;
            openAnimation.start();
        } else {
            d.reveal = 1;
            d.settled = true;
        }
    }

    NumberAnimation {
        id: openAnimation
        target: d
        property: "reveal"
        to: 1
        duration: 140
        easing.type: Easing.InOutQuad
        onFinished: d.settled = true
    }
    NumberAnimation {
        id: closeAnimation
        target: d
        property: "reveal"
        to: 0
        duration: 120
        easing.type: Easing.InOutQuad
    }
    NumberAnimation {
        id: fadeOut
        target: body
        property: "opacity"
        to: 0
        duration: 60
        easing.type: Easing.InOutQuad
        onFinished: {
            d.runPending();
            fadeIn.start();
        }
    }
    NumberAnimation {
        id: fadeIn
        target: body
        property: "opacity"
        to: 1
        duration: 60
        easing.type: Easing.InOutQuad
    }

    Connections {
        target: root.profiles
        function onNavigationChanged() {
            if (root.profiles.open) {
                // Opened again while it was fading out.
                if (closeAnimation.running || d.reveal < 1)
                    root.reveal();
                return;
            }
            // Closed: the reverse of the open motion while Main keeps the
            // page for its close fade.
            openAnimation.stop();
            if (root.animationsAllowed) {
                closeAnimation.from = d.reveal;
                closeAnimation.start();
            } else {
                d.reveal = 0;
            }
        }
        function onPublished(offline) {
            toast.show(offline ? "Saved. It will be sent when you're back online."
                               : "Saved. Your contacts will see it the next time they open your profile.");
        }
    }

    // Nothing under the page gets a pointer event: not a hover, a wheel or
    // a click. The mouse's Back button goes back.
    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.AllButtons
        onWheel: wheel => wheel.accepted = true
        onClicked: mouse => {
            if (mouse.button === Qt.BackButton)
                root.handleBack();
        }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.contentBackground
    }

    // PgUp/PgDn, Space, Home/End and the arrows scroll the page when nothing
    // inside takes them.
    Keys.onPressed: event => {
        const view = bodyView.item;
        if (!view || root.profiles.editing)
            return;
        const flick = view.flickable;
        const page = flick.height * 0.9;
        const maxY = Math.max(0, flick.contentHeight - flick.height);
        let y = flick.contentY;
        switch (event.key) {
        case Qt.Key_PageDown: y += page; break;
        case Qt.Key_PageUp: y -= page; break;
        case Qt.Key_Space: y += (event.modifiers & Qt.ShiftModifier) ? -page : page; break;
        case Qt.Key_Down: y += 40; break;
        case Qt.Key_Up: y -= 40; break;
        case Qt.Key_Home: y = 0; break;
        case Qt.Key_End: y = maxY; break;
        default: return;
        }
        flick.contentY = Math.max(0, Math.min(y, maxY));
        event.accepted = true;
    }

    ProfileTopBar {
        id: topBar
        width: parent.width
        z: 3
        profiles: root.profiles
        page: root
        onBackRequested: root.handleBack()
        onHistoryChosen: index => root.navigate(() => root.profiles.popTo(index))
        onPlainStyleChangeRequested: plain => root.plainStyleChangeRequested(plain)
    }

    // A ringing or running call stays in reach under the bar (SPEC §1.3).
    Loader {
        id: callStrip
        objectName: "profileCallStripLoader"
        y: topBar.height
        z: 2
        width: parent.width
        height: active ? 40 : 0
        active: !!root.callController && root.callController.inCall === true
        sourceComponent: CallStrip {
            controller: root.callController
            onReturnRequested: {
                root.profiles.closeAll();
                root.callController.showCallChat();
            }
        }
    }

    Item {
        id: body
        objectName: "profilePageBody"
        y: topBar.height + callStrip.height
        width: parent.width
        height: parent.height - y
        clip: true

        Loader {
            id: bodyView
            anchors.fill: parent
            active: !root.profiles.editing && root.profiles.pageState !== Profile.StubPage
            sourceComponent: ProfilePageView {
                page: root.profiles.view
                profiles: root.profiles
                chatController: root.chatController
                contactController: root.contactController
                callController: root.callController
                songPlayer: player
                mode: "view"
                animate: root.settled
                navigate: root.navigate
                onChangePictureRequested: root.openChangePicture()
            }
        }
        Loader {
            id: bodyStub
            anchors.fill: parent
            active: !root.profiles.editing && root.profiles.pageState === Profile.StubPage
            sourceComponent: ProfileStubCard {
                profiles: root.profiles
                contactController: root.contactController
                onLeaveRequested: root.handleBack()
            }
        }
        // The editor (U8). Its inputs are set when it declares them, so the
        // two can grow apart without a load failure.
        Loader {
            id: bodyEditor
            objectName: "profileEditorLoader"
            anchors.fill: parent
            active: root.profiles.editing
            sourceComponent: ProfileEditor {}
            onLoaded: {
                const editor = bodyEditor.item;
                const inputs = {
                    profiles: () => root.profiles,
                    page: () => root,
                    chatController: () => root.chatController,
                    contactController: () => root.contactController,
                    callController: () => root.callController,
                    songPlayer: () => player
                };
                for (const name in inputs) {
                    if (name in editor)
                        editor[name] = Qt.binding(inputs[name]);
                }
            }
        }
    }

    // Refusals ("That picture couldn't be used…") and confirmations
    // ("Copied @michael.r"), one line under the bar, gone after a while.
    Item {
        id: notice
        objectName: "profilePageNotice"
        readonly property string text: root.chatController && root.chatController.profileNotice
                                       && root.chatController.profileNotice.length > 0
                                       ? root.chatController.profileNotice : root.profiles.notice
        readonly property bool fromChat: root.chatController && root.chatController.profileNotice
                                         && root.chatController.profileNotice.length > 0
        visible: text.length > 0
        z: 4
        y: body.y + 10
        x: Math.round((root.width - width) / 2)
        width: Math.min(root.width - 32, noticeText.implicitWidth + 28)
        height: noticeText.implicitHeight + 14

        Accessible.role: Accessible.AlertMessage
        Accessible.name: notice.text

        Rectangle {
            x: 1
            y: 2
            width: parent.width - 2
            height: parent.height
            radius: 5
            color: Theme.tooltipShadowFill
        }
        Rectangle {
            anchors.fill: parent
            radius: 5
            border.width: 1
            border.color: Theme.tooltipBorder
            gradient: Gradient {
                GradientStop { position: 0; color: Theme.tooltipTop }
                GradientStop { position: 0.48; color: Theme.tooltipMid }
                GradientStop { position: 1; color: Theme.tooltipBottom }
            }
        }
        Text {
            id: noticeText
            objectName: "profilePageNoticeText"
            anchors.centerIn: parent
            width: Math.min(implicitWidth, root.width - 60)
            wrapMode: Text.Wrap
            text: notice.text
            textFormat: Text.PlainText
            color: Theme.textPrimary
            font.family: Theme.uiFont
            font.pixelSize: 13
            renderType: Text.NativeRendering
        }
        // Each new line gets its full six seconds.
        onTextChanged: if (text.length > 0) noticeTimer.restart()
        Timer {
            id: noticeTimer
            running: notice.visible
            interval: 6000
            onTriggered: {
                if (notice.fromChat)
                    root.chatController.clearProfileNotice();
                else
                    root.profiles.clearNotice();
            }
        }
    }

    ProfileToast {
        id: toast
        z: 4
        x: Math.round((root.width - width) / 2)
        y: root.height - height - 24
    }

    // The page's one song player: the page (or the editor's preview) shows
    // it, and only a press on play ever starts it. A ringing or running call
    // pauses it; leaving the page, a hidden or minimised window stops it.
    SongPlayer {
        id: player
        objectName: "profileSongPlayer"
        songKey: root.shownPage ? root.shownPage.songKey : ""
        suspended: !!root.callController && root.callController.inCall === true
        active: root.settled && root.profiles.open && root.windowShown
    }

    // Every "Change picture" (your photo, the Your Profile box, the editor's
    // About me tab) opens this one dialog.
    FileDialog {
        id: avatarDialog
        objectName: "localAvatarFileDialog"
        title: "Choose a profile picture"
        fileMode: FileDialog.OpenFile
        nameFilters: ["Images (*.png *.jpg *.jpeg *.bmp *.gif *.webp *.tif *.tiff)", "All files (*)"]
        onAccepted: {
            if (root.chatController)
                root.chatController.setLocalAvatarFromFile(selectedFile);
        }
    }

    Shortcut {
        sequence: "Ctrl+E"
        enabled: root.profiles.isOwnProfile && !root.profiles.editing && !root.popupOpen
        onActivated: root.profiles.beginEditing()
    }
}
