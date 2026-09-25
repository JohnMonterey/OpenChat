import QtQuick
import OpenChat
import OpenChat.Native

Item {
    id: delegateRoot
    required property int deliveryState
    signal retryRequested(string messageBody)
    required property int direction
    required property string body
    required property string timestamp
    required property int kind
    required property string dateLabel
    required property bool showDateDivider
    // Who sent an incoming message in a group chat; empty in a one-to-one
    // chat, where the bubble alone says.
    required property string senderName
    // The rest of what the model knows about a message. Optional, so a bubble
    // made on its own (a preview, a test) needs only the roles above.
    property string stableId: ""
    property bool edited: false
    property bool editable: false
    property string replyToId: ""
    property string quotedSender: ""
    property string quotedBody: ""
    // This device is changing the text in the composer: the bubble says so in
    // place of the text. Everyone else still sees the message as it was.
    property bool editing: false
    // An attachment (kind 4, its caption the body): what it is, from its
    // descriptor, and how its transfer stands. The bubble draws it with
    // AttachmentBlock, loaded only for such a message, so a bubble on its own
    // never needs the media types.
    property int attachmentKind: 0
    property string fileName: ""
    property string mimeType: ""
    property real byteCount: 0
    property string sizeText: ""
    property int mediaWidth: 0
    property int mediaHeight: 0
    property real durationMs: 0
    property var peaks: []
    property int transferState: 1
    property int transferReason: 0
    property real transferProgress: 0
    property string transferText: ""
    property bool hasPreview: false
    property int previewRevision: 0
    property bool canCancel: false
    property bool canRetry: false
    property bool canSave: false
    // The chat's controller (for the attachment's media) and its one audio
    // player; null for a bubble on its own.
    property var chatController: null
    property var chatAudio: null
    // The actions under a hovered message, and a click on a reply's quote.
    signal copyRequested
    signal editRequested
    signal replyRequested
    signal quoteActivated
    // An attachment's: a photo or video asked to be opened large (from
    // `source`, the picture in the bubble), Save, and stopping its sending.
    signal mediaOpened(Item source)
    signal saveRequested
    signal cancelRequested
    // Selecting text gives the message the keyboard, for Ctrl+C. Anything
    // typed then is meant for the composer: it goes there, with the keyboard
    // (empty text for Esc).
    signal typed(string text)
    readonly property bool outgoing: direction === 1
    readonly property bool eventRow: kind === 2 || kind === 3
    readonly property bool callEvent: kind === 3
    readonly property bool showSender: !eventRow && !outgoing && senderName.length > 0
    readonly property bool attachment: kind === 4
    readonly property bool isReply: (kind === 0 || attachment) && (quotedSender.length > 0 || quotedBody.length > 0)
    readonly property string shownText: editing ? "editing..." : body
    // Who sent an incoming message (account hex), to wear their skin.
    property string senderAccount: ""
    // The collectible skin this bubble wears: the local user's own messages
    // wear the one equipped here, everyone else's the one they wear (from the
    // relay). Event rows never wear one. Empty means the classic bubble.
    property string bubbleSkin: eventRow ? ""
        : outgoing ? AppearanceSettings.bubbleSkin
        : PeerCosmetics.revision >= 0 && senderAccount.length > 0 ? PeerCosmetics.item(senderAccount, "bubble")
        : ""
    readonly property real senderHeight: showSender ? 18 : 0
    // A bubble hugs its text and only widens for a longer message: up to most
    // of the pane, but never past about eighty characters a line. Text that
    // wraps sizes the bubble by its widest line, not by the limit.
    readonly property real maximumBubbleWidth: Math.min(720, width * 0.72)
    readonly property real bubbleTailWidth: 9
    readonly property real bodyLeadingInset: outgoing ? 0 : bubbleTailWidth
    readonly property real bodyTrailingInset: outgoing ? bubbleTailWidth : 0
    readonly property real contentPadding: 14
    readonly property real contentLeftInset: bodyLeadingInset + contentPadding
    readonly property real contentRightInset: bodyTrailingInset + contentPadding
    readonly property real horizontalContentInset: contentLeftInset + contentRightInset
    // A reply's quote sits above its text; a short answer to a long message
    // still leaves the quote room to be read.
    readonly property real quoteHeight: isReply ? 40 : 0
    // An attachment's block. A photo or a video keeps its own shape (4:3
    // while unknown), held between a tall 2:5 and a wide 5:2 and fitted into
    // 320 x 320, never narrower than 140; it sits 4 px inside the bubble's
    // body. A sound or a file is a row as wide as a short message, inside the
    // text's padding. Either way the bubble is as wide as the block and a
    // caption wraps to it; without a caption a picture's bubble ends at the
    // picture (the time rides on it) and a row's at the time under it.
    readonly property bool visualMedia: attachment && (attachmentKind === 1 || attachmentKind === 2)
    readonly property bool hasCaption: attachment && body.length > 0
    readonly property real mediaAspect: mediaWidth > 0 && mediaHeight > 0
        ? Math.max(0.4, Math.min(2.5, mediaWidth / mediaHeight)) : 4 / 3
    readonly property real mediaBoxWidth: !attachment ? 0
        : visualMedia
          ? Math.round(Math.min(maximumBubbleWidth - bubbleTailWidth - 8,
                                Math.max(140, Math.min(320, 320 * mediaAspect))))
          : Math.min(260, maximumBubbleWidth - horizontalContentInset)
    readonly property real mediaBoxHeight: !attachment ? 0
        : visualMedia ? Math.round(Math.min(320, mediaBoxWidth / mediaAspect))
        : attachmentKind === 3 ? 40 : 44
    readonly property real mediaTop: isReply ? 50 : visualMedia ? 4 : 11
    readonly property real mediaBottom: mediaTop + mediaBoxHeight
    readonly property real preferredBubbleWidth: kind === 1
        ? 158
        : attachment
          ? mediaBoxWidth + (visualMedia ? bubbleTailWidth + 8 : horizontalContentInset)
          : Math.max(messageBody.paintedWidth + horizontalContentInset,
                     messageTime.implicitWidth + horizontalContentInset,
                     isReply ? Math.min(quote.naturalWidth, 280) + horizontalContentInset : 0)
    readonly property real bubbleWidth: Math.min(maximumBubbleWidth, preferredBubbleWidth)
    readonly property real bubbleHeight: kind === 1
        ? 54
        : attachment
          ? mediaBottom + (hasCaption
                           ? (visualMedia ? 7 : 6) + messageBody.paintedHeight + messageTime.implicitHeight + 10
                           : visualMedia ? 4 : messageTime.implicitHeight + 10)
          : Math.max(54, quoteHeight + messageBody.paintedHeight + messageTime.implicitHeight + 21)
    // The bubble, for an attachment block to take its skin's colours from.
    readonly property BubbleBackground bubbleItem: bubble
    readonly property real dateSectionHeight: showDateDivider ? 64 : 0
    // Under the bubble, in order: the retry prompt of a failed send, "edited",
    // and the actions the pointer brings up. The actions live in the gap every
    // message leaves before the next one, so they always keep their room and
    // showing them moves nothing.
    readonly property real retryHeight: retryText.visible ? 20 : 0
    readonly property real editedHeight: editedLabel.visible ? editedLabel.implicitHeight + 1 : 0
    readonly property real actionRowHeight: 20
    readonly property real footerY: bubble.y + bubble.height

    implicitHeight: dateSectionHeight + (eventRow ? eventLabel.implicitHeight + 16
        : senderHeight + bubbleHeight + retryHeight + editedHeight + actionRowHeight)
    // A hovered message stays above its neighbours while its actions show.
    z: actionHover.hovered ? 1 : 0

    HoverHandler {
        id: actionHover
        enabled: !delegateRoot.eventRow
    }

    Text {
        objectName: "messageSender"
        visible: delegateRoot.showSender
        x: bubble.x + delegateRoot.contentLeftInset
        y: delegateRoot.dateSectionHeight + 2
        width: delegateRoot.maximumBubbleWidth
        elide: Text.ElideRight
        text: delegateRoot.senderName
        textFormat: Text.PlainText
        color: Theme.categoryText
        font.family: Theme.uiFont
        font.pixelSize: 12
        font.bold: true
        renderType: Text.NativeRendering
    }

    Text {
        id: retryText
        objectName: "messageRetry"
        // An attachment is tried again as a new one (canRetry covers a send
        // that failed and a transfer that did).
        visible: !delegateRoot.eventRow && delegateRoot.outgoing
                 && (delegateRoot.attachment ? delegateRoot.canRetry : delegateRoot.deliveryState === 6)
        anchors.right: bubble.right
        anchors.rightMargin: delegateRoot.bubbleTailWidth
        y: delegateRoot.footerY + 4
        text: "Not sent. Try again"
        color: Theme.retryText
        font.family: Theme.uiFont
        font.pixelSize: 12
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: delegateRoot.retryRequested(delegateRoot.body)
        }
    }

    // The text as one unbroken line, measured the way messageBody lays it out,
    // so a message only wraps once it outgrows the widest bubble.
    TextEdit {
        id: naturalMessageBody
        visible: false
        readOnly: true
        text: delegateRoot.shownText
        textFormat: TextEdit.PlainText
        font.family: Theme.uiFont
        font.pixelSize: delegateRoot.kind === 1 ? 22 : 16
        font.italic: delegateRoot.editing
        wrapMode: TextEdit.NoWrap
    }

    Item {
        objectName: "scrollingDateDivider"
        x: 18
        y: 20
        width: parent.width - 36
        height: 32
        visible: delegateRoot.showDateDivider

        Rectangle {
            anchors.left: parent.left
            anchors.right: dateText.left
            anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            height: 1
            color: Theme.dateRule
        }
        Text {
            id: dateText
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.verticalCenter: parent.verticalCenter
            text: delegateRoot.dateLabel
            color: Theme.textSecondary
            font.family: Theme.uiFont
            font.pixelSize: 14
            renderType: Text.NativeRendering
        }
        Rectangle {
            anchors.left: dateText.right
            anchors.leftMargin: 10
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            height: 1
            color: Theme.dateRule
        }
    }

    Item {
        id: eventDivider
        objectName: "conversationEvent"
        visible: delegateRoot.eventRow
        width: Math.min(delegateRoot.width - 36,
                        eventLabel.implicitWidth + (delegateRoot.callEvent ? 48 : 80))
        height: eventLabel.implicitHeight + 16
        x: delegateRoot.callEvent
            ? (delegateRoot.outgoing ? delegateRoot.width - width - 17 : 16)
            : (delegateRoot.width - width) / 2
        y: delegateRoot.dateSectionHeight

        Rectangle {
            width: delegateRoot.callEvent ? 14 : 30
            height: 1
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            color: delegateRoot.callEvent ? Theme.successText : Theme.dateRule
        }
        Text {
            id: eventLabel
            objectName: "conversationEventText"
            anchors.centerIn: parent
            width: Math.min(implicitWidth, eventDivider.width - (delegateRoot.callEvent ? 48 : 80))
            text: delegateRoot.body
            textFormat: Text.PlainText
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            color: delegateRoot.callEvent ? Theme.successText : Theme.textSecondary
            font.family: Theme.uiFont
            font.pixelSize: 13
            font.weight: delegateRoot.callEvent ? Font.DemiBold : Font.Normal
            renderType: Text.NativeRendering
        }
        Rectangle {
            width: delegateRoot.callEvent ? 14 : 30
            height: 1
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            color: delegateRoot.callEvent ? Theme.successText : Theme.dateRule
        }
    }

    BubbleBackground {
        id: bubble
        objectName: "messageBubble"
        visible: !delegateRoot.eventRow
        x: delegateRoot.outgoing ? delegateRoot.width - bubble.width - 17 : 16
        y: delegateRoot.dateSectionHeight + delegateRoot.senderHeight
        width: delegateRoot.bubbleWidth
        height: delegateRoot.bubbleHeight
        outgoing: delegateRoot.outgoing
        radius: 6
        tailWidth: delegateRoot.bubbleTailWidth
        tailHeight: 13
        fillTop: delegateRoot.outgoing ? Theme.outgoingTop : Theme.incomingTop
        fillBottom: delegateRoot.outgoing ? Theme.outgoingBottom : Theme.incomingBottom
        strokeColor: delegateRoot.outgoing ? Theme.outgoingBorder : Theme.incomingBorder
        skin: delegateRoot.bubbleSkin
    }

    // A reply quotes what it answers above its own text: who wrote it and the
    // start of it, beside an accent bar. A click brings the original into view.
    Item {
        id: quote
        objectName: "messageQuote"
        readonly property real naturalWidth: 10 + Math.max(quoteAuthor.implicitWidth,
                                                           quoteText.implicitWidth)
        visible: delegateRoot.isReply && !delegateRoot.eventRow
        x: bubble.x + delegateRoot.contentLeftInset
        y: bubble.y + 10
        width: bubble.width - delegateRoot.horizontalContentInset
        height: 34

        Rectangle {
            width: 2
            height: parent.height
            radius: 1
            color: bubble.skinned ? bubble.skinSecondaryTextColor : Theme.accentBlue
        }
        Text {
            id: quoteAuthor
            objectName: "messageQuoteSender"
            x: 10
            y: 0
            width: parent.width - x
            text: delegateRoot.quotedSender.length > 0 ? delegateRoot.quotedSender : "Message"
            textFormat: Text.PlainText
            elide: Text.ElideRight
            color: bubble.skinned ? bubble.skinTextColor : Theme.focusBorder
            font.family: Theme.uiFont
            font.pixelSize: 12
            font.bold: true
            renderType: Text.NativeRendering
        }
        Text {
            id: quoteText
            objectName: "messageQuoteText"
            x: 10
            y: 17
            width: parent.width - x
            // One line: a quote is a reminder, not the message again.
            text: delegateRoot.quotedBody.replace(/\s+/g, " ").trim()
            textFormat: Text.PlainText
            elide: Text.ElideRight
            color: bubble.skinned ? bubble.skinSecondaryTextColor : Theme.textSecondary
            font.family: Theme.uiFont
            font.pixelSize: 12
            renderType: Text.NativeRendering
        }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: delegateRoot.quoteActivated()
        }
    }

    // An attachment: the photo, the video, the sound or the file, under the
    // quote of what it answers. Loaded by URL, and only for an attachment,
    // so a bubble made on its own never compiles the media types.
    Loader {
        id: attachmentLoader
        objectName: "attachmentLoader"
        active: delegateRoot.attachment && !delegateRoot.eventRow
        x: bubble.x + (delegateRoot.visualMedia ? delegateRoot.bodyLeadingInset + 4 : delegateRoot.contentLeftInset)
        y: bubble.y + delegateRoot.mediaTop
        width: delegateRoot.mediaBoxWidth
        height: delegateRoot.mediaBoxHeight
        Component.onCompleted: setSource(Qt.resolvedUrl("AttachmentBlock.qml"), { row: delegateRoot })
    }

    // On a skin the text is raised, as Text.Raised draws it: the same layout
    // once more in the shadow colour, a pixel lower, under the real text.
    TextEdit {
        id: bodyShadow
        visible: messageBody.visible && messageBody.style === Text.Raised
        enabled: false
        x: messageBody.x
        y: messageBody.y + 1
        width: messageBody.width
        readOnly: true
        text: messageBody.text
        textFormat: TextEdit.PlainText
        color: messageBody.styleColor
        font: messageBody.font
        wrapMode: TextEdit.Wrap
        renderType: Text.NativeRendering
        TextLineSpacing {
            document: bodyShadow.textDocument
            lineHeight: 1.18
        }
    }

    // Selectable, so any part of a message can be copied.
    TextEdit {
        id: messageBody
        objectName: "messageBody"
        // The raised look a skin asks for, drawn by bodyShadow.
        property int style: bubble.skinned && !delegateRoot.editing ? Text.Raised : Text.Normal
        property color styleColor: bubble.skinned ? bubble.skinTextShadowColor : "transparent"
        // An attachment's caption, under it; none, and it takes no room.
        visible: !delegateRoot.eventRow && (!delegateRoot.attachment || delegateRoot.hasCaption)
        x: delegateRoot.kind === 1 || delegateRoot.isReply || delegateRoot.attachment
            ? bubble.x + delegateRoot.contentLeftInset
            : bubble.x + delegateRoot.contentLeftInset
              + (bubble.width - delegateRoot.horizontalContentInset - paintedWidth) / 2
        y: delegateRoot.attachment
            ? bubble.y + delegateRoot.mediaBottom + (delegateRoot.visualMedia ? 7 : 6)
            : bubble.y + (delegateRoot.kind === 1 ? 10 : 11) + delegateRoot.quoteHeight
        width: delegateRoot.kind === 1
            ? bubble.width - messageTime.implicitWidth - (delegateRoot.outgoing ? 49 : 58)
            : delegateRoot.attachment
              ? bubble.width - delegateRoot.horizontalContentInset
              : Math.min(naturalMessageBody.implicitWidth,
                         delegateRoot.maximumBubbleWidth - delegateRoot.horizontalContentInset)
        readOnly: true
        selectByMouse: !delegateRoot.editing
        text: delegateRoot.shownText
        textFormat: TextEdit.PlainText
        color: delegateRoot.editing ? Theme.textSecondary
            : bubble.skinned ? bubble.skinTextColor : Theme.textPrimary
        selectionColor: Theme.selectionBackground
        selectedTextColor: Theme.selectionText
        font.family: Theme.uiFont
        font.pixelSize: delegateRoot.kind === 1 ? 22 : 16
        font.italic: delegateRoot.editing
        wrapMode: TextEdit.Wrap
        renderType: Text.NativeRendering
        Keys.onPressed: event => {
            const shortcut = (event.modifiers & (Qt.ControlModifier | Qt.AltModifier
                                                 | Qt.MetaModifier)) !== 0;
            if (event.key === Qt.Key_Escape) {
                messageBody.deselect();
                delegateRoot.typed("");
                event.accepted = true;
            } else if (!shortcut && event.text.length > 0 && event.text.charCodeAt(0) >= 0x20
                       && event.text.charCodeAt(0) !== 0x7f) {
                messageBody.deselect();
                delegateRoot.typed(event.text);
                event.accepted = true;
            }
        }
        TextLineSpacing {
            document: messageBody.textDocument
            lineHeight: 1.18
        }
    }

    Text {
        id: messageTime
        // On a photo or a video with no caption the time rides on the
        // picture instead (AttachmentBlock draws it there).
        visible: !delegateRoot.eventRow && !(delegateRoot.visualMedia && !delegateRoot.hasCaption)
        objectName: "messageTimestamp"
        x: delegateRoot.kind === 1
            ? bubble.x + bubble.width - implicitWidth - (delegateRoot.outgoing ? 14 : 13)
            : bubble.x + delegateRoot.contentLeftInset
        y: delegateRoot.kind === 1
            ? bubble.y + bubble.height - implicitHeight - 11
            : delegateRoot.attachment && !delegateRoot.hasCaption
              ? bubble.y + delegateRoot.mediaBottom + 3
              : messageBody.y + messageBody.paintedHeight + 3
        width: delegateRoot.kind === 1
            ? implicitWidth
            : bubble.width - delegateRoot.horizontalContentInset
        text: String(delegateRoot.timestamp)
        color: bubble.skinned ? bubble.skinSecondaryTextColor : Theme.timestampText
        style: bubble.skinned ? Text.Raised : Text.Normal
        styleColor: bubble.skinned ? bubble.skinTextShadowColor : "transparent"
        font.family: Theme.uiFont
        font.pixelSize: 12
        horizontalAlignment: Text.AlignRight
        renderType: Text.NativeRendering
    }

    // Everyone sees that the sender changed the text after sending it.
    Text {
        id: editedLabel
        objectName: "messageEdited"
        visible: !delegateRoot.eventRow && delegateRoot.edited
        x: delegateRoot.outgoing
            ? bubble.x + bubble.width - delegateRoot.contentRightInset - implicitWidth
            : bubble.x + delegateRoot.contentLeftInset
        y: delegateRoot.footerY + delegateRoot.retryHeight + 1
        text: "edited"
        color: Theme.timestampText
        font.family: Theme.uiFont
        font.pixelSize: 11
        renderType: Text.NativeRendering
    }

    // Copy, edit (one's own messages) and reply: bare glyphs lined up with the
    // text's edge, shown while the pointer is on the message. The hovered one
    // is named beside the row, on the side away from the bubble's edge. An
    // attachment copies only its caption (and without one offers no Copy), is
    // never edited, and a complete photo or file can be saved.
    Item {
        id: actions
        objectName: "messageActions"
        // How far a button's glyph sits inside its slot.
        readonly property real glyphInset: (copyAction.width - copyAction.glyphSize) / 2
        readonly property var hoveredAction: copyAction.hovered ? copyAction
            : editAction.hovered ? editAction : replyAction.hovered ? replyAction
            : saveAction.hovered ? saveAction : null
        property bool justCopied: false
        visible: !delegateRoot.eventRow && actionHover.hovered
        x: delegateRoot.outgoing
            ? bubble.x + bubble.width - delegateRoot.contentRightInset + glyphInset - buttons.width
            : bubble.x + delegateRoot.contentLeftInset - glyphInset
        y: delegateRoot.footerY + delegateRoot.retryHeight + delegateRoot.editedHeight
        width: buttons.width
        height: delegateRoot.actionRowHeight

        Row {
            id: buttons
            height: parent.height

            MessageActionButton {
                id: copyAction
                objectName: "messageCopyAction"
                icon: "copy"
                label: actions.justCopied ? "Copied" : "Copy"
                visible: !delegateRoot.attachment || delegateRoot.hasCaption
                height: parent.height
                onClicked: {
                    delegateRoot.copyRequested();
                    actions.justCopied = true;
                    copiedReset.restart();
                }
            }
            MessageActionButton {
                id: editAction
                objectName: "messageEditAction"
                icon: "edit"
                label: "Edit"
                visible: delegateRoot.editable && !delegateRoot.editing && !delegateRoot.attachment
                height: parent.height
                onClicked: delegateRoot.editRequested()
            }
            MessageActionButton {
                id: replyAction
                objectName: "messageReplyAction"
                icon: "reply"
                label: "Reply"
                height: parent.height
                onClicked: delegateRoot.replyRequested()
            }
            MessageActionButton {
                id: saveAction
                objectName: "messageSaveAction"
                icon: "save"
                label: "Save"
                visible: delegateRoot.attachment && delegateRoot.canSave
                height: parent.height
                onClicked: delegateRoot.saveRequested()
            }
        }

        Text {
            objectName: "messageActionCaption"
            visible: actions.hoveredAction !== null
            // Level with the glyphs.
            y: copyAction.glyphTop + copyAction.glyphSize / 2 - height / 2
            anchors.left: delegateRoot.outgoing ? undefined : buttons.right
            anchors.right: delegateRoot.outgoing ? buttons.left : undefined
            anchors.leftMargin: 2
            anchors.rightMargin: 2
            text: actions.hoveredAction !== null ? actions.hoveredAction.label : ""
            color: Theme.timestampText
            font.family: Theme.uiFont
            font.pixelSize: 11
            renderType: Text.NativeRendering
        }

        Timer {
            id: copiedReset
            interval: 1500
            onTriggered: actions.justCopied = false
        }
    }
}
