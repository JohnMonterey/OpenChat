import QtQuick
import QtQuick.Controls
import OpenChat
import OpenChat.Native

// The editor's Aero colour picker (SPEC §14.13): a 276 px popover in the
// tooltip chrome with a nose pointing back at its well. Top to bottom: title
// and close ×, "From your page" (the draft's own eight colours), 21 MySpace-era
// Classics, a saturation × value field and a hue strip (gradient Rectangles,
// no Canvas), the hex field beside a was | now swatch, Recent, and for text
// colours the readability line: the colour measured where it will sit, and
// what the page will really show.
//
// Every change previews live through the well's `picked`, and the whole
// session is one undo step (a gesture on the controller). Enter (on a swatch
// too: it takes that colour first; Space only takes it), the ×, or a click
// outside keep the colour (it joins Recent); Esc puts back the colour the well
// had ("was") and closes.
Popup {
    id: picker
    objectName: "profileColorPicker"

    property var profiles: null
    property var page: null // the draft
    property Item well: null
    // Where the picker's left edge sits, in the overlay's coordinates: just
    // over the panel's right edge, so it never covers the fields it edits.
    property real anchorX: 0

    property color original: "#ffffff"
    property color value: "#ffffff"
    // Kept apart from `value`, so the hue survives a trip through grey and
    // black (a colour with no saturation has no hue of its own).
    property real hue: 0
    property real saturation: 0
    property real brightness: 1
    property bool reverting: false
    // This session's undo gesture, closed (only it) when the picker closes.
    property string gestureKey: ""

    readonly property int inkRole: well ? well.inkRole : -1
    readonly property var report: inkRole >= 0 && profiles ? profiles.contrastFor(inkRole, value) : ({})
    readonly property bool hasReport: report.ratio !== undefined
    readonly property bool onStrip: inkRole === Profile.HeaderTextInk || inkRole === Profile.AltHeaderTextInk
    readonly property var classics: ["#000000", "#ffffff", "#ff2e97", "#ff69b4", "#c2186b", "#8a2be2", "#b8a1ff",
                                     "#ff0000", "#e0212e", "#ff7a00", "#ffd400", "#b6ff00", "#39ff14", "#00c853",
                                     "#00e5ff", "#35d4ff", "#6699cc", "#003399", "#5d4037", "#9e9e9e", "#ffcc99"]
    readonly property real noseY: {
        if (!well || !parent)
            return 40;
        const centre = well.mapToItem(parent, 0, well.height - 15).y;
        return Math.max(18, Math.min(height - 18, centre - y));
    }

    function hexOf(colour) {
        return String(colour).slice(1, 7).toUpperCase();
    }
    function openFor(target, left) {
        well = target;
        anchorX = left;
        original = target.color;
        value = target.color;
        const h = target.color.hsvHue;
        hue = h >= 0 ? h : 0;
        saturation = target.color.hsvSaturation;
        brightness = target.color.hsvValue;
        reverting = false;
        gestureKey = "colour:" + target.pickerTitle;
        if (profiles)
            profiles.beginGesture(gestureKey);
        open();
    }
    // A colour from a swatch or the hex field: its own hue, unless it has none.
    function takeColour(colour) {
        const c = Qt.color(colour);
        if (c.hsvHue >= 0)
            hue = c.hsvHue;
        saturation = c.hsvSaturation;
        brightness = c.hsvValue;
        apply(c);
    }
    function takeHsv(h, s, v) {
        hue = Math.max(0, Math.min(1, h));
        saturation = Math.max(0, Math.min(1, s));
        brightness = Math.max(0, Math.min(1, v));
        apply(Qt.hsva(hue, saturation, brightness, 1));
    }
    function apply(colour) {
        value = colour;
        if (well)
            well.picked(colour);
    }
    function commit() {
        close();
    }
    function revert() {
        reverting = true;
        apply(original);
        close();
    }

    parent: Overlay.overlay
    x: anchorX
    y: {
        if (!well || !parent)
            return 0;
        const centre = well.mapToItem(parent, 0, well.height - 15).y;
        return Math.max(8, Math.min(parent.height - height - 8, centre - 110));
    }
    width: 276
    padding: 0
    modal: false
    focus: true
    closePolicy: Popup.CloseOnPressOutside
    onOpened: sections.forceActiveFocus(Qt.PopupFocusReason)
    onClosed: {
        if (!reverting && profiles)
            profiles.rememberColor(value);
        if (profiles)
            profiles.endGesture(gestureKey);
        if (well)
            well.forceActiveFocus(Qt.PopupFocusReason);
        well = null;
    }

    component Swatch: Rectangle {
        id: swatch
        property color swatchColor: "#000000"
        property bool chosen: false
        radius: 3
        color: swatchColor
        border.width: chosen ? 2 : 1
        border.color: chosen ? Theme.focusBorder : Qt.darker(swatchColor, 1.35)
        Accessible.role: Accessible.Button
        Accessible.name: String(swatchColor).toUpperCase()
        Rectangle {
            x: 2; y: 1
            width: parent.width - 4
            height: Math.max(1, parent.height * 0.4)
            radius: 2
            color: "#30ffffff"
        }
        Rectangle {
            visible: swatch.chosen
            anchors.fill: parent
            anchors.margins: 2
            radius: 2
            color: "transparent"
            border.width: 1
            border.color: "white"
        }
    }

    background: Item {
        Rectangle {
            y: 3
            width: parent.width
            height: parent.height
            radius: 6
            color: Theme.tooltipShadowFill
        }
        Rectangle {
            anchors.fill: parent
            radius: 6
            border.width: 1
            border.color: Theme.tooltipBorder
            gradient: Gradient {
                GradientStop { position: 0; color: Theme.tooltipTop }
                GradientStop { position: 0.2; color: Theme.tooltipMid }
                GradientStop { position: 1; color: Theme.tooltipBottom }
            }
            Rectangle {
                x: 1; y: 1
                width: parent.width - 2
                height: 16
                radius: 5
                gradient: Gradient {
                    GradientStop { position: 0; color: Theme.glossStrong }
                    GradientStop { position: 1; color: "transparent" }
                }
            }
        }
        // The nose, pointing back at the well: a square turned 45°, half of
        // it tucked under the body.
        Item {
            x: -7
            y: picker.noseY - 8
            width: 8
            height: 16
            clip: true
            Rectangle {
                x: 3
                y: 2
                width: 11
                height: 11
                rotation: 45
                color: Theme.tooltipMid
                border.width: 1
                border.color: Theme.tooltipBorder
            }
        }
    }

    contentItem: FocusScope {
        id: sections
        implicitHeight: column.implicitHeight + 24
        Keys.onEscapePressed: picker.revert()
        Keys.onReturnPressed: picker.commit()
        Keys.onEnterPressed: picker.commit()

        Column {
            id: column
            x: 14
            y: 12
            width: parent.width - 28
            spacing: 10

            Item {
                width: parent.width
                height: 20
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - 24
                    elide: Text.ElideRight
                    text: picker.well ? picker.well.pickerTitle : ""
                    color: Theme.textPrimary
                    font.family: Theme.uiFont
                    font.pixelSize: 15
                    renderType: Text.NativeRendering
                }
                Item {
                    id: closeButton
                    objectName: "profileColorPickerClose"
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    width: 18
                    height: 18
                    // Closes keeping the colour, like Enter.
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: "Close"
                    Accessible.onPressAction: picker.commit()
                    Keys.onReturnPressed: picker.commit()
                    Keys.onEnterPressed: picker.commit()
                    Keys.onSpacePressed: picker.commit()
                    ProfileEditorRail.Glyph {
                        anchors.centerIn: parent
                        width: 14
                        height: 14
                        kind: "cross"
                        ink: closeMouse.containsMouse || closeButton.activeFocus ? Theme.focusBorder : Theme.iconInk
                        stroke: 1.8
                    }
                    Rectangle {
                        visible: closeButton.activeFocus
                        anchors.fill: parent
                        anchors.margins: -2
                        radius: 3
                        color: "transparent"
                        border.width: 2
                        border.color: Theme.focusBorder
                    }
                    MouseArea {
                        id: closeMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: picker.commit()
                    }
                }
            }

            Column {
                spacing: 5
                visible: pageSwatches.count > 0
                Text {
                    text: "From your page"
                    color: Theme.textSecondaryStrong
                    font.family: Theme.uiFont
                    font.pixelSize: 12
                    renderType: Text.NativeRendering
                }
                ProfileTileGrid {
                    id: pageSwatches
                    objectName: "profileColorPickerPage"
                    accessibleName: "Colours from your page"
                    model: picker.page ? picker.page.paletteSwatches : []
                    columns: 8
                    ringRadius: 3
                    currentIndex: -1
                    onActivated: index => picker.takeColour(model[index])
                    onSubmitted: picker.commit()
                    delegate: Swatch {
                        required property var modelData
                        required property int index
                        width: 26
                        height: 26
                        swatchColor: modelData
                        chosen: Qt.colorEqual(modelData, picker.value)
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: pageSwatches.activate(parent.index)
                        }
                    }
                }
            }

            Column {
                spacing: 5
                Text {
                    text: "Classics"
                    color: Theme.textSecondaryStrong
                    font.family: Theme.uiFont
                    font.pixelSize: 12
                    renderType: Text.NativeRendering
                }
                ProfileTileGrid {
                    id: classicSwatches
                    objectName: "profileColorPickerClassics"
                    accessibleName: "Classic colours"
                    model: picker.classics
                    columns: 7
                    columnSpacing: 5
                    rowSpacing: 5
                    ringRadius: 3
                    onActivated: index => picker.takeColour(picker.classics[index])
                    onSubmitted: picker.commit()
                    delegate: Swatch {
                        required property var modelData
                        required property int index
                        objectName: "profileClassicSwatch_" + String(modelData).slice(1)
                        width: 30
                        height: 20
                        swatchColor: modelData
                        chosen: Qt.colorEqual(modelData, picker.value)
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: classicSwatches.activate(parent.index)
                        }
                    }
                }
            }

            // Saturation × value, over the current hue.
            Item {
                id: field
                objectName: "profileColorPickerField"
                width: parent.width
                height: 118
                activeFocusOnTab: true
                Accessible.role: Accessible.Slider
                Accessible.name: "Saturation and brightness"
                Keys.onLeftPressed: event => picker.takeHsv(picker.hue, picker.saturation - step(event), picker.brightness)
                Keys.onRightPressed: event => picker.takeHsv(picker.hue, picker.saturation + step(event), picker.brightness)
                Keys.onUpPressed: event => picker.takeHsv(picker.hue, picker.saturation, picker.brightness + step(event))
                Keys.onDownPressed: event => picker.takeHsv(picker.hue, picker.saturation, picker.brightness - step(event))
                function step(event) {
                    return event.modifiers & Qt.ShiftModifier ? 0.1 : 0.01;
                }
                function pick(x, y) {
                    picker.takeHsv(picker.hue, x / (width - 1), 1 - y / (height - 1));
                }

                Rectangle {
                    anchors.fill: parent
                    radius: 4
                    color: Qt.hsva(picker.hue, 1, 1, 1)
                }
                Rectangle {
                    anchors.fill: parent
                    radius: 4
                    gradient: Gradient {
                        orientation: Gradient.Horizontal
                        GradientStop { position: 0; color: "#ffffffff" }
                        GradientStop { position: 1; color: "#00ffffff" }
                    }
                }
                Rectangle {
                    anchors.fill: parent
                    radius: 4
                    border.width: field.activeFocus ? 2 : 1
                    border.color: field.activeFocus ? Theme.focusBorder : Theme.inputBorder
                    gradient: Gradient {
                        GradientStop { position: 0; color: "#00000000" }
                        GradientStop { position: 1; color: "#ff000000" }
                    }
                }
                Rectangle {
                    x: Math.round(picker.saturation * (parent.width - 1)) - 7
                    y: Math.round((1 - picker.brightness) * (parent.height - 1)) - 7
                    width: 14
                    height: 14
                    radius: 7
                    color: "transparent"
                    border.width: 2
                    border.color: "white"
                    Rectangle {
                        anchors.centerIn: parent
                        width: 16
                        height: 16
                        radius: 8
                        color: "transparent"
                        border.width: 1
                        border.color: "#80000000"
                    }
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.CrossCursor
                    onPressed: mouse => field.pick(mouse.x, mouse.y)
                    onPositionChanged: mouse => {
                        if (pressed)
                            field.pick(Math.max(0, Math.min(field.width - 1, mouse.x)),
                                       Math.max(0, Math.min(field.height - 1, mouse.y)));
                    }
                }
            }

            Item {
                id: hueStrip
                objectName: "profileColorPickerHue"
                width: parent.width
                height: 14
                activeFocusOnTab: true
                Accessible.role: Accessible.Slider
                Accessible.name: "Hue"
                Keys.onLeftPressed: event => picker.takeHsv((picker.hue * 360 - step(event)) / 360, picker.saturation, picker.brightness)
                Keys.onRightPressed: event => picker.takeHsv((picker.hue * 360 + step(event)) / 360, picker.saturation, picker.brightness)
                function step(event) {
                    return event.modifiers & Qt.ShiftModifier ? 10 : 1;
                }
                function pick(x) {
                    picker.takeHsv(Math.max(0, Math.min(width - 1, x)) / (width - 1), picker.saturation, picker.brightness);
                }

                Rectangle {
                    y: 2.5
                    width: parent.width
                    height: 9
                    radius: 4
                    border.width: hueStrip.activeFocus ? 2 : 1
                    border.color: hueStrip.activeFocus ? Theme.focusBorder : Theme.inputBorder
                    gradient: Gradient {
                        orientation: Gradient.Horizontal
                        GradientStop { position: 0; color: "#ff0000" }
                        GradientStop { position: 1 / 6; color: "#ffff00" }
                        GradientStop { position: 2 / 6; color: "#00ff00" }
                        GradientStop { position: 3 / 6; color: "#00ffff" }
                        GradientStop { position: 4 / 6; color: "#0000ff" }
                        GradientStop { position: 5 / 6; color: "#ff00ff" }
                        GradientStop { position: 1; color: "#ff0000" }
                    }
                }
                Rectangle {
                    x: Math.round(picker.hue * (parent.width - 1)) - 7
                    anchors.verticalCenter: parent.verticalCenter
                    width: 14
                    height: 14
                    radius: 7
                    border.width: 1
                    border.color: Theme.buttonBorder
                    gradient: Gradient {
                        GradientStop { position: 0; color: Theme.switchKnobTop }
                        GradientStop { position: 1; color: Theme.switchKnobBottom }
                    }
                }
                MouseArea {
                    anchors.fill: parent
                    anchors.topMargin: -3
                    anchors.bottomMargin: -3
                    cursorShape: Qt.PointingHandCursor
                    onPressed: mouse => hueStrip.pick(mouse.x)
                    onPositionChanged: mouse => {
                        if (pressed)
                            hueStrip.pick(mouse.x);
                    }
                }
            }

            Row {
                spacing: 10
                AeroTextField {
                    id: hex
                    objectName: "profileColorPickerHex"
                    width: 132
                    height: 32
                    prefix: "#"
                    fontPixelSize: 14
                    accessibleName: "Hex colour"
                    text: picker.hexOf(picker.value)
                    onTextChanged: {
                        const typed = text.trim().replace(/^#/, "");
                        if (/^[0-9a-fA-F]{6}$/.test(typed) && typed.toUpperCase() !== picker.hexOf(picker.value))
                            picker.takeColour("#" + typed);
                    }
                    onAccepted: picker.commit()
                }
                // was | now
                Rectangle {
                    width: column.width - 142
                    height: 32
                    radius: 4
                    color: "transparent"
                    border.width: 1
                    border.color: Theme.inputBorder
                    Row {
                        x: 1
                        y: 1
                        Rectangle {
                            objectName: "profileColorPickerWas"
                            width: (column.width - 144) / 2
                            height: 30
                            radius: 3
                            color: picker.original
                            Text {
                                x: 6
                                anchors.verticalCenter: parent.verticalCenter
                                text: "was"
                                color: picker.original.hslLightness > 0.6 ? "black" : "white"
                                font.family: Theme.uiFont
                                font.pixelSize: 11
                                renderType: Text.NativeRendering
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: picker.takeColour(picker.original)
                            }
                        }
                        Rectangle {
                            objectName: "profileColorPickerNow"
                            width: (column.width - 144) / 2
                            height: 30
                            radius: 3
                            color: picker.value
                            Text {
                                anchors.right: parent.right
                                anchors.rightMargin: 6
                                anchors.verticalCenter: parent.verticalCenter
                                text: "now"
                                color: picker.value.hslLightness > 0.6 ? "black" : "white"
                                font.family: Theme.uiFont
                                font.pixelSize: 11
                                renderType: Text.NativeRendering
                            }
                        }
                    }
                }
            }

            Row {
                spacing: 5
                visible: recentSwatches.count > 0
                Text {
                    width: 46
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Recent"
                    color: Theme.textSecondaryStrong
                    font.family: Theme.uiFont
                    font.pixelSize: 12
                    renderType: Text.NativeRendering
                }
                ProfileTileGrid {
                    id: recentSwatches
                    objectName: "profileColorPickerRecent"
                    accessibleName: "Recent colours"
                    model: picker.profiles ? picker.profiles.recentColors : []
                    columns: 8
                    ringRadius: 3
                    onActivated: index => picker.takeColour(model[index])
                    onSubmitted: picker.commit()
                    delegate: Swatch {
                        required property var modelData
                        required property int index
                        width: 22
                        height: 22
                        swatchColor: modelData
                        chosen: Qt.colorEqual(modelData, picker.value)
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: recentSwatches.activate(parent.index)
                        }
                    }
                }
            }

            // Text colours: how readable this one is where it will sit.
            Rectangle {
                objectName: "profileColorPickerReadability"
                readonly property bool passes: picker.report.passes === true
                readonly property color shown: picker.hasReport ? picker.report.shown : picker.value
                visible: picker.hasReport
                width: parent.width
                height: 44
                radius: 4
                color: passes ? Theme.panelBackground : Theme.warningBackground
                border.width: 1
                border.color: passes ? Theme.inputBorder : Theme.warningBorder
                Accessible.role: Accessible.StaticText
                Accessible.name: ratioText.text + ". " + verdictText.text

                // "Aa" on what the text will really sit on: the box over the
                // base colour, or the strip.
                Item {
                    x: 6
                    y: 6
                    width: 58
                    height: 32
                    Rectangle {
                        anchors.fill: parent
                        radius: 3
                        color: picker.page ? (picker.onStrip ? picker.stripFill() : picker.page.render.color1) : "white"
                    }
                    Rectangle {
                        visible: !picker.onStrip
                        anchors.fill: parent
                        radius: 3
                        color: picker.page ? picker.page.render.boxFill : "white"
                    }
                    Text {
                        anchors.centerIn: parent
                        text: "Aa"
                        color: picker.value
                        font.family: Theme.uiFont
                        font.pixelSize: 15
                        font.bold: true
                        renderType: Text.NativeRendering
                    }
                }
                Column {
                    x: 72
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 1
                    Text {
                        id: ratioText
                        objectName: "profileColorPickerRatio"
                        text: picker.hasReport
                              ? Number(picker.report.ratio).toFixed(1) + " : 1 on " + (picker.onStrip ? "its strip" : "your boxes")
                              : ""
                        color: parent.parent.passes ? Theme.textPrimary : Theme.warningText
                        font.family: Theme.uiFont
                        font.pixelSize: 13
                        font.bold: true
                        renderType: Text.NativeRendering
                    }
                    Text {
                        id: verdictText
                        objectName: "profileColorPickerVerdict"
                        text: parent.parent.passes ? "Easy to read"
                              : "Too faint, so shown "
                                + (parent.parent.shown.hslLightness < picker.value.hslLightness ? "darker" : "lighter")
                        color: parent.parent.passes ? Theme.textSecondaryStrong : Theme.warningText
                        font.family: Theme.uiFont
                        font.pixelSize: 12
                        renderType: Text.NativeRendering
                    }
                }
            }
        }
    }

    // The strip a header text colour sits on (the recipe's glass edge, F).
    function stripFill() {
        const render = page ? page.render : null;
        if (!render)
            return "white";
        const stops = inkRole === Profile.AltHeaderTextInk ? render.altStripStops : render.stripStops;
        return stops.length > 2 ? stops[2] : stops.length > 0 ? stops[0] : render.boxFill;
    }
}
