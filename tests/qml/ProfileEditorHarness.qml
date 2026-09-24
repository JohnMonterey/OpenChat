import QtQuick
import QtQuick.Dialogs
import OpenChat
import OpenChat.Native

// Hosts the owner's editor the way the profile page does (tst_profileeditor).
//
// The Item "profilePage" stands in for the page kit's ProfilePage with the
// frozen contract the editor relies on (ARCH §8.1): the `profiles` and
// controller inputs, `popupOpen`, handleEscape() / requestWindowClose(), the
// page's single SongPlayer ("profileSongPlayer", following the page on
// screen: the preview's while editing) and the picture FileDialog
// ("localAvatarFileDialog"). Like ProfileTopBar and ProfilePage it creates
// ProfileEditorBar and ProfileEditor with no properties at all while
// `profiles.editing`, so they find all of it themselves. The Escape shortcut
// is Main.qml's single gated one.
Window {
    id: window

    required property var chatController
    readonly property var profiles: chatController.profiles
    readonly property Item editor: editorLoader.item
    readonly property Item bar: barLoader.item
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

    Item {
        id: page
        objectName: "profilePage"
        anchors.fill: parent

        property var profiles: window.profiles
        property var chatController: window.chatController
        property var contactController: null
        property var callController: null
        readonly property bool popupOpen: window.editor !== null && window.editor.popupOpen

        // SPEC §1.4 steps 3–4 for the editor; the page kit pops otherwise.
        function handleEscape() {
            if (profiles.editing)
                window.editor.requestLeave(() => profiles.endEditing());
            else
                profiles.back();
        }
        function requestWindowClose(proceed) {
            if (profiles.editing)
                window.editor.requestLeave(proceed);
            else
                proceed();
        }

        SongPlayer {
            objectName: "profileSongPlayer"
            songKey: !page.profiles.editing ? page.profiles.view.songKey
                     : page.profiles.tryOnPreset >= 0 ? page.profiles.tryOn.songKey : page.profiles.draft.songKey
        }
        FileDialog {
            objectName: "localAvatarFileDialog"
            title: "Choose a picture"
            nameFilters: ["Images (*.png *.jpg *.jpeg *.bmp *.gif *.webp)"]
            onAccepted: page.chatController.setLocalAvatarFromFile(selectedFile)
        }

        Loader {
            id: barLoader
            width: parent.width
            height: 48
            active: page.profiles.editing
            sourceComponent: Component { ProfileEditorBar {} }
        }
        Loader {
            id: editorLoader
            y: 48
            width: parent.width
            height: parent.height - 48
            active: page.profiles.editing
            focus: true
            sourceComponent: Component { ProfileEditor {} }
        }
    }

    Shortcut {
        sequences: ["Escape"]
        enabled: window.profiles.open && !page.popupOpen
        onActivated: page.handleEscape()
    }

    function closeWindow() {
        closeOutcome = "";
        page.requestWindowClose(() => window.closeOutcome = "closed");
    }
}
