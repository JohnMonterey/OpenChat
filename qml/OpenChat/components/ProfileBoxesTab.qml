import QtQuick
import OpenChat
import OpenChat.Native

// Boxes (SPEC §14.7, `final-editor-boxes.png`): the box colour and how see-
// through it is (0–40%, i.e. 60–100% opaque), the border's colour, width and
// style ("Double needs 3 px or more."), the corners drawn at their real radius,
// the neon edge, the table style, the header strip styles drawn in the page's
// own strip colours, the strip colours (the text with its readability badge),
// and a different strip for the right column with its own fill, text and
// border.
Item {
    id: tab
    objectName: "profileBoxesTab"

    property var profiles: null
    property Item editor: null

    readonly property var draft: profiles ? profiles.draft : null
    readonly property int halfWidth: Math.floor((width - 32 - 8) / 2)
    readonly property int quarterWidth: Math.floor((width - 32 - 15) / 4)
    readonly property var radii: [0, 3, 6, 10]

    // The strip tiles' mini strips, in the page's strip colour: the shading of
    // each style, roughly (the real recipe is the renderer's, SPEC §4.2).
    function stripStops(style, fill) {
        const top = style === Profile.GlossHeader ? Qt.lighter(fill, 1.32)
                  : style === Profile.GradientHeader ? Qt.lighter(fill, 1.14) : fill;
        const middle = style === Profile.GlossHeader ? Qt.lighter(fill, 1.1) : style === Profile.GradientHeader ? fill : fill;
        const bottom = style === Profile.FlatHeader ? fill : Qt.darker(fill, 1.1);
        return [top, middle, style === Profile.GlossHeader ? fill : middle, bottom];
    }
    function focusField(field) {
        if (field === "strip")
            strips.forceActiveFocus(Qt.OtherFocusReason);
        else
            boxColour.forceActiveFocus(Qt.OtherFocusReason);
    }

    readonly property string editingTarget: strips.activeFocus || stripColour.activeFocus || stripText.activeFocus
                                            ? "strip" : ""

    implicitHeight: column.y + column.implicitHeight + 16

    Timer {
        id: sliderRest
        interval: 400
        onTriggered: tab.profiles.endGesture()
    }

    Column {
        id: column
        x: 16
        y: 14
        width: tab.width - 32

        Text {
            text: "Boxes"
            color: Theme.categoryText
            font.family: Theme.uiFont
            font.pixelSize: 17
            renderType: Text.NativeRendering
            Accessible.role: Accessible.Heading
            Accessible.name: text
        }
        Item { width: 1; height: 12 }
        Row {
            spacing: 8
            ProfileColorWell {
                id: boxColour
                objectName: "profileBoxColourWell"
                width: tab.halfWidth
                label: "Box colour"
                color: tab.draft ? tab.draft.boxFill : "white"
                editor: tab.editor
                page: tab.draft
                onPicked: colour => tab.draft.boxFill = colour
            }
            Column {
                width: tab.halfWidth
                Item {
                    width: parent.width
                    height: 20
                    Text {
                        text: "See-through"
                        color: Theme.textSecondaryStrong
                        font.family: Theme.uiFont
                        font.pixelSize: 12
                        renderType: Text.NativeRendering
                    }
                    Text {
                        objectName: "profileSeeThroughValue"
                        anchors.right: parent.right
                        text: (tab.draft ? 100 - tab.draft.boxOpacity : 0) + "%"
                        color: Theme.textPrimary
                        font.family: Theme.uiFont
                        font.pixelSize: 12
                        renderType: Text.NativeRendering
                    }
                }
                AeroSlider {
                    objectName: "profileSeeThrough"
                    y: 2
                    width: parent.width
                    accessibleName: "See-through"
                    value: tab.draft ? (100 - tab.draft.boxOpacity) / 40 : 0
                    onMoved: position => {
                        if (!sliderRest.running)
                            tab.profiles.beginGesture("boxes:see-through");
                        sliderRest.restart();
                        tab.draft.boxOpacity = 100 - Math.round(position * 40);
                    }
                }
            }
        }

        Item { width: 1; height: 14 }
        Text {
            text: "Border"
            color: Theme.textSecondaryStrong
            font.family: Theme.uiFont
            font.pixelSize: 12
            renderType: Text.NativeRendering
        }
        Item { width: 1; height: 5 }
        Row {
            spacing: 8
            ProfileColorWell {
                objectName: "profileBorderColourWell"
                width: 64
                pickerTitle: "Border colour"
                color: tab.draft ? tab.draft.borderColor : "white"
                editor: tab.editor
                page: tab.draft
                onPicked: colour => tab.draft.borderColor = colour
            }
            ProfileSegmented {
                objectName: "profileBorderWidth"
                width: column.width - 72
                height: 30
                accessibleName: "Border width"
                options: ["0", "1", "2", "3", "4"]
                fontPixelSize: 12
                currentIndex: tab.draft ? tab.draft.borderWidth : 1
                onActivated: index => tab.draft.borderWidth = index
            }
        }
        Item { width: 1; height: 6 }
        ProfileSegmented {
            objectName: "profileBorderStyle"
            width: parent.width
            accessibleName: "Border style"
            options: ["Solid", "Dashed", "Dotted", "Double"]
            fontPixelSize: 12
            currentIndex: tab.draft ? tab.draft.borderStyle : 0
            onActivated: index => tab.draft.borderStyle = index
        }
        Text {
            objectName: "profileDoubleBorderHint"
            visible: tab.draft !== null && tab.draft.borderStyle === Profile.DoubleBorder && tab.draft.borderWidth < 3
            topPadding: 5
            text: "Double needs 3 px or more."
            color: Theme.warningText
            font.family: Theme.uiFont
            font.pixelSize: 12
            renderType: Text.NativeRendering
        }

        Item { width: 1; height: 14 }
        Text {
            text: "Corners"
            color: Theme.textSecondaryStrong
            font.family: Theme.uiFont
            font.pixelSize: 12
            renderType: Text.NativeRendering
        }
        Item { width: 1; height: 5 }
        ProfileTileGrid {
            id: corners
            objectName: "profileCornerGrid"
            accessibleName: "Corners"
            model: tab.radii
            columns: 4
            columnSpacing: 5
            ringRadius: 4
            currentIndex: tab.draft ? tab.draft.boxRadius : 2
            onActivated: index => tab.draft.boxRadius = index
            delegate: Rectangle {
                id: corner
                required property var modelData
                required property int index
                readonly property bool chosen: index === corners.currentIndex
                objectName: "profileCorner_" + modelData
                width: tab.quarterWidth
                height: 34
                radius: 4
                color: chosen ? Theme.navSelected : cornerMouse.containsMouse ? Theme.buttonHover : Theme.buttonBackground
                border.width: chosen ? 2 : 1
                border.color: chosen ? Theme.focusBorder : Theme.buttonBorder
                Accessible.role: Accessible.RadioButton
                Accessible.name: modelData + " pixel corners"
                Accessible.checkable: true
                Accessible.checked: chosen
                // A corner at its real radius.
                Item {
                    x: 12
                    y: 9
                    width: 22
                    height: 16
                    clip: true
                    Rectangle {
                        width: 40
                        height: 40
                        radius: corner.modelData
                        color: "transparent"
                        border.width: 2
                        border.color: Theme.iconInk
                    }
                }
                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: corner.modelData
                    color: Theme.buttonText
                    font.family: Theme.uiFont
                    font.pixelSize: 11
                    renderType: Text.NativeRendering
                }
                MouseArea {
                    id: cornerMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: corners.activate(corner.index)
                }
            }
        }

        Item { width: 1; height: 12 }
        Item {
            width: parent.width
            height: 28
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "Neon edge"
                color: Theme.textPrimary
                font.family: Theme.uiFont
                font.pixelSize: 13
                renderType: Text.NativeRendering
            }
            AeroSwitch {
                objectName: "profileNeonEdge"
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                width: 46
                height: 26
                accessibleName: "Neon edge"
                checked: tab.draft !== null && tab.draft.boxGlow
                onToggled: checked => tab.draft.boxGlow = checked
            }
        }
        Item { width: 1; height: 8 }
        Text {
            text: "Tables (Interests, Details)"
            color: Theme.textSecondaryStrong
            font.family: Theme.uiFont
            font.pixelSize: 12
            renderType: Text.NativeRendering
        }
        Item { width: 1; height: 5 }
        ProfileSegmented {
            objectName: "profileTableStyle"
            width: parent.width
            accessibleName: "Tables"
            options: ["Tinted cells", "Lines"]
            fontPixelSize: 12
            currentIndex: tab.draft ? tab.draft.tableStyle : 0
            onActivated: index => tab.draft.tableStyle = index
        }

        Item { width: 1; height: 14 }
        Rectangle { width: parent.width; height: 1; color: Theme.softRule }
        Item { width: 1; height: 12 }
        Text {
            text: "Header strips"
            color: Theme.textSecondaryStrong
            font.family: Theme.uiFont
            font.pixelSize: 12
            renderType: Text.NativeRendering
        }
        Item { width: 1; height: 5 }
        ProfileTileGrid {
            id: strips
            objectName: "profileStripGrid"
            accessibleName: "Header strips"
            model: ["Flat", "Gradient", "Gloss", "No strip"]
            columns: 4
            columnSpacing: 5
            ringRadius: 4
            currentIndex: tab.draft ? tab.draft.headerStyle : Profile.GlossHeader
            onActivated: index => tab.draft.headerStyle = index
            delegate: Item {
                id: strip
                required property var modelData
                required property int index
                readonly property bool chosen: index === strips.currentIndex
                readonly property color fill: tab.draft ? tab.draft.headerFill : "#9fcdef"
                readonly property var stops: tab.stripStops(index, fill)
                objectName: "profileStripTile_" + index
                width: tab.quarterWidth
                height: 50
                Accessible.role: Accessible.RadioButton
                Accessible.name: modelData
                Accessible.checkable: true
                Accessible.checked: chosen

                Rectangle {
                    width: parent.width
                    height: 34
                    radius: 4
                    color: Theme.fieldBackground
                    border.width: strip.chosen ? 2 : 1
                    border.color: strip.chosen ? Theme.focusBorder : Theme.inputBorder
                    clip: true
                    Rectangle {
                        visible: strip.index !== Profile.NoHeader
                        x: 5
                        y: 6
                        width: parent.width - 10
                        height: 12
                        radius: 2
                        gradient: Gradient {
                            GradientStop { position: 0; color: strip.stops[0] }
                            GradientStop { position: 0.49; color: strip.stops[1] }
                            GradientStop { position: 0.5; color: strip.stops[2] }
                            GradientStop { position: 1; color: strip.stops[3] }
                        }
                        Rectangle {
                            x: 3
                            y: 3
                            width: parent.width * 0.5
                            height: 2
                            radius: 1
                            color: tab.draft ? tab.draft.headerText : "black"
                        }
                    }
                    Rectangle { visible: strip.index === Profile.NoHeader; x: 7; y: 9; width: parent.width * 0.45; height: 2; radius: 1; color: Theme.textPrimary }
                    Rectangle { visible: strip.index === Profile.NoHeader; x: 7; y: 15; width: parent.width - 14; height: 1; color: Theme.inputBorder }
                    Rectangle { x: 7; y: 23; width: parent.width - 18; height: 2; radius: 1; color: Theme.buttonBorder }
                }
                Text {
                    y: 37
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: strip.modelData
                    color: strip.chosen ? Theme.textPrimary : Theme.textSecondaryStrong
                    font.family: Theme.uiFont
                    font.pixelSize: 11
                    renderType: Text.NativeRendering
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: strips.activate(strip.index)
                }
            }
        }
        Item { width: 1; height: 8 }
        Row {
            spacing: 8
            ProfileColorWell {
                id: stripColour
                objectName: "profileStripColourWell"
                width: tab.halfWidth
                label: "Strip colour"
                color: tab.draft ? tab.draft.headerFill : "white"
                editor: tab.editor
                page: tab.draft
                onPicked: colour => tab.draft.headerFill = colour
            }
            ProfileColorWell {
                id: stripText
                objectName: "profileStripTextWell"
                width: tab.halfWidth
                label: "Strip text"
                inkRole: Profile.HeaderTextInk
                color: tab.draft ? tab.draft.headerText : "black"
                editor: tab.editor
                page: tab.draft
                onPicked: colour => tab.draft.headerText = colour
            }
        }
        Item { width: 1; height: 10 }
        Item {
            width: parent.width
            height: 28
            Text {
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - 56
                elide: Text.ElideRight
                text: "Different strip for the right column"
                color: Theme.textPrimary
                font.family: Theme.uiFont
                font.pixelSize: 13
                renderType: Text.NativeRendering
            }
            AeroSwitch {
                objectName: "profileAltHeader"
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                width: 46
                height: 26
                accessibleName: "Different strip for the right column"
                checked: tab.draft !== null && tab.draft.altHeader
                onToggled: checked => tab.draft.altHeader = checked
            }
        }
        Column {
            visible: tab.draft !== null && tab.draft.altHeader
            width: parent.width
            Item { width: 1; height: 6 }
            Row {
                spacing: 8
                ProfileColorWell {
                    objectName: "profileAltStripColourWell"
                    width: tab.halfWidth
                    label: "Right strip"
                    color: tab.draft ? tab.draft.altHeaderFill : "white"
                    editor: tab.editor
                    page: tab.draft
                    onPicked: colour => tab.draft.altHeaderFill = colour
                }
                ProfileColorWell {
                    objectName: "profileAltStripTextWell"
                    width: tab.halfWidth
                    label: "Right strip text"
                    inkRole: Profile.AltHeaderTextInk
                    color: tab.draft ? tab.draft.altHeaderText : "black"
                    editor: tab.editor
                    page: tab.draft
                    onPicked: colour => tab.draft.altHeaderText = colour
                }
            }
            Item { width: 1; height: 8 }
            ProfileColorWell {
                objectName: "profileAltBorderColourWell"
                width: tab.halfWidth
                label: "Right border"
                color: tab.draft ? tab.draft.altBorderColor : "white"
                editor: tab.editor
                page: tab.draft
                onPicked: colour => tab.draft.altBorderColor = colour
            }
        }

        Item { width: 1; height: 12 }
        ProfileReadabilityNotice {
            width: parent.width
            page: tab.draft
            editorTab: Profile.BoxesTab
            onShowMe: role => tab.editor.showMe(role)
        }
    }
}
