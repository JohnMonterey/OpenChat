import QtQuick
import QtQuick.Controls.Basic
import OpenChat

Item {
    id: composer
    objectName: "messageComposer"
    required property var controller
    signal messageSent
    // The tallest the field grows before its text scrolls instead.
    property real maxInputHeight: 112
    // One line of text keeps the composer at the bottom bar's height, level with
    // the sidebar's navigation; further lines grow the field, up to a cap.
    readonly property int margin: 12
    readonly property int singleLineHeight: Theme.bottomBarHeight - 2 * margin
    readonly property real inputHeight: input.lineCount <= 1
        ? singleLineHeight
        : Math.max(singleLineHeight, Math.min(maxInputHeight, Math.ceil(input.contentHeight) + 20))
    implicitHeight: inputHeight + 2 * margin

    Rectangle {
        anchors.fill: parent
        color: Theme.composerBackground
    }
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 1
        color: Theme.rule
    }

    Item {
        id: inputFrame
        objectName: "composerInputFrame"
        x: 17
        y: composer.margin
        width: parent.width - 142
        height: composer.inputHeight

        Rectangle {
            anchors.fill: parent
            radius: 5
            color: Theme.fieldBackground
        }

        // Once the field is as tall as it may grow, the text scrolls inside it,
        // following the cursor. Mouse drags stay with the text for selecting;
        // the wheel and the scroll bar move it.
        Flickable {
            id: inputScroll
            objectName: "messageInputScroll"
            anchors.left: parent.left
            anchors.right: attachment.left
            anchors.leftMargin: 12
            anchors.rightMargin: 15
            anchors.verticalCenter: parent.verticalCenter
            height: Math.min(input.height, parent.height - 16)
            contentWidth: width
            contentHeight: input.height
            clip: true
            interactive: false
            onHeightChanged: keepInBounds()
            onContentHeightChanged: keepInBounds()

            function keepInBounds() {
                contentY = Math.max(0, Math.min(contentY, contentHeight - height));
            }
            function ensureVisible(r) {
                if (contentY >= r.y)
                    contentY = r.y;
                else if (contentY + height <= r.y + r.height)
                    contentY = r.y + r.height - height;
            }
            function scrollBy(pixels) {
                contentY = Math.max(0, Math.min(contentHeight - height, contentY + pixels));
            }

            ScrollBar.vertical: ScrollBar {
                id: inputScrollBar
                objectName: "messageInputScrollBar"
                // In the gutter between the text and the attachment button.
                parent: inputFrame
                x: attachment.x - width - 4
                y: inputScroll.y
                height: inputScroll.height
                padding: 0
                minimumSize: 0.1
                policy: ScrollBar.AsNeeded
                background: Item {}
                contentItem: Rectangle {
                    implicitWidth: 6
                    radius: 3
                    color: inputScrollBar.pressed || inputScrollBar.hovered ? Theme.iconHover
                                                                            : Theme.inputBorder
                    visible: inputScrollBar.size < 1.0
                }
            }

            TextEdit {
                id: input
                objectName: "messageInput"
                width: inputScroll.width
                height: Math.max(contentHeight, 20)
                text: composer.controller.composerText
                color: Theme.textPrimary
                font.family: Theme.uiFont
                font.pixelSize: 16
                wrapMode: TextEdit.Wrap
                selectByMouse: true
                selectionColor: Theme.selectionBackground
                selectedTextColor: Theme.selectionText
                onCursorRectangleChanged: inputScroll.ensureVisible(cursorRectangle)
                onTextChanged: {
                    const overflow = length - composer.controller.composerMaxLength;
                    if (overflow > 0) {
                        // Past the limit, the excess comes off what was just typed
                        // or pasted, which ends at the cursor, so the text already
                        // there is kept; failing that, off the end. A character
                        // made of two code units is never split.
                        const end = cursorPosition >= overflow ? cursorPosition : length;
                        let start = end - overflow;
                        const before = start > 0 ? text.charCodeAt(start - 1) : 0;
                        if (before >= 0xd800 && before <= 0xdbff)
                            start -= 1;
                        remove(start, end);
                        return;
                    }
                    if (text !== composer.controller.composerText)
                        composer.controller.setComposerText(text);
                }
                Keys.onReturnPressed: event => {
                    if (!(event.modifiers & Qt.ShiftModifier) && composer.controller.canSend) {
                        composer.controller.sendMessage();
                        composer.messageSent();
                        event.accepted = true;
                    }
                }
            }
        }

        WheelHandler {
            target: null
            enabled: inputScroll.contentHeight > inputScroll.height
            onWheel: event => {
                const lines = input.cursorRectangle.height * 3;
                inputScroll.scrollBy(event.pixelDelta.y !== 0 ? -event.pixelDelta.y
                                                              : -event.angleDelta.y / 120 * lines);
            }
        }

        Item {
            id: attachment
            objectName: "attachmentButton"
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.right: parent.right
            width: 38

            Rectangle {
                anchors.fill: parent
                radius: 5
                color: Theme.fieldAccessory
            }
            Rectangle {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width / 2
                height: parent.height
                color: Theme.fieldAccessory
            }

            Rectangle {
                anchors.left: parent.left
                width: 1
                height: parent.height
                color: Theme.fieldDivider
            }
            Image {
                anchors.centerIn: parent
                width: 11
                height: 7
                source: Qt.resolvedUrl("../../../assets/icons/chevron-down" + (Theme.darkMode ? "-dark.svg" : ".svg"))
            }
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
            }
        }

        // Restrained Aero inset: a shadow along the inner top/left edges and a
        // faint highlight opposite it. The shared outer outline is drawn last.
        Rectangle {
            x: 5
            y: 1
            width: parent.width - 10
            height: 1
            color: Theme.insetTop
        }
        Rectangle {
            x: 1
            y: 5
            width: 1
            height: parent.height - 10
            color: Theme.insetLeft
        }
        Rectangle {
            x: 5
            y: parent.height - 2
            width: parent.width - 10
            height: 1
            color: Theme.gloss
        }
        Rectangle {
            anchors.fill: parent
            z: 10
            radius: 5
            color: "transparent"
            border.width: 1
            border.color: Theme.inputBorder
        }
    }

    Item {
        id: send
        objectName: "sendButton"
        enabled: composer.controller.canSend
        signal clicked
        x: inputFrame.x + inputFrame.width + 17
        y: Math.round((parent.height - height) / 2)
        width: parent.width - x - 18
        height: composer.singleLineHeight

        Rectangle {
            anchors.fill: parent
            radius: 4
            color: send.enabled ? (sendMouse.containsMouse ? Theme.buttonHover : Theme.buttonBackground) : Theme.buttonDisabled
            border.width: 1
            border.color: send.enabled ? Theme.buttonBorder : Theme.buttonDisabledBorder
        }
        Text {
            anchors.centerIn: parent
            text: "Send"
            color: send.enabled ? Theme.sendText : Theme.buttonDisabledText
            font.family: Theme.uiFont
            font.pixelSize: 15
            renderType: Text.NativeRendering
        }
        MouseArea {
            id: sendMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: send.clicked()
        }
        onClicked: {
            if (send.enabled && composer.controller.sendMessage())
                composer.messageSent();
        }
    }

    // Near the longest message that can be sent, how much room is left.
    Text {
        id: lengthCounter
        objectName: "composerLengthCounter"
        readonly property int remaining: composer.controller.composerMaxLength - input.length
        anchors.horizontalCenter: send.horizontalCenter
        anchors.top: send.bottom
        anchors.topMargin: 6
        visible: remaining < 1000
        text: Number(remaining).toLocaleString(Qt.locale(), "f", 0) + " left"
        color: remaining > 0 ? Theme.textSecondary : "#e0503d"
        font.family: Theme.uiFont
        font.pixelSize: 11
        renderType: Text.NativeRendering
    }
}
