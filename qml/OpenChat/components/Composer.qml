import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Shapes
import QtQuick.Window
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
    // Changing or answering a message: a bar above the field says which, and
    // its cross (or Esc) goes back to writing a new message.
    readonly property bool editingMessage: controller.editingMessageId.length > 0
    readonly property bool composing: editingMessage || controller.replyingToMessageId.length > 0
    readonly property real composeBarHeight: composing ? 26 : 0
    // Attachments: the "+" left of the field opens a menu of what can be
    // sent, and what is picked waits in a tray above the field (under a line
    // saying why, when something could not be taken). Nothing can be attached
    // to an edit, or while the chat's messages are hidden.
    readonly property bool canAttach: typeof controller.attachFiles === "function"
                                      && controller.plaintextVisible === true && !editingMessage
    readonly property real noticeHeight: attachmentNotice.shown ? 26 : 0
    readonly property real trayHeight: stagedTray.shown ? stagedTray.height + 10 : 0
    readonly property real stackHeight: noticeHeight + trayHeight
    implicitHeight: stackHeight + composeBarHeight + inputHeight + 2 * margin
                    + (lengthCounter.visible ? lengthCounter.implicitHeight + lengthCounter.anchors.topMargin : 0)

    // Whichever chat is open, the keyboard is in its composer, so typing and
    // Enter go to it. Only a change of chat moves focus, not a refresh of it.
    readonly property string chatId: controller.currentContactId
    onChatIdChanged: {
        attachMenu.close();
        input.forceActiveFocus();
    }
    Component.onCompleted: input.forceActiveFocus()
    // The menu goes when attaching stops being possible (an edit begins, the
    // messages are hidden) or the chat is covered or gone (a profile opens
    // over it, the call fills the window).
    onCanAttachChanged: if (!canAttach) attachMenu.close()
    onEnabledChanged: if (!enabled) attachMenu.close()
    onVisibleChanged: if (!visible) attachMenu.close()

    // Picking Edit or Reply under a message hands the keyboard back here, at
    // the end of the text.
    Connections {
        target: composer.controller
        function onComposeModeChanged() {
            if (!composer.composing)
                return;
            input.forceActiveFocus();
            input.cursorPosition = input.length;
        }
    }

    function send() {
        if (controller.canSend && controller.sendMessage())
            messageSent();
    }

    // Takes the keyboard back, with whatever was typed elsewhere meanwhile
    // (after selecting text in a message), at the cursor.
    function takeTyping(text) {
        input.forceActiveFocus();
        if (text.length > 0)
            input.insert(input.cursorPosition, text);
    }

    // Opens the attach menu above the "+". From the keyboard (Ctrl+O) its
    // first row is picked out, ready for Enter.
    function openAttachMenu(fromKeyboard) {
        if (!composer.canAttach)
            return;
        attachMenu.open();
        if (fromKeyboard)
            attachMenu.currentIndex = 0;
    }

    // Local files (picked, dropped or pasted) go to the tray, each as the kind
    // its name says.
    function attach(files) {
        if (composer.canAttach && files.length > 0)
            controller.attachFiles(files);
    }

    // What a dialog of the menu's handed back, then the keyboard to the field.
    function pickedFrom(dialog) {
        const files = dialog.selectedFiles.length > 0 ? dialog.selectedFiles
                    : dialog.selectedFile.toString().length > 0 ? [dialog.selectedFile] : [];
        composer.attach(files);
        input.forceActiveFocus();
    }

    // Enter sends. The rest is the editing code editors are loved for; every
    // edit is one undo step. Returns whether the key was used.
    function handleKey(event) {
        const ctrl = (event.modifiers & Qt.ControlModifier) !== 0;
        const shift = (event.modifiers & Qt.ShiftModifier) !== 0;
        const alt = (event.modifiers & Qt.AltModifier) !== 0;
        const start = input.selectionStart;
        const end = input.selectionEnd;
        // Files copied in a file manager, or a picture with no text beside
        // it, on the clipboard are attached; anything else pastes as ever.
        if (event.matches(StandardKey.Paste))
            return composer.canAttach && typeof controller.attachClipboard === "function"
                && controller.attachClipboard() === true;
        let result = null;
        switch (event.key) {
        case Qt.Key_O:
            if (!ctrl || shift || alt)
                return false;
            composer.openAttachMenu(true);
            return true;
        case Qt.Key_Escape:
            if (!composing || ctrl || shift || alt)
                return false;
            controller.cancelComposeMode();
            return true;
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

    AttachmentNotice {
        id: attachmentNotice
        x: inputFrame.x
        y: 8
        width: inputFrame.width
        height: implicitHeight
        controller: composer.controller
    }

    StagedAttachmentTray {
        id: stagedTray
        x: inputFrame.x
        y: composer.margin + composer.noticeHeight
        width: inputFrame.width
        height: implicitHeight
        controller: composer.controller
    }

    Item {
        id: composeBar
        objectName: "composeBar"
        visible: composer.composing
        x: inputFrame.x + 2
        y: 8 + composer.stackHeight
        width: inputFrame.width - 4
        height: 22

        Rectangle {
            width: 2
            height: parent.height
            radius: 1
            color: Theme.accentBlue
        }
        Text {
            id: composeTitle
            objectName: "composeBarTitle"
            x: 10
            anchors.verticalCenter: parent.verticalCenter
            text: composer.editingMessage ? "Editing message"
                                          : "Replying to " + composer.controller.composeTargetName
            textFormat: Text.PlainText
            color: Theme.focusBorder
            font.family: Theme.uiFont
            font.pixelSize: 12
            font.bold: true
            renderType: Text.NativeRendering
        }
        Text {
            objectName: "composeBarText"
            anchors.left: composeTitle.right
            anchors.leftMargin: 8
            anchors.right: composeCancel.left
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            text: composer.controller.composeTargetText.replace(/\s+/g, " ").trim()
            textFormat: Text.PlainText
            elide: Text.ElideRight
            color: Theme.textSecondary
            font.family: Theme.uiFont
            font.pixelSize: 12
            renderType: Text.NativeRendering
        }
        Item {
            id: composeCancel
            objectName: "composeCancel"
            anchors.right: parent.right
            width: 22
            height: parent.height
            Accessible.role: Accessible.Button
            Accessible.name: composer.editingMessage ? "Stop editing" : "Stop replying"
            Accessible.onPressAction: composer.controller.cancelComposeMode()

            Shape {
                anchors.centerIn: parent
                width: 8
                height: 8
                ShapePath {
                    fillColor: "transparent"
                    strokeColor: cancelMouse.containsMouse ? Theme.textPrimary : Theme.timestampText
                    strokeWidth: 1.4
                    capStyle: ShapePath.RoundCap
                    PathSvg { path: "M 0 0 L 8 8 M 8 0 L 0 8" }
                }
            }
            MouseArea {
                id: cancelMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: composer.controller.cancelComposeMode()
            }
        }
    }

    // Level with the field's last line, so it stays by the text as the field
    // grows. The field starts after it, and keeps the old right margin (the
    // outgoing bubbles' edge).
    AttachButton {
        id: attachButton
        x: 17
        anchors.bottom: inputFrame.bottom
        width: composer.singleLineHeight
        height: composer.singleLineHeight
        enabled: composer.canAttach
        open: attachMenu.visible
        onActivated: wasOpen => {
            if (wasOpen)
                attachMenu.close();
            else
                composer.openAttachMenu(false);
        }
    }

    AttachmentMenu {
        id: attachMenu
        parent: attachButton
        x: 0
        y: -height - 6
        videoSupported: composer.controller.videoAttachmentsSupported !== false
        onPicked: kind => {
            const dialog = kind === 1 ? photoDialog : kind === 2 ? videoDialog
                         : kind === 3 ? audioDialog : fileDialog;
            dialog.open();
        }
        // The keyboard goes back to the field unless something else took it
        // (a click into the search field, a dialog the pick opened).
        onClosed: {
            const window = composer.Window.window;
            const holder = window ? window.activeFocusItem : null;
            if (composer.enabled && composer.visible && (holder === null || holder === window.contentItem))
                input.forceActiveFocus();
        }
    }

    // What each row offers; "All files" is always there too, and whatever is
    // picked is sent as the kind its name says.
    FileDialog {
        id: photoDialog
        objectName: "attachPhotoDialog"
        title: "Send photos"
        fileMode: FileDialog.OpenFiles
        nameFilters: ["Photos (*.jpg *.jpeg *.png *.webp *.bmp *.tif *.tiff)", "All files (*)"]
        onAccepted: composer.pickedFrom(photoDialog)
        onRejected: input.forceActiveFocus()
    }
    FileDialog {
        id: videoDialog
        objectName: "attachVideoDialog"
        title: "Send a video"
        fileMode: FileDialog.OpenFiles
        nameFilters: ["Videos (*.mp4 *.m4v *.mov *.webm *.mkv *.avi *.wmv)", "All files (*)"]
        onAccepted: composer.pickedFrom(videoDialog)
        onRejected: input.forceActiveFocus()
    }
    FileDialog {
        id: audioDialog
        objectName: "attachAudioDialog"
        title: "Send audio"
        fileMode: FileDialog.OpenFiles
        nameFilters: ["Audio (*.mp3 *.m4a *.aac *.wav *.ogg *.oga *.opus *.flac)", "All files (*)"]
        onAccepted: composer.pickedFrom(audioDialog)
        onRejected: input.forceActiveFocus()
    }
    FileDialog {
        id: fileDialog
        objectName: "attachFileDialog"
        title: "Send files"
        fileMode: FileDialog.OpenFiles
        nameFilters: ["All files (*)"]
        onAccepted: composer.pickedFrom(fileDialog)
        onRejected: input.forceActiveFocus()
    }

    Item {
        id: inputFrame
        objectName: "composerInputFrame"
        x: 65
        y: composer.margin + composer.stackHeight + composer.composeBarHeight
        width: parent.width - x - 17
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
            anchors.right: parent.right
            anchors.leftMargin: 12
            anchors.rightMargin: 22
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
                // In the gutter between the text and the field's right edge.
                parent: inputFrame
                x: inputFrame.width - width - 5
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
