import QtQuick
import OpenChat
import OpenChat.Native

// The profile of someone who is not a contact (SPEC §13): a calm backdrop
// (the Aero Sky gradient for the viewer's mode with the aurora ribbons, no
// bubbles, nothing moving) and one dialog-style card with only what the app
// knows: the name, the @handle once the relay confirms it, why they are here,
// and the actions that really apply to them (ARCH §8.4). No theme, presence,
// mood, song, banner or request age: "This profile is private."
Item {
    id: stub
    objectName: "profileStubCard"
    property var profiles: null
    property var contactController: null
    // Decline and Block leave the stub: the page pops it.
    signal leaveRequested()

    readonly property bool popupOpen: confirm.opened || confirm.visible
    readonly property bool request: profiles !== null && profiles.relationship === Profile.IncomingRequestPerson
    readonly property bool outgoing: profiles !== null && profiles.relationship === Profile.OutgoingRequestPerson
    readonly property string handle: profiles ? profiles.personHandle : ""
    readonly property string first: profiles ? profiles.personFirstName : ""
    // Why this person is here: a request, Search & Find, a Friend Space or a call.
    readonly property string reason: {
        if (!profiles)
            return "search";
        if (request)
            return "request";
        if (profiles.referrerName.length > 0)
            return "friend";
        if (profiles.origin === Profile.FromCall)
            return "call";
        return "search";
    }
    readonly property int lookupState: contactController && contactController.lookupState !== undefined
                                       ? contactController.lookupState : -1
    // The handle this card sent a request to, so the result shows here.
    property string requestedHandle: ""

    // What the actions row offers.
    readonly property string actionMode: {
        if (!profiles)
            return "none";
        if (request)
            return contactController ? "answer" : "none";
        if (outgoing)
            return "sent";
        if (!contactController)
            return "none";
        if (reason === "search") {
            if (lookupState === ContactController.LookupState.Found)
                return "lookup";
            if (lookupState === ContactController.LookupState.RequestSent || lookupState === ContactController.LookupState.RequestPending)
                return "sent";
            return "none";
        }
        if (requestedHandle.length > 0 && requestedHandle === handle)
            return "requested";
        if (handle.length > 0 && !profiles.personHandlePending && contactController.enabled && !profiles.personBlocked)
            return "add";
        return "ask";
    }

    Accessible.role: Accessible.Pane
    Accessible.name: profiles ? profiles.personName : ""

    ProfileBackdrop {
        objectName: "profileStubBackdrop"
        anchors.fill: parent
        kind: Profile.GradientBackground
        color1: Theme.darkMode ? "#16293a" : "#c7e1f5"
        color2: Theme.darkMode ? "#0f1b26" : "#f1f8fd"
        aurora: true
        darkBase: Theme.darkMode
        Accessible.ignored: true
    }

    component Line: Row {
        id: line
        property string glyph: "lock"
        property string title: ""
        property string text: ""
        width: parent ? parent.width : 0
        spacing: 10
        ProfileGlyph {
            y: 2
            width: 16
            height: 16
            kind: line.glyph
            ink: Theme.iconInk
            stroke: 1.5
        }
        Column {
            width: line.width - 26
            spacing: 3
            Text {
                visible: line.title.length > 0
                width: parent.width
                wrapMode: Text.Wrap
                text: line.title
                textFormat: Text.PlainText
                color: Theme.textPrimary
                font.family: Theme.uiFont
                font.pixelSize: 15
                font.bold: true
                renderType: Text.NativeRendering
            }
            Text {
                width: parent.width
                wrapMode: Text.Wrap
                text: line.text
                textFormat: Text.PlainText
                color: Theme.textSecondaryStrong
                font.family: Theme.uiFont
                font.pixelSize: 13
                lineHeight: 1.12
                renderType: Text.NativeRendering
            }
        }
    }

    Item {
        id: cardFrame
        objectName: "profileStubCardFrame"
        width: Math.min(520, stub.width - 32)
        height: card.height
        x: Math.round((stub.width - width) / 2)
        y: Math.max(24, Math.round((stub.height - height) * 0.32))

        // The dialog ring shadow.
        Rectangle {
            anchors.fill: parent
            anchors.margins: -1
            radius: 7
            color: "transparent"
            border.width: 1
            border.color: Theme.darkMode ? "#55000000" : "#1a1b3a58"
        }
        Rectangle {
            x: 4
            y: card.height + 1
            width: card.width - 8
            height: 2
            radius: 1
            color: Theme.darkMode ? "#40000000" : "#141b3a58"
        }

        Rectangle {
            id: card
            width: parent.width
            height: column.y + column.height + 24
            radius: 6
            color: Theme.contentBackground
            border.width: 1
            border.color: Theme.inputBorder
            Rectangle {
                x: 1
                y: 1
                width: parent.width - 2
                height: 136
                radius: 5
                gradient: Gradient {
                    GradientStop { position: 0; color: Theme.headerTop }
                    GradientStop { position: 1; color: Theme.contentBackground }
                }
            }
            Rectangle { x: 6; y: 1; width: parent.width - 12; height: 1; color: Theme.glossStrong }
            Rectangle { x: 6; y: parent.height - 2; width: parent.width - 12; height: 1; color: Theme.gloss }

            Column {
                id: column
                x: 24
                y: 22
                width: parent.width - 48
                spacing: 18

                // Who: the picture, the name, the confirmed @handle and why.
                Row {
                    spacing: 18
                    Avatar {
                        width: 96
                        height: 96
                        cornerRadius: 6
                        avatarKey: stub.profiles ? stub.profiles.personAvatarKey : "userpfp_none"
                    }
                    Column {
                        anchors.verticalCenter: parent.verticalCenter
                        width: column.width - 96 - 18
                        spacing: 6
                        Text {
                            objectName: "profileStubName"
                            width: parent.width
                            elide: Text.ElideRight
                            text: stub.profiles ? stub.profiles.personName : ""
                            textFormat: Text.PlainText
                            color: Theme.textPrimary
                            font.family: Theme.uiFont
                            font.pixelSize: 24
                            renderType: Text.NativeRendering
                        }
                        Text {
                            objectName: "profileStubHandle"
                            visible: text.length > 0
                            width: parent.width
                            elide: Text.ElideRight
                            text: stub.handle.length > 0 ? "@" + stub.handle
                                  : stub.profiles && stub.profiles.personHandlePending ? "looking up…" : ""
                            textFormat: Text.PlainText
                            color: Theme.textSecondaryStrong
                            font.family: Theme.uiFont
                            font.pixelSize: 14
                            renderType: Text.NativeRendering
                        }
                        Item {
                            width: 1
                            height: 2
                        }
                        Rectangle {
                            objectName: "profileStubRelation"
                            width: relationRow.implicitWidth + 20
                            height: 24
                            radius: 12
                            color: Theme.panelBackground
                            border.width: 1
                            border.color: Theme.inputBorder
                            Row {
                                id: relationRow
                                anchors.centerIn: parent
                                spacing: 6
                                ProfileGlyph {
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 13
                                    height: 13
                                    kind: "person"
                                    ink: Theme.categoryText
                                }
                                Text {
                                    objectName: "profileStubRelationText"
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: stub.reason === "request" ? "Wants to add you as a contact"
                                          : stub.reason === "friend" ? "In " + stub.profiles.referrerName + "'s Friend Space"
                                          : stub.reason === "call" ? "In this call with you" : "Found in Search & Find"
                                    textFormat: Text.PlainText
                                    color: Theme.categoryText
                                    font.family: Theme.uiFont
                                    font.pixelSize: 12
                                    renderType: Text.NativeRendering
                                }
                            }
                        }
                    }
                }

                // Only real actions (ARCH §8.4). Tab: primary, secondary, Block.
                Item {
                    objectName: "profileStubActions"
                    visible: stub.actionMode !== "none"
                    width: parent.width
                    height: 36

                    Row {
                        visible: stub.actionMode === "answer"
                        spacing: 10
                        ProfileRequestButton {
                            objectName: "profileAcceptButton"
                            accept: true
                            accessibleName: "Accept " + (stub.profiles ? stub.profiles.personName : "")
                            onClicked: stub.contactController.accept(stub.profiles.requestId)
                        }
                        ProfileRequestButton {
                            objectName: "profileDeclineButton"
                            accept: false
                            accessibleName: "Decline " + (stub.profiles ? stub.profiles.personName : "")
                            onClicked: {
                                stub.contactController.decline(stub.profiles.requestId);
                                stub.leaveRequested();
                            }
                        }
                    }
                    Text {
                        id: blockLink
                        objectName: "profileBlockLink"
                        visible: stub.actionMode === "answer"
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        text: "Block " + (stub.handle.length > 0 ? "@" + stub.handle : stub.first)
                        textFormat: Text.PlainText
                        color: Theme.errorText
                        font.family: Theme.uiFont
                        font.pixelSize: 13
                        font.underline: true
                        renderType: Text.NativeRendering
                        activeFocusOnTab: visible

                        Accessible.role: Accessible.Button
                        Accessible.name: text
                        Accessible.onPressAction: confirm.open()
                        Keys.onPressed: event => {
                            if (event.key === Qt.Key_Space || event.key === Qt.Key_Return
                                    || event.key === Qt.Key_Enter) {
                                confirm.open();
                                event.accepted = true;
                            }
                        }
                        ProfileFocusRing {
                            anchors.fill: parent
                            shown: blockLink.activeFocus
                            radius: 2
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: confirm.open()
                        }
                    }

                    ProfileDefaultButton {
                        objectName: "profileSendRequestButton"
                        visible: stub.actionMode === "lookup" || stub.actionMode === "add"
                        height: 34
                        glyph: "plus"
                        label: "Send contact request"
                        onClicked: {
                            if (stub.actionMode === "lookup") {
                                stub.contactController.requestLookup();
                            } else {
                                stub.requestedHandle = stub.handle;
                                stub.contactController.addByHandle(stub.handle);
                            }
                        }
                    }
                    Text {
                        objectName: "profileStubStatus"
                        visible: stub.actionMode === "sent" || stub.actionMode === "requested"
                                 || stub.actionMode === "ask"
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width
                        wrapMode: Text.Wrap
                        text: {
                            if (stub.actionMode === "ask")
                                return "Ask " + stub.first + " for their handle to add them.";
                            if (stub.actionMode === "requested" && stub.contactController) {
                                if (stub.contactController.status === ContactController.Status.Error)
                                    return stub.contactController.statusMessage;
                                if (stub.contactController.status === ContactController.Status.Working)
                                    return "Sending request…";
                            }
                            return "Request sent";
                        }
                        textFormat: Text.PlainText
                        color: stub.actionMode === "requested" && stub.contactController
                               && stub.contactController.status === ContactController.Status.Error
                               ? Theme.errorText : Theme.textSecondaryStrong
                        font.family: Theme.uiFont
                        font.pixelSize: 13
                        renderType: Text.NativeRendering
                    }
                }

                Rectangle {
                    width: parent.width
                    height: 1
                    color: Theme.softRule
                }

                Line {
                    glyph: "lock"
                    title: "This profile is private."
                    text: stub.request
                          ? "OpenChat profile pages are only shared between contacts. If you accept, you'll see "
                            + stub.first + "'s page, and " + stub.first + " will see yours."
                          : "OpenChat profile pages are only shared between contacts. Once you're contacts, you'll see each other's pages."
                }
                Line {
                    visible: stub.request
                    glyph: "shield"
                    text: "Only accept requests from people you know. After accepting, you can compare safety numbers to be sure it's really them."
                }
            }
        }
    }

    ProfileConfirmPopup {
        id: confirm
        title: "Block " + (stub.handle.length > 0 ? "@" + stub.handle : stub.first) + "?"
        body: "Their request goes away, and OpenChat ignores any new request from them."
        confirmLabel: "Block"
        returnFocus: blockLink
        onConfirmed: {
            stub.contactController.block(stub.profiles.requestId);
            stub.leaveRequested();
        }
    }
}
