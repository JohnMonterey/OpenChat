import QtQuick
import QtQuick.Controls as Controls
import OpenChat
import OpenChat.Native

// The editor's vertical tab rail (SPEC §14.2): the generator sites' tab strip
// turned on its side, on the sidebar's own surface, in two groups, Design (how
// the page looks) and Content (what it says). The rail is one Tab stop, the
// chosen tab, which has the keyboard focus itself (a screen reader announces
// each): ↑/↓ move to the next tab (Ctrl+1…9 are the editor's). The chosen tab
// is `profiles.lastTab`, which the controller remembers for this viewer.
//
// This file also carries the editor's small shared chrome, as inline
// components the rest of the editor uses as ProfileEditorRail.Glyph,
// ProfileEditorRail.Button, ProfileEditorRail.DefaultButton and
// ProfileEditorRail.Tip (see below).
FocusScope {
    id: rail
    objectName: "profileEditorRail"

    property var profiles: null

    // In Profile.EditorTab order, so a tab's index is its enum value.
    readonly property var tabs: [
        { index: 0, name: "themes", label: "Themes", glyph: "palette", group: 0 },
        { index: 1, name: "background", label: "Background", glyph: "image", group: 0 },
        { index: 2, name: "boxes", label: "Boxes", glyph: "boxes", group: 0 },
        { index: 3, name: "text", label: "Text", glyph: "text", group: 0 },
        { index: 4, name: "name", label: "Name & FX", glyph: "sparkle", group: 0 },
        { index: 5, name: "about", label: "About me", glyph: "pencil", group: 1 },
        { index: 6, name: "friends", label: "Top Friends", glyph: "people", group: 1 },
        { index: 7, name: "song", label: "Song", glyph: "note", group: 1 },
        { index: 8, name: "layout", label: "Layout", glyph: "columns", group: 1 }
    ]
    readonly property int currentIndex: profiles ? profiles.lastTab : 0
    // Tabs shrink from 52 to 44 px so all nine fit the 720×560 window.
    readonly property int tabHeight: Math.max(44, Math.min(52, Math.floor((height - 12 - 2 * 22) / 9)))

    function choose(index) {
        if (profiles && index >= 0 && index < tabs.length)
            profiles.lastTab = index;
    }
    function findTab(root, name) {
        for (let i = 0; i < root.children.length; ++i) {
            const child = root.children[i];
            if (child.objectName === name)
                return child;
            const found = findTab(child, name);
            if (found)
                return found;
        }
        return null;
    }
    // The focus sits on the chosen tab while the rail has it.
    function focusChosen() {
        const chosen = tabs[currentIndex];
        const item = chosen ? findTab(tabColumn, "profileEditorTab_" + chosen.name) : null;
        if (item && rail.activeFocus && !item.activeFocus)
            item.forceActiveFocus(Qt.TabFocusReason);
    }

    width: 76
    onActiveFocusChanged: if (activeFocus) focusChosen()
    onCurrentIndexChanged: focusChosen()
    Accessible.role: Accessible.PageTabList
    Accessible.name: "Editor tabs"
    Keys.onUpPressed: choose(Math.max(0, currentIndex - 1))
    Keys.onDownPressed: choose(Math.min(tabs.length - 1, currentIndex + 1))

    // The editor's small shared chrome lives here, beside the rail that uses
    // it most: its line glyphs, its two button looks and its tooltip. The bar,
    // the tabs, the dialogs and the preview frame use them, so the editor
    // keeps one drawing of each and needs nothing of the page kit but the
    // page view it previews.

    // A chip-sized Aero button (the mockups' ChromeButton): flat
    // buttonBackground, 1 px buttonBorder, radius 4 and a gloss line, with an
    // optional leading glyph. `checked` shows it pressed in (navSelected).
    component Button: Item {
        id: button
        property string label: ""
        property string glyph: ""
        property int glyphSize: 15
        property int fontPixelSize: 14
        property bool checkable: false
        property bool checked: false
        property color labelColor: Theme.buttonText
        // False where Enter belongs to something else (a dialog's default
        // button): Enter then goes on to the button's parents.
        property bool returnActivates: true
        readonly property bool hovered: buttonMouse.containsMouse
        signal clicked()
        function press() {
            if (enabled)
                clicked();
        }
        implicitWidth: buttonRow.implicitWidth + (label.length > 0 ? 24 : 14)
        implicitHeight: 30
        width: implicitWidth
        height: implicitHeight
        opacity: enabled ? 1 : 0.55
        activeFocusOnTab: enabled
        Accessible.role: checkable ? Accessible.CheckBox : Accessible.Button
        Accessible.name: label
        Accessible.checkable: checkable
        Accessible.checked: checked
        Accessible.onPressAction: press()
        Keys.onReturnPressed: event => {
            if (returnActivates)
                press();
            else
                event.accepted = false;
        }
        Keys.onEnterPressed: event => {
            if (returnActivates)
                press();
            else
                event.accepted = false;
        }
        Keys.onSpacePressed: press()

        Rectangle {
            anchors.fill: parent
            radius: 4
            color: button.checked ? Theme.navSelected
                                  : button.hovered && button.enabled ? Theme.buttonHover : Theme.buttonBackground
            border.width: button.activeFocus ? 2 : 1
            border.color: button.activeFocus || button.checked || (button.hovered && button.enabled)
                          ? Theme.focusBorder : Theme.buttonBorder
            Rectangle { x: 4; y: 1; width: parent.width - 8; height: 1; color: Theme.gloss }
        }
        Row {
            id: buttonRow
            anchors.centerIn: parent
            spacing: 6
            Glyph {
                visible: button.glyph.length > 0
                anchors.verticalCenter: parent.verticalCenter
                width: button.glyphSize
                height: button.glyphSize
                kind: button.glyph
                ink: Theme.iconInk
                stroke: 1.5
            }
            Text {
                visible: button.label.length > 0
                anchors.verticalCenter: parent.verticalCenter
                text: button.label
                color: button.labelColor
                font.family: Theme.uiFont
                font.pixelSize: button.fontPixelSize
                renderType: Text.NativeRendering
            }
        }
        MouseArea {
            id: buttonMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: button.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
            onClicked: button.press()
        }
    }

    // The Windows 7 default button (SPEC §18.1): pale blue glass with a glass
    // edge at its middle, a focusBorder rim (2 px when focused), an inner
    // white ring and a bold label. Save, Keep editing.
    component DefaultButton: Item {
        id: defaultButton
        property string label: ""
        property int fontPixelSize: 14
        readonly property bool hovered: defaultMouse.containsMouse
        signal clicked()
        function press() {
            if (enabled)
                clicked();
        }
        implicitWidth: defaultLabel.implicitWidth + 32
        implicitHeight: 30
        width: implicitWidth
        height: implicitHeight
        opacity: enabled ? 1 : 0.55
        activeFocusOnTab: enabled
        Accessible.role: Accessible.Button
        Accessible.name: label
        Accessible.onPressAction: press()
        Keys.onReturnPressed: press()
        Keys.onEnterPressed: press()
        Keys.onSpacePressed: press()

        Rectangle {
            anchors.fill: parent
            radius: 4
            border.width: defaultButton.activeFocus ? 2 : 1
            border.color: Theme.focusBorder
            gradient: Gradient {
                GradientStop {
                    position: 0
                    color: defaultButton.hovered && defaultButton.enabled ? Qt.lighter(Theme.defaultButtonTop, 1.04)
                                                                          : Theme.defaultButtonTop
                }
                GradientStop { position: 0.5; color: Qt.darker(Theme.defaultButtonTop, 1.02) }
                GradientStop { position: 0.51; color: Qt.lighter(Theme.defaultButtonBottom, 1.03) }
                GradientStop { position: 1; color: Theme.defaultButtonBottom }
            }
            Rectangle {
                anchors.fill: parent
                anchors.margins: defaultButton.activeFocus ? 2 : 1
                radius: 3
                color: "transparent"
                border.width: 1
                border.color: Theme.defaultButtonInner
            }
        }
        Text {
            id: defaultLabel
            anchors.centerIn: parent
            text: defaultButton.label
            color: Theme.defaultButtonText
            font.family: Theme.uiFont
            font.pixelSize: defaultButton.fontPixelSize
            font.bold: true
            renderType: Text.NativeRendering
        }
        MouseArea {
            id: defaultMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: defaultButton.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
            onClicked: defaultButton.press()
        }
    }

    // A tooltip in the Aero tooltip chrome (the tooltip gradient, border and
    // highlight, a y+2 shadow), with an optional bold title line.
    component Tip: Controls.ToolTip {
        id: tip
        property string title: ""
        delay: 600
        padding: 0
        topPadding: 0
        bottomPadding: 0
        leftPadding: 0
        rightPadding: 0
        background: Item {
            implicitWidth: tipColumn.implicitWidth
            implicitHeight: tipColumn.implicitHeight
            Rectangle {
                y: 2
                width: parent.width
                height: parent.height
                radius: 5
                color: Theme.tooltipShadowFill
            }
            Rectangle {
                anchors.fill: parent
                radius: 5
                border.width: 1
                border.color: Theme.tooltipBorder
                gradient: Gradient {
                    GradientStop { position: 0; color: Theme.tooltipTop }
                    GradientStop { position: 0.48; color: Theme.tooltipMid }
                    GradientStop { position: 1; color: Theme.tooltipBottom }
                }
                Rectangle { x: 5; y: 1; width: parent.width - 10; height: 1; color: Theme.tooltipHighlight }
            }
        }
        contentItem: Column {
            id: tipColumn
            padding: 8
            leftPadding: 11
            rightPadding: 11
            spacing: 3
            Text {
                visible: tip.title.length > 0
                width: Math.min(implicitWidth, 270)
                wrapMode: Text.Wrap
                text: tip.title
                color: Theme.textPrimary
                font.family: Theme.uiFont
                font.pixelSize: 13
                font.bold: true
                renderType: Text.NativeRendering
            }
            Text {
                width: Math.min(implicitWidth, 270)
                wrapMode: Text.Wrap
                text: tip.text
                color: Theme.textPrimary
                font.family: Theme.uiFont
                font.pixelSize: 12
                renderType: Text.NativeRendering
            }
        }
    }

    // The editor's line glyphs (the mockups' Glyph.qml, drawn on a Canvas so
    // one small item recolours at run time).
    component Glyph: Canvas {
        id: glyph
        property string kind: "check"
        property color ink: Theme.iconInk
        property real stroke: Math.max(1.4, width / 12)
        implicitWidth: 16
        implicitHeight: 16
        Accessible.ignored: true
        onInkChanged: requestPaint()
        onKindChanged: requestPaint()
        onStrokeChanged: requestPaint()

        function roundRect(c, x, y, w, h, r) {
            c.beginPath();
            c.moveTo(x + r, y);
            c.lineTo(x + w - r, y);
            c.quadraticCurveTo(x + w, y, x + w, y + r);
            c.lineTo(x + w, y + h - r);
            c.quadraticCurveTo(x + w, y + h, x + w - r, y + h);
            c.lineTo(x + r, y + h);
            c.quadraticCurveTo(x, y + h, x, y + h - r);
            c.lineTo(x, y + r);
            c.quadraticCurveTo(x, y, x + r, y);
            c.closePath();
        }
        function circle(c, cx, cy, r) {
            c.beginPath();
            c.arc(cx, cy, r, 0, Math.PI * 2, false);
            c.closePath();
        }
        function ellipse(c, cx, cy, rx, ry) {
            c.save();
            c.translate(cx, cy);
            c.scale(rx, ry);
            circle(c, 0, 0, 1);
            c.restore();
        }
        function sparkle(c, cx, cy, r) {
            c.beginPath();
            c.moveTo(cx, cy - r);
            c.quadraticCurveTo(cx + r * 0.12, cy - r * 0.12, cx + r, cy);
            c.quadraticCurveTo(cx + r * 0.12, cy + r * 0.12, cx, cy + r);
            c.quadraticCurveTo(cx - r * 0.12, cy + r * 0.12, cx - r, cy);
            c.quadraticCurveTo(cx - r * 0.12, cy - r * 0.12, cx, cy - r);
            c.closePath();
        }
        function undoArrow(c, s) {
            c.lineWidth = Math.max(1.6, s / 10);
            c.beginPath();
            c.arc(s * 0.54, s * 0.56, s * 0.28, Math.PI * 1.05, Math.PI * 2.35, false);
            c.stroke();
            c.beginPath();
            c.moveTo(s * 0.12, s * 0.38);
            c.lineTo(s * 0.27, s * 0.6);
            c.lineTo(s * 0.44, s * 0.42);
            c.stroke();
        }
        function eyeOutline(c, s) {
            c.beginPath();
            c.moveTo(s * 0.06, s * 0.5);
            c.quadraticCurveTo(s * 0.5, s * 0.08, s * 0.94, s * 0.5);
            c.quadraticCurveTo(s * 0.5, s * 0.92, s * 0.06, s * 0.5);
            c.closePath();
            c.stroke();
        }

        onPaint: {
            const c = getContext("2d");
            c.reset();
            const s = Math.min(width, height);
            c.translate((width - s) / 2, (height - s) / 2);
            c.strokeStyle = ink;
            c.fillStyle = ink;
            c.lineWidth = stroke;
            c.lineJoin = "round";
            c.lineCap = "round";
            switch (kind) {
            case "back":
                c.lineWidth = Math.max(1.8, s / 9);
                c.beginPath(); c.moveTo(s * 0.62, s * 0.2); c.lineTo(s * 0.32, s * 0.5); c.lineTo(s * 0.62, s * 0.8); c.stroke();
                break;
            case "chevron":
                c.lineWidth = Math.max(1.6, s / 10);
                c.beginPath(); c.moveTo(s * 0.4, s * 0.22); c.lineTo(s * 0.68, s * 0.5); c.lineTo(s * 0.4, s * 0.78); c.stroke();
                break;
            case "up":
                c.beginPath(); c.moveTo(s * 0.22, s * 0.62); c.lineTo(s * 0.5, s * 0.34); c.lineTo(s * 0.78, s * 0.62); c.stroke();
                break;
            case "down":
                c.beginPath(); c.moveTo(s * 0.22, s * 0.38); c.lineTo(s * 0.5, s * 0.66); c.lineTo(s * 0.78, s * 0.38); c.stroke();
                break;
            case "swap":
                c.beginPath(); c.moveTo(s * 0.14, s * 0.36); c.lineTo(s * 0.86, s * 0.36); c.lineTo(s * 0.68, s * 0.2); c.stroke();
                c.beginPath(); c.moveTo(s * 0.86, s * 0.66); c.lineTo(s * 0.14, s * 0.66); c.lineTo(s * 0.32, s * 0.82); c.stroke();
                break;
            case "check":
                c.lineWidth = Math.max(1.8, s / 8);
                c.beginPath(); c.moveTo(s * 0.2, s * 0.52); c.lineTo(s * 0.42, s * 0.74); c.lineTo(s * 0.82, s * 0.28); c.stroke();
                break;
            case "cross":
                c.lineWidth = Math.max(1.8, s / 8);
                c.beginPath(); c.moveTo(s * 0.24, s * 0.24); c.lineTo(s * 0.76, s * 0.76);
                c.moveTo(s * 0.76, s * 0.24); c.lineTo(s * 0.24, s * 0.76); c.stroke();
                break;
            case "plus":
                c.lineWidth = Math.max(1.8, s / 8);
                c.beginPath(); c.moveTo(s * 0.5, s * 0.18); c.lineTo(s * 0.5, s * 0.82);
                c.moveTo(s * 0.18, s * 0.5); c.lineTo(s * 0.82, s * 0.5); c.stroke();
                break;
            case "undo":
                undoArrow(c, s);
                break;
            case "redo":
                c.translate(s, 0);
                c.scale(-1, 1);
                undoArrow(c, s);
                break;
            case "eye":
                eyeOutline(c, s);
                circle(c, s * 0.5, s * 0.5, s * 0.14); c.fill();
                break;
            case "eyeOff":
                eyeOutline(c, s);
                c.beginPath(); c.moveTo(s * 0.14, s * 0.86); c.lineTo(s * 0.86, s * 0.14); c.stroke();
                break;
            case "palette":
                c.beginPath();
                c.moveTo(s * 0.5, s * 0.08);
                c.bezierCurveTo(s * 0.1, s * 0.08, s * 0.04, s * 0.5, s * 0.14, s * 0.7);
                c.bezierCurveTo(s * 0.26, s * 0.94, s * 0.5, s * 0.96, s * 0.56, s * 0.84);
                c.bezierCurveTo(s * 0.62, s * 0.72, s * 0.52, s * 0.66, s * 0.62, s * 0.6);
                c.bezierCurveTo(s * 0.74, s * 0.54, s * 0.94, s * 0.66, s * 0.94, s * 0.46);
                c.bezierCurveTo(s * 0.94, s * 0.22, s * 0.74, s * 0.08, s * 0.5, s * 0.08);
                c.closePath();
                c.stroke();
                circle(c, s * 0.3, s * 0.36, s * 0.065); c.fill();
                circle(c, s * 0.5, s * 0.25, s * 0.065); c.fill();
                circle(c, s * 0.7, s * 0.34, s * 0.065); c.fill();
                circle(c, s * 0.3, s * 0.6, s * 0.065); c.fill();
                break;
            case "image":
                roundRect(c, s * 0.08, s * 0.16, s * 0.84, s * 0.68, s * 0.08); c.stroke();
                c.beginPath(); c.moveTo(s * 0.14, s * 0.76); c.lineTo(s * 0.38, s * 0.5); c.lineTo(s * 0.54, s * 0.64);
                c.lineTo(s * 0.66, s * 0.54); c.lineTo(s * 0.86, s * 0.76); c.closePath(); c.fill();
                circle(c, s * 0.68, s * 0.34, s * 0.07); c.fill();
                break;
            case "boxes":
                roundRect(c, s * 0.1, s * 0.12, s * 0.8, s * 0.76, s * 0.08); c.stroke();
                c.fillRect(s * 0.1, s * 0.12, s * 0.8, s * 0.22);
                c.beginPath(); c.moveTo(s * 0.24, s * 0.52); c.lineTo(s * 0.76, s * 0.52);
                c.moveTo(s * 0.24, s * 0.68); c.lineTo(s * 0.6, s * 0.68); c.stroke();
                break;
            case "text":
                c.font = "bold " + Math.round(s * 0.62) + "px \"" + Theme.uiFont + "\"";
                c.textBaseline = "middle";
                c.fillText("Aa", s * 0.02, s * 0.54);
                break;
            case "sparkle":
                sparkle(c, s * 0.42, s * 0.46, s * 0.36); c.fill();
                sparkle(c, s * 0.8, s * 0.2, s * 0.16); c.fill();
                break;
            case "pencil":
                c.save();
                c.translate(s * 0.5, s * 0.5);
                c.rotate(Math.PI / 4);
                roundRect(c, -s * 0.12, -s * 0.44, s * 0.24, s * 0.66, s * 0.04); c.stroke();
                c.beginPath(); c.moveTo(-s * 0.12, s * 0.22); c.lineTo(0, s * 0.42); c.lineTo(s * 0.12, s * 0.22); c.stroke();
                c.restore();
                break;
            case "people":
                circle(c, s * 0.36, s * 0.34, s * 0.14); c.fill();
                c.beginPath(); c.moveTo(s * 0.1, s * 0.84); c.quadraticCurveTo(s * 0.12, s * 0.54, s * 0.36, s * 0.54);
                c.quadraticCurveTo(s * 0.6, s * 0.54, s * 0.62, s * 0.84); c.closePath(); c.fill();
                circle(c, s * 0.7, s * 0.3, s * 0.11); c.fill();
                c.beginPath(); c.moveTo(s * 0.66, s * 0.5); c.quadraticCurveTo(s * 0.9, s * 0.48, s * 0.92, s * 0.76);
                c.lineTo(s * 0.68, s * 0.76); c.quadraticCurveTo(s * 0.68, s * 0.6, s * 0.6, s * 0.52); c.closePath(); c.fill();
                break;
            case "note": {
                // Two beamed quavers, sized to sit inside the glyph's box.
                const n = s * 0.7;
                c.translate(s * 0.24, s * 0.76);
                c.lineWidth = Math.max(1.2, n * 0.08);
                ellipse(c, 0, 0, n * 0.26, n * 0.19); c.fill();
                c.beginPath(); c.moveTo(n * 0.23, 0); c.lineTo(n * 0.23, -n * 0.95); c.stroke();
                ellipse(c, n * 0.7, n * 0.08, n * 0.26, n * 0.19); c.fill();
                c.beginPath(); c.moveTo(n * 0.93, n * 0.08); c.lineTo(n * 0.93, -n * 0.87); c.stroke();
                c.lineWidth = n * 0.2;
                c.beginPath(); c.moveTo(n * 0.2, -n * 0.9); c.lineTo(n * 0.96, -n * 0.82); c.stroke();
                break;
            }
            case "columns":
                roundRect(c, s * 0.08, s * 0.14, s * 0.32, s * 0.72, s * 0.06); c.stroke();
                roundRect(c, s * 0.48, s * 0.14, s * 0.44, s * 0.32, s * 0.06); c.stroke();
                roundRect(c, s * 0.48, s * 0.54, s * 0.44, s * 0.32, s * 0.06); c.stroke();
                break;
            case "lock":
                roundRect(c, s * 0.2, s * 0.44, s * 0.6, s * 0.46, s * 0.08); c.fill();
                c.beginPath(); c.arc(s * 0.5, s * 0.44, s * 0.2, Math.PI, 0, false); c.stroke();
                break;
            case "grip":
                for (let gy = 0; gy < 3; ++gy) {
                    for (let gx = 0; gx < 2; ++gx) {
                        circle(c, s * (0.38 + gx * 0.24), s * (0.26 + gy * 0.24), s * 0.07);
                        c.fill();
                    }
                }
                break;
            case "camera":
                roundRect(c, s * 0.08, s * 0.28, s * 0.84, s * 0.56, s * 0.1); c.stroke();
                c.beginPath(); c.moveTo(s * 0.32, s * 0.28); c.lineTo(s * 0.4, s * 0.16); c.lineTo(s * 0.6, s * 0.16);
                c.lineTo(s * 0.68, s * 0.28); c.stroke();
                circle(c, s * 0.5, s * 0.56, s * 0.16); c.stroke();
                break;
            case "play":
                c.beginPath(); c.moveTo(s * 0.34, s * 0.2); c.lineTo(s * 0.82, s * 0.5); c.lineTo(s * 0.34, s * 0.8);
                c.closePath(); c.fill();
                break;
            case "pause":
                c.fillRect(s * 0.28, s * 0.2, s * 0.16, s * 0.6);
                c.fillRect(s * 0.56, s * 0.2, s * 0.16, s * 0.6);
                break;
            case "dice":
                roundRect(c, s * 0.12, s * 0.12, s * 0.76, s * 0.76, s * 0.14); c.stroke();
                circle(c, s * 0.34, s * 0.34, s * 0.07); c.fill();
                circle(c, s * 0.5, s * 0.5, s * 0.07); c.fill();
                circle(c, s * 0.66, s * 0.66, s * 0.07); c.fill();
                break;
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0; color: Theme.sidebarTop }
            GradientStop { position: 1; color: Theme.sidebarBottom }
        }
    }
    Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.rule }

    Column {
        id: tabColumn
        y: 4
        width: parent.width - 1

        Repeater {
            model: 2

            Column {
                id: group
                required property int index
                width: parent.width

                Item {
                    width: parent.width
                    height: 22
                    Rectangle {
                        visible: group.index > 0
                        x: 10; y: 3
                        width: parent.width - 20
                        height: 1
                        color: Theme.rule
                    }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        y: group.index > 0 ? 7 : 5
                        text: group.index === 0 ? "Design" : "Content"
                        color: Theme.textSecondaryStrong
                        font.family: Theme.uiFont
                        font.pixelSize: 11
                        renderType: Text.NativeRendering
                        Accessible.ignored: true
                    }
                }

                Repeater {
                    model: rail.tabs.filter(tab => tab.group === group.index)

                    Item {
                        id: tab
                        required property var modelData
                        readonly property int tabIndex: modelData.index
                        readonly property bool selected: tabIndex === rail.currentIndex
                        objectName: "profileEditorTab_" + modelData.name
                        width: parent.width
                        height: rail.tabHeight
                        // Until the focus has left it, the focused tab stays one.
                        activeFocusOnTab: selected || activeFocus
                        Accessible.role: Accessible.PageTab
                        Accessible.name: modelData.label
                        Accessible.selected: selected
                        Accessible.onPressAction: rail.choose(tabIndex)

                        Rectangle {
                            visible: tab.selected
                            anchors.fill: parent
                            anchors.topMargin: 1
                            anchors.bottomMargin: 1
                            gradient: Gradient {
                                orientation: Gradient.Horizontal
                                GradientStop { position: 0; color: Theme.selectedTop }
                                GradientStop { position: 1; color: Theme.selectedBottom }
                            }
                        }
                        Rectangle {
                            visible: tab.selected
                            y: 5
                            width: 3
                            height: parent.height - 10
                            radius: 1.5
                            color: Theme.focusBorder
                        }
                        Rectangle {
                            visible: !tab.selected && tabMouse.containsMouse
                            anchors.fill: parent
                            anchors.topMargin: 1
                            anchors.bottomMargin: 1
                            color: Theme.buttonHover
                            opacity: 0.7
                        }
                        // The rail's keyboard focus sits on the chosen tab.
                        Rectangle {
                            visible: tab.selected && rail.activeFocus
                            anchors.fill: parent
                            anchors.margins: 3
                            radius: 3
                            color: "transparent"
                            border.width: 2
                            border.color: Theme.focusBorder
                        }
                        Column {
                            anchors.centerIn: parent
                            spacing: 4
                            Glyph {
                                anchors.horizontalCenter: parent.horizontalCenter
                                width: 20
                                height: 20
                                kind: tab.modelData.glyph
                                ink: tab.selected ? Theme.focusBorder : Theme.iconInk
                                stroke: 1.6
                            }
                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: tab.modelData.label
                                color: tab.selected ? Theme.textPrimary : Theme.buttonText
                                font.family: Theme.uiFont
                                font.pixelSize: 11
                                renderType: Text.NativeRendering
                            }
                        }
                        MouseArea {
                            id: tabMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: rail.choose(tab.tabIndex)
                        }
                    }
                }
            }
        }
    }
}
