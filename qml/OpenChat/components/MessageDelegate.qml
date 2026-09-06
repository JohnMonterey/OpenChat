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
    readonly property bool outgoing: direction === 1
    readonly property bool eventRow: kind === 2 || kind === 3
    readonly property bool callEvent: kind === 3
    readonly property bool showSender: !eventRow && !outgoing && senderName.length > 0
    readonly property real senderHeight: showSender ? 18 : 0
    readonly property real maximumBubbleWidth: Math.min(360, width * 0.68)
    readonly property real directionalLimit: outgoing
        ? Math.min(338, maximumBubbleWidth)
        : Math.min(290, maximumBubbleWidth)
    readonly property real bubbleTailWidth: 9
    readonly property real bodyLeadingInset: outgoing ? 0 : bubbleTailWidth
    readonly property real bodyTrailingInset: outgoing ? bubbleTailWidth : 0
    readonly property real contentPadding: 14
    readonly property real contentLeftInset: bodyLeadingInset + contentPadding
    readonly property real contentRightInset: bodyTrailingInset + contentPadding
    readonly property real horizontalContentInset: contentLeftInset + contentRightInset
    readonly property real preferredBubbleWidth: kind === 1
        ? 158
        : Math.min(directionalLimit,
                   Math.max(naturalMessageBody.implicitWidth + horizontalContentInset,
                            messageTime.implicitWidth + horizontalContentInset))
    readonly property real bubbleWidth: Math.min(maximumBubbleWidth, preferredBubbleWidth)
    readonly property real bubbleHeight: kind === 1
        ? 54
        : Math.max(54, messageBody.paintedHeight + messageTime.implicitHeight + 21)
    readonly property real dateSectionHeight: showDateDivider ? 64 : 0

    implicitHeight: dateSectionHeight + (eventRow ? eventLabel.implicitHeight + 16
        : senderHeight + bubbleHeight + 20 + (retryText.visible ? 20 : 0))

    Text {
        objectName: "messageSender"
        visible: delegateRoot.showSender
        x: bubble.x + delegateRoot.contentLeftInset
        y: delegateRoot.dateSectionHeight + 2
        width: delegateRoot.maximumBubbleWidth
        elide: Text.ElideRight
        text: delegateRoot.senderName
        color: Theme.categoryText
        font.family: Theme.uiFont
        font.pixelSize: 12
        font.bold: true
        renderType: Text.NativeRendering
    }

    Text {
        id: retryText
        objectName: "messageRetry"
        visible: !delegateRoot.eventRow && delegateRoot.outgoing && delegateRoot.deliveryState === 6
        anchors.right: bubble.right
        anchors.rightMargin: delegateRoot.bubbleTailWidth
        y: bubble.y + bubble.height + 4
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

    Text {
        id: naturalMessageBody
        visible: false
        text: delegateRoot.body
        font.family: Theme.uiFont
        font.pixelSize: delegateRoot.kind === 1 ? 22 : 16
        wrapMode: Text.NoWrap
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
    }

    Text {
        id: messageBody
        visible: !delegateRoot.eventRow
        x: delegateRoot.kind === 1
            ? bubble.x + delegateRoot.contentLeftInset
            : bubble.x + delegateRoot.contentLeftInset
              + (bubble.width - delegateRoot.horizontalContentInset - paintedWidth) / 2
        y: bubble.y + (delegateRoot.kind === 1 ? 10 : 11)
        width: delegateRoot.kind === 1
            ? bubble.width - messageTime.implicitWidth - (delegateRoot.outgoing ? 49 : 58)
            : Math.min(naturalMessageBody.implicitWidth,
                       bubble.width - delegateRoot.horizontalContentInset)
        text: delegateRoot.body
        color: Theme.textPrimary
        font.family: Theme.uiFont
        font.pixelSize: delegateRoot.kind === 1 ? 22 : 16
        lineHeight: 1.18
        wrapMode: Text.Wrap
        renderType: Text.NativeRendering
    }

    Text {
        id: messageTime
        visible: !delegateRoot.eventRow
        objectName: "messageTimestamp"
        x: delegateRoot.kind === 1
            ? bubble.x + bubble.width - implicitWidth - (delegateRoot.outgoing ? 14 : 13)
            : bubble.x + delegateRoot.contentLeftInset
        y: delegateRoot.kind === 1
            ? bubble.y + bubble.height - implicitHeight - 11
            : messageBody.y + messageBody.paintedHeight + 3
        width: delegateRoot.kind === 1
            ? implicitWidth
            : bubble.width - delegateRoot.horizontalContentInset
        text: String(delegateRoot.timestamp)
        color: Theme.timestampText
        font.family: Theme.uiFont
        font.pixelSize: 12
        horizontalAlignment: Text.AlignRight
        renderType: Text.NativeRendering
    }
}
