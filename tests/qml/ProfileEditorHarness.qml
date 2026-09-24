import QtQuick
import OpenChat
import OpenChat.Native

// Hosts the owner's editor in the real ProfilePage, the way Main.qml hosts
// the page (tst_profileeditor): the page in a Loader that lives while a
// profile is open, Main's single gated Escape, and Main's onClosing, which
// asks the page before the window goes (requestWindowClose). The page creates
// ProfileEditorBar (through ProfileTopBar) and ProfileEditor with no
// properties at all while `profiles.editing`, so the editor tests run through
// the page's own handleEscape / leaveEditor, its editor wiring, its single
// SongPlayer ("profileSongPlayer") and its picture FileDialog.
Window {
    id: window

    required property var chatController
    readonly property var profiles: chatController.profiles
    readonly property Item page: pageLoader.item
    readonly property Item editor: page ? page.editor : null
    readonly property Item bar: page ? page.editorBar : null
    // What a closing window did: "" until requestWindowClose went on.
    property string closeOutcome: ""

    width: 1024
    height: 768
    visible: true
    color: Theme.contentBackground

    Binding {
        target: window.profiles
        property: "darkMode"
        value: Theme.darkMode
    }

    Loader {
        id: pageLoader
        objectName: "profileLoader"
        anchors.fill: parent
        active: window.profiles.open
        focus: true
        sourceComponent: ProfilePage {
            profiles: window.profiles
            chatController: window.chatController
        }
    }

    Shortcut {
        sequences: ["Escape"]
        enabled: window.profiles.open && window.page !== null && !window.page.popupOpen
        onActivated: window.page.handleEscape()
    }

    // Main.qml's onClosing: the page decides, and `proceed` closes.
    function closeWindow() {
        closeOutcome = "";
        if (page)
            page.requestWindowClose(() => window.closeOutcome = "closed");
        else
            closeOutcome = "closed";
    }
}
