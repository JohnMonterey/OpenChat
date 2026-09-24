import QtQuick
import QtQuick.Window
import OpenChat
import OpenChat.Native

// Hosts a ProfilePage the way Main.qml does, for tst_profilepageqml: a
// window with the chat controller (the reference mock), optional contact and
// call controllers, the page in a Loader that lives while a profile is open,
// and Main's gated Escape / Alt+Left. A scripted call controller stands in
// where a test needs a call the preview seams cannot stage (a call with the
// person on screen), and records what the page asked it to do.
Window {
    id: harness
    width: 860
    height: 680
    visible: true
    color: Theme.contentBackground

    required property var chatController
    property var contactController: null
    property var realCallController: null
    // Use the scripted call controller below instead of the real one.
    property bool useScriptedCalls: false
    readonly property var callController: useScriptedCalls ? scriptedCalls : realCallController
    readonly property var profiles: chatController.profiles
    readonly property Item page: pageLoader.item
    // Every Plain style request the page raised, in order.
    property var plainRequests: []

    // A call controller with the properties and calls the page and the
    // CallStrip use, and a log of the calls made.
    QtObject {
        id: scriptedCalls
        objectName: "scriptedCalls"
        property bool inCall: false
        property bool isRinging: false
        property bool callsAvailable: true
        property string callChatId: ""
        property string peerName: ""
        property bool isGroupCall: false
        property bool callEnded: false
        property bool waitingForOthers: false
        property string waitingText: ""
        property string statusText: "0:12"
        property var log: []
        function record(entry) { log = log.concat([entry]); }
        function callCurrentContact(video) { record(video ? "video" : "voice"); }
        function showCallChat() { record("showCallChat"); }
        function acceptCall() { record("accept"); }
        function declineCall() { record("decline"); }
        function hangUp() { record("hangUp"); }
    }
    readonly property QtObject scripted: scriptedCalls

    Binding {
        target: harness.profiles
        property: "darkMode"
        value: Theme.darkMode
    }

    Loader {
        id: pageLoader
        objectName: "profileLoader"
        anchors.fill: parent
        active: harness.profiles.open
        sourceComponent: ProfilePage {
            profiles: harness.profiles
            chatController: harness.chatController
            contactController: harness.contactController
            callController: harness.callController
            onPlainStyleChangeRequested: plain => {
                harness.plainRequests = harness.plainRequests.concat([plain]);
                harness.profiles.plainStyle = plain;
            }
        }
    }

    // The editor's live preview of the draft (ProfilePreviewFrame's view),
    // at a chosen width on the right, over the page: 0 hides it.
    property int previewWidth: 0
    property string previewTarget: ""
    property var editRequests: []
    readonly property Item preview: previewLoader.item
    Loader {
        id: previewLoader
        objectName: "previewLoader"
        active: harness.previewWidth > 0 && harness.profiles.editing
        z: 10
        x: harness.width - harness.previewWidth
        y: 78
        width: harness.previewWidth
        height: harness.height - y
        sourceComponent: ProfilePageView {
            page: harness.profiles.draft
            profiles: harness.profiles
            mode: "preview"
            songPlayer: harness.page ? harness.page.songPlayer : null
            editingTarget: harness.previewTarget
            onEditRequested: target => harness.editRequests = harness.editRequests.concat([target])
        }
    }

    readonly property bool profileKeysEnabled: harness.profiles.open && pageLoader.item !== null
                                               && !pageLoader.item.popupOpen
    Shortcut {
        sequences: ["Escape"]
        enabled: harness.profileKeysEnabled
        onActivated: pageLoader.item.handleEscape()
    }
    Shortcut {
        sequences: ["Alt+Left"]
        enabled: harness.profileKeysEnabled && !harness.profiles.editing
        onActivated: pageLoader.item.handleBack()
    }

    function setDarkMode(dark) {
        Theme.setDarkMode(dark);
    }
}
