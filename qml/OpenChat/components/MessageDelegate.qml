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
    readonly property real minimumBubbleWidth: kind === 1 ? 158 : 190
    readonly property real preferredBubbleWidth: kind === 1
        ? 158
        : Math.max(Math.min(minimumBubbleWidth, directionalLimit),
                   Math.min(directionalLimit,
                            messageBody.implicitWidth + messageTime.implicitWidth + 140))
    readonly property real bubbleWidth: Math.min(maximumBubbleWidth, preferredBubbleWidth)
    readonly property real bubbleHeight: Math.max(kind === 1 ? 54 : 50,
                                                   messageBody.paintedHeight + 24)
    readonly property real dateSectionHeight: showDateDivider ? 64 : 0

    implicitHeight: dateSectionHeight + bubbleHeight + 20

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
        tailWidth: 9
        tailHeight: 13
        fillTop: delegateRoot.outgoing ? "#fdfefe" : "#edf8ff"
        fillBottom: delegateRoot.outgoing ? "#f4f8fb" : "#e2f2fc"
        strokeColor: delegateRoot.outgoing ? "#b9c7d4" : "#8dbbe0"
    }

    Text {
        id: messageBody
        x: bubble.x + (delegateRoot.outgoing ? 14 : 23)
        y: bubble.y + (delegateRoot.kind === 1 ? 10 : 11)
        width: bubble.width - messageTime.implicitWidth - (delegateRoot.outgoing ? 49 : 58)
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
        anchors.right: bubble.right
        anchors.rightMargin: delegateRoot.outgoing ? 14 : 13
        anchors.bottom: bubble.bottom
        anchors.bottomMargin: 11
        text: String(delegateRoot.timestamp)
        color: "#92a2b4"
        font.family: Theme.uiFont
        font.pixelSize: 12
        renderType: Text.NativeRendering
    }
}
