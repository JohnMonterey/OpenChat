import QtQuick
import OpenChat

// The call, seen from any conversation other than its own. The call surface
// belongs to the conversation the call is on and replaces only that header;
// everywhere else this one line keeps the call in reach: who it is with, how
// long it has run (or that it is ringing), and the way back to it — or, for a
// call that is ringing, the answer and the refusal right here.
Item {
    id: strip
    objectName: "callStrip"
    required property var controller
    // Raised by Return / Open: the window opens the call's conversation.
    signal returnRequested
    readonly property bool ringing: controller.isRinging === true
    readonly property string who: controller.peerName !== undefined ? controller.peerName : ""
    implicitHeight: 40

    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0; color: Theme.callBackdropTop }
            GradientStop { position: 1; color: Theme.callBackdropBottom }
        }
    }

    Text {
        id: line
        objectName: "callStripText"
        anchors.left: parent.left
        anchors.leftMargin: 18
        anchors.right: actions.left
        anchors.rightMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        elide: Text.ElideRight
        text: strip.ringing
              ? (strip.controller.isGroupCall ? "Incoming group call · " : "Incoming call from ")
                + strip.who
              : (strip.controller.callEnded ? "Call ended · " + strip.who
                 : "In call with " + strip.who + " · "
                   + (strip.controller.waitingForOthers === true
                      ? strip.controller.waitingText : strip.controller.statusText))
        color: Theme.textPrimary
        font.family: Theme.uiFont
        font.pixelSize: 13
        renderType: Text.NativeRendering
    }

    Row {
        id: actions
        anchors.right: parent.right
        anchors.rightMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        spacing: 8

        CallActionButton {
            objectName: "callStripAnswerButton"
            visible: strip.ringing
            height: 26
            label: "Answer"
            accent: "accept"
            onClicked: strip.controller.acceptCall()
        }
        CallActionButton {
            objectName: "callStripDeclineButton"
            visible: strip.ringing
            height: 26
            label: "Decline"
            accent: "end"
            onClicked: strip.controller.declineCall()
        }
        CallActionButton {
            objectName: "callStripReturnButton"
            height: 26
            label: strip.ringing ? "Open" : "Return"
            accent: "neutral"
            onClicked: strip.returnRequested()
        }
        CallActionButton {
            objectName: "callStripEndButton"
            visible: !strip.ringing && !strip.controller.callEnded
            height: 26
            label: "End call"
            accent: "end"
            onClicked: strip.controller.hangUp()
        }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: Theme.rule
    }
}
