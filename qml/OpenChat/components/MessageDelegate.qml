import QtQuick
import OpenChat
import OpenChat.Native

Item {
    id: delegateRoot
    required property int direction
    required property string body
    required property string timestamp
    required property int kind
    required property string dateLabel
    required property bool showDateDivider
    readonly property bool outgoing: direction === 1
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

    implicitHeight: dateSectionHeight + bubbleHeight + 20

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
            color: "#d7e1e8"
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
            color: "#d7e1e8"
        }
    }

    BubbleBackground {
        id: bubble
        x: delegateRoot.outgoing ? delegateRoot.width - bubble.width - 17 : 16
        y: delegateRoot.dateSectionHeight
        width: delegateRoot.bubbleWidth
        height: delegateRoot.bubbleHeight
        outgoing: delegateRoot.outgoing
        radius: 6
        tailWidth: delegateRoot.bubbleTailWidth
        tailHeight: 13
        fillTop: delegateRoot.outgoing ? "#fdfefe" : "#edf8ff"
        fillBottom: delegateRoot.outgoing ? "#f4f8fb" : "#e2f2fc"
        strokeColor: delegateRoot.outgoing ? "#b9c7d4" : "#8dbbe0"
    }

    Text {
        id: messageBody
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
        color: "#92a2b4"
        font.family: Theme.uiFont
        font.pixelSize: 12
        horizontalAlignment: Text.AlignRight
        renderType: Text.NativeRendering
    }
}
