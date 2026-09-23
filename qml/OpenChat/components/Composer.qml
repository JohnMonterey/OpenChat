import QtQuick
import QtQuick.Controls.Basic
import OpenChat
import OpenChat.Native

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
                    + (lengthCounter.visible ? lengthCounter.implicitHeight + lengthCounter.anchors.topMargin : 0)

    // Whichever chat is open, the keyboard is in its composer, so typing and
    // Enter go to it. Only a change of chat moves focus, not a refresh of it.
    readonly property string chatId: controller.currentContactId
    onChatIdChanged: input.forceActiveFocus()
    Component.onCompleted: input.forceActiveFocus()

    function send() {
        if (controller.canSend && controller.sendMessage())
            messageSent();
    }

    // Enter sends. The rest is the editing code editors are loved for; every
    // edit is one undo step. Returns whether the key was used.
    function handleKey(event) {
        const ctrl = (event.modifiers & Qt.ControlModifier) !== 0;
        const shift = (event.modifiers & Qt.ShiftModifier) !== 0;
        const alt = (event.modifiers & Qt.AltModifier) !== 0;
        const start = input.selectionStart;
        const end = input.selectionEnd;
        let result = null;
        switch (event.key) {
        case Qt.Key_Return:
        case Qt.Key_Enter:
            if (alt)
                return false;
            if (ctrl)
                result = editing.insertLine(start, end, shift);   // line below / above
            else if (shift)
                result = editing.newLine(start, end);             // line break, indented
            else {
                send();
                return true;                                      // never a bare line break
            }
            break;
        case Qt.Key_Tab:
            if (!ctrl && !alt)
                result = shift ? editing.outdentLines(start, end) : editing.tab(start, end);
            break;
        case Qt.Key_Backtab:
            if (!ctrl && !alt)
                result = editing.outdentLines(start, end);
            break;
        case Qt.Key_BracketRight:
            if (ctrl && !alt)
                result = editing.indentLines(start, end);
            break;
        case Qt.Key_BracketLeft:
            if (ctrl && !alt)
                result = editing.outdentLines(start, end);
            break;
        case Qt.Key_Up:
        case Qt.Key_Down: {
            const up = event.key === Qt.Key_Up;
            if (alt && !ctrl) {
                result = shift ? editing.copyLines(start, end, up) : editing.moveLines(start, end, up);
            } else if (ctrl && !alt && !shift) {
                // The view moves a line; the cursor stays where it is.
                inputScroll.scrollBy((up ? -1 : 1) * input.cursorRectangle.height);
                return true;
            }
            break;
        }
        case Qt.Key_K:
            if (ctrl && shift && !alt)
                result = editing.deleteLines(start, end);
            break;
        case Qt.Key_L:
            if (ctrl && !shift && !alt)
                result = editing.selectLines(start, end);
            break;
        case Qt.Key_C:
            if (ctrl && !shift && !alt && start === end)
                result = editing.copyLine(start);
            break;
        case Qt.Key_X:
            if (ctrl && !shift && !alt && start === end)
                result = editing.cutLine(start);
            break;
        case Qt.Key_Y:
        case Qt.Key_Z:
            // Redo as both editors spell it, whatever the platform's default.
            if (ctrl && !alt && (event.key === Qt.Key_Y ? !shift : shift)) {
                input.redo();
                return true;
            }
            break;
        }
        if (result === null)
            return false;
        input.select(result.start, result.end);
        return true;
    }

    ComposerEditing {
        id: editing
        document: input.textDocument
        maxLength: composer.controller.composerMaxLength
    }

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
        width: parent.width - 2 * x
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
            flickableDirection: Flickable.VerticalFlick
            boundsBehavior: Flickable.StopAtBounds
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
                Keys.onPressed: event => event.accepted = composer.handleKey(event)
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

    // Near the longest message that can be sent, how much room is left, under
    // the field's right edge; the composer grows by a line to show it.
    Text {
        id: lengthCounter
        objectName: "composerLengthCounter"
        readonly property int remaining: composer.controller.composerMaxLength - input.length
        anchors.right: inputFrame.right
        anchors.top: inputFrame.bottom
        anchors.topMargin: 3
        visible: remaining < 1000
        text: Number(remaining).toLocaleString(Qt.locale(), "f", 0) + " left"
        color: remaining > 0 ? Theme.textSecondary : "#e0503d"
        font.family: Theme.uiFont
        font.pixelSize: 11
        renderType: Text.NativeRendering
    }
}
