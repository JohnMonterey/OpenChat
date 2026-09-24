import QtQuick
import OpenChat

// The editor's text field (SPEC §14.3): the AeroTextField frame and four-line
// bevel, with a 2 px focusBorder while focused. `multiLine` makes it a text
// area at least `minimumHeight` tall (60 by default) that grows with its text
// and keeps line breaks; otherwise it is one line, 32 tall.
//
// The model owns the words: `value` is bound to a page property, every
// keystroke goes out through `edited(text)`, and whatever the page stores
// (live sanitising, the length bound) comes back through `value`. Typed
// spaces and blank lines survive that round trip (Profile::sanitizeLive).
//
// The field's focus period is one undo step: it opens `gestureKey` on the
// controller when it takes focus and closes it when focus leaves, so undo
// history lands on focus loss while Ctrl+Z inside the field stays the field's
// own. Esc puts back what the field held when it took focus; with nothing to
// put back, Esc goes on to the page (the editor's leave dialog).
Item {
    id: field

    property string value: ""
    property bool multiLine: true
    property int maximumLength: 0 // 0: no bound here (the model still bounds)
    property string placeholder: ""
    property string accessibleName: ""
    property var profiles: null
    property string gestureKey: ""
    property int minimumHeight: multiLine ? 60 : 32
    property int fontPixelSize: 13

    readonly property Item input: multiLine ? area : line
    readonly property string text: input.text
    readonly property bool editing: input.activeFocus
    signal edited(string text)

    // What the field held when it took focus: what Esc restores.
    property string focusValue: ""

    function focusInput() {
        input.forceActiveFocus(Qt.OtherFocusReason);
    }
    function sync() {
        if (input.text === value)
            return;
        const cursor = input.cursorPosition;
        input.text = value;
        input.cursorPosition = Math.min(cursor, value.length);
    }
    function textEdited(text) {
        if (text === value)
            return;
        edited(text);
        // The page kept something else (the bound, a character it refuses):
        // show what it holds, once this edit has finished.
        if (input.text !== value)
            Qt.callLater(sync);
    }
    function focusMoved(focused) {
        if (focused) {
            focusValue = value;
            if (profiles && gestureKey.length > 0)
                profiles.beginGesture(gestureKey);
        } else if (profiles && gestureKey.length > 0) {
            profiles.endGesture();
        }
    }
    function revert() {
        edited(focusValue);
        sync();
    }

    implicitWidth: 240
    implicitHeight: multiLine ? Math.max(minimumHeight, area.contentHeight + 16) : 32
    height: implicitHeight
    onValueChanged: sync()
    Component.onCompleted: sync()

    Rectangle {
        anchors.fill: parent
        radius: 5
        color: Theme.fieldBackground
    }

    TextInput {
        id: line
        visible: !field.multiLine
        enabled: !field.multiLine
        anchors.left: parent.left
        anchors.leftMargin: 11
        anchors.right: parent.right
        anchors.rightMargin: 11
        anchors.verticalCenter: parent.verticalCenter
        color: Theme.textPrimary
        font.family: Theme.uiFont
        font.pixelSize: field.fontPixelSize
        maximumLength: field.maximumLength > 0 ? field.maximumLength : 32767
        clip: true
        selectByMouse: true
        selectionColor: Theme.selectionBackground
        selectedTextColor: Theme.selectionText
        activeFocusOnTab: !field.multiLine
        Accessible.role: Accessible.EditableText
        Accessible.name: field.accessibleName
        onTextChanged: if (!field.multiLine) field.textEdited(text)
        onActiveFocusChanged: if (!field.multiLine) field.focusMoved(activeFocus)
        Keys.onShortcutOverride: event => {
            if (event.key === Qt.Key_Escape && text !== field.focusValue)
                event.accepted = true;
        }
        Keys.onEscapePressed: field.revert()
    }

    TextEdit {
        id: area
        visible: field.multiLine
        enabled: field.multiLine
        x: 10
        y: 8
        width: parent.width - 20
        color: Theme.textPrimary
        font.family: Theme.uiFont
        font.pixelSize: field.fontPixelSize
        wrapMode: TextEdit.Wrap
        textFormat: TextEdit.PlainText
        selectByMouse: true
        selectionColor: Theme.selectionBackground
        selectedTextColor: Theme.selectionText
        activeFocusOnTab: field.multiLine
        Accessible.role: Accessible.EditableText
        Accessible.name: field.accessibleName
        Accessible.multiLine: true
        onTextChanged: {
            if (!field.multiLine)
                return;
            // A TextEdit has no maximumLength; the bound is the model's.
            if (field.maximumLength > 0 && text.length > field.maximumLength) {
                field.textEdited(text.slice(0, field.maximumLength));
                Qt.callLater(field.sync);
                return;
            }
            field.textEdited(text);
        }
        onActiveFocusChanged: if (field.multiLine) field.focusMoved(activeFocus)
        Keys.onShortcutOverride: event => {
            if (event.key === Qt.Key_Escape && text !== field.focusValue)
                event.accepted = true;
        }
        Keys.onEscapePressed: field.revert()
    }

    Text {
        anchors.left: parent.left
        anchors.leftMargin: field.multiLine ? 10 : 11
        anchors.right: parent.right
        anchors.rightMargin: 11
        y: field.multiLine ? 8 : (parent.height - height) / 2
        visible: field.input.text.length === 0 && !field.input.activeFocus
        text: field.placeholder
        elide: Text.ElideRight
        color: Theme.placeholderText
        font.family: Theme.uiFont
        font.pixelSize: field.fontPixelSize
        renderType: Text.NativeRendering
    }

    // Aero inset: a shadow along the inner top and left, a faint highlight
    // along the bottom, then the outline, which thickens while focused.
    Rectangle { x: 5; y: 1; width: parent.width - 10; height: 1; color: Theme.insetTop }
    Rectangle { x: 1; y: 5; width: 1; height: parent.height - 10; color: Theme.insetLeft }
    Rectangle { x: 5; y: parent.height - 2; width: parent.width - 10; height: 1; color: Theme.gloss }
    Rectangle {
        anchors.fill: parent
        radius: 5
        color: "transparent"
        border.width: field.editing ? 2 : 1
        border.color: field.editing ? Theme.focusBorder : Theme.inputBorder
    }

    MouseArea {
        // Clicks below the last line of a text area still land in it.
        anchors.fill: parent
        visible: field.multiLine
        acceptedButtons: Qt.LeftButton
        cursorShape: Qt.IBeamCursor
        propagateComposedEvents: true
        onPressed: mouse => {
            if (mouse.y > area.y + area.contentHeight) {
                area.forceActiveFocus(Qt.MouseFocusReason);
                area.cursorPosition = area.length;
            }
            mouse.accepted = false;
        }
    }
}
