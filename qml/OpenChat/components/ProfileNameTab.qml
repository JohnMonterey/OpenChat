import QtQuick
import OpenChat
import OpenChat.Native

// Name & FX (SPEC §14.9, `final-editor-name.png`): the owner's name up close
// at 36 px on the page's own box, the nine name faces (each chip says the name
// in its face, Pixel included), the size, the seven effects shown as live "Aa"
// swatches (Glitter sparkles while pointed at), the name colour and a second
// colour named for what the effect does with it (hidden for None), the seven
// flourishes, and the falling sparkle behind the page's boxes.
Item {
    id: tab
    objectName: "profileNameTab"

    property var profiles: null
    property Item editor: null

    readonly property var draft: profiles ? profiles.draft : null
    readonly property var render: draft ? draft.render : null
    readonly property string ownerName: draft && draft.displayName.length > 0 ? draft.displayName
                                        : profiles ? profiles.personName : ""
    readonly property var fonts: profiles ? profiles.fontChoices : []
    readonly property var fontOrder: [Profile.InterfaceFont, Profile.RoundedFont, Profile.SerifFont,
                                      Profile.TypewriterFont, Profile.FutureFont, Profile.MarkerFont,
                                      Profile.ScriptFont, Profile.GothicFont, Profile.PixelFont]
    readonly property var nameFonts: fontOrder.map(id => fonts.find(font => font.id === id))
                                              .filter(font => font !== undefined)
    readonly property var effects: profiles ? profiles.nameEffectChoices : []
    readonly property string colour2Label: {
        const effect = effects.find(choice => draft && choice.id === draft.nameEffect);
        return effect ? effect.colour2Label : "";
    }
    readonly property string nameFamily: render && render.nameFamily.length > 0 ? render.nameFamily : Theme.uiFont
    readonly property int halfWidth: Math.floor((width - 32 - 8) / 2)
    // Everything here edits the name on the identity card.
    readonly property string editingTarget: "name"

    function focusField(field) {
        fontGrid.forceActiveFocus(Qt.OtherFocusReason);
    }

    implicitHeight: column.y + column.implicitHeight + 16

    // What the page's box looks like: its fill over the page's base colour.
    component BoxSurface: Item {
        property real radius: 4
        Rectangle {
            anchors.fill: parent
            radius: parent.radius
            color: tab.render ? tab.render.color1 : "white"
        }
        Rectangle {
            anchors.fill: parent
            radius: parent.radius
            color: tab.render ? tab.render.boxFill : "white"
        }
    }

    Column {
        id: column
        x: 16
        y: 14
        width: tab.width - 32

        Text {
            text: "Name & FX"
            color: Theme.categoryText
            font.family: Theme.uiFont
            font.pixelSize: 17
            renderType: Text.NativeRendering
            Accessible.role: Accessible.Heading
            Accessible.name: text
        }
        Item { width: 1; height: 10 }
        // The name up close, on the page's own box.
        Item {
            width: parent.width
            height: 70
            clip: true
            BoxSurface {
                anchors.fill: parent
                radius: 5
            }
            ProfileNameText {
                id: closeUp
                objectName: "profileNameCloseUp"
                anchors.centerIn: parent
                text: tab.ownerName
                flourish: tab.draft ? tab.draft.nameFlourish : 0
                fontFamily: tab.nameFamily
                basePixelSize: 36
                color: tab.render ? tab.render.nameColor : "black"
                color2: tab.render ? tab.render.nameColor2 : "white"
                effect: tab.render ? tab.render.nameEffect : 0
                darkBox: tab.render !== null && tab.render.boxDark
                availableWidth: parent.width - 30
                animate: ProfileRenderPolicy.animationsAllowed
                Accessible.role: Accessible.StaticText
                Accessible.name: closeUp.accessibleName
            }
            Rectangle {
                anchors.fill: parent
                radius: 5
                color: "transparent"
                border.width: 1
                border.color: Theme.inputBorder
            }
        }

        Item { width: 1; height: 12 }
        Text {
            text: "Font"
            color: Theme.textSecondaryStrong
            font.family: Theme.uiFont
            font.pixelSize: 12
            renderType: Text.NativeRendering
        }
        Item { width: 1; height: 5 }
        ProfileTileGrid {
            id: fontGrid
            objectName: "profileNameFontGrid"
            accessibleName: "Name face"
            model: tab.nameFonts
            columns: 3
            columnSpacing: 5
            rowSpacing: 5
            ringRadius: 4
            currentIndex: {
                for (let i = 0; i < tab.nameFonts.length; ++i) {
                    if (tab.draft && tab.nameFonts[i].id === tab.draft.nameFont)
                        return i;
                }
                return -1;
            }
            onActivated: index => tab.draft.nameFont = tab.nameFonts[index].id
            delegate: Rectangle {
                id: chip
                required property var modelData
                required property int index
                readonly property bool chosen: index === fontGrid.currentIndex
                readonly property bool pixel: modelData.category === "pixel"
                objectName: "profileNameFont_" + modelData.id
                width: Math.floor((column.width - 10) / 3)
                height: 36
                radius: 4
                color: chosen ? Theme.navSelected : chipMouse.containsMouse ? Theme.buttonHover : Theme.fieldBackground
                border.width: chosen ? 2 : 1
                border.color: chosen ? Theme.focusBorder : Theme.inputBorder
                Accessible.role: Accessible.RadioButton
                Accessible.name: modelData.name
                Accessible.checkable: true
                Accessible.checked: chosen
                Text {
                    anchors.centerIn: parent
                    anchors.verticalCenterOffset: chip.modelData.category === "script" ? -2 : 0
                    width: parent.width - 10
                    horizontalAlignment: Text.AlignHCenter
                    fontSizeMode: Text.HorizontalFit
                    minimumPixelSize: 8
                    text: tab.ownerName
                    textFormat: Text.PlainText
                    color: Theme.textPrimary
                    font.family: chip.modelData.nameFamily.length > 0 ? chip.modelData.nameFamily : Theme.uiFont
                    font.pixelSize: chip.pixel ? 12 : Math.round(17 * (chip.modelData.nameFactor || 1))
                    font.bold: chip.modelData.nameBold !== undefined ? chip.modelData.nameBold
                        : chip.modelData.category === "sans" || chip.modelData.category === "monospace"
                    renderType: Text.NativeRendering
                }
                MouseArea {
                    id: chipMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: fontGrid.activate(chip.index)
                }
            }
        }

        Item { width: 1; height: 12 }
        Text {
            text: "Size"
            color: Theme.textSecondaryStrong
            font.family: Theme.uiFont
            font.pixelSize: 12
            renderType: Text.NativeRendering
        }
        Item { width: 1; height: 5 }
        ProfileSegmented {
            objectName: "profileNameSize"
            width: parent.width
            accessibleName: "Name size"
            options: ["Medium", "Large", "Extra large"]
            fontPixelSize: 12
            currentIndex: tab.draft ? tab.draft.nameSize : 0
            onActivated: index => tab.draft.nameSize = index
        }

        Item { width: 1; height: 12 }
        Text {
            text: "Effect"
            color: Theme.textSecondaryStrong
            font.family: Theme.uiFont
            font.pixelSize: 12
            renderType: Text.NativeRendering
        }
        Item { width: 1; height: 5 }
        ProfileTileGrid {
            id: effectGrid
            objectName: "profileNameEffectGrid"
            accessibleName: "Name effect"
            model: tab.effects
            columns: 4
            columnSpacing: 5
            rowSpacing: 5
            ringRadius: 4
            currentIndex: tab.draft ? tab.draft.nameEffect : 0
            onActivated: index => tab.draft.nameEffect = tab.effects[index].id
            delegate: Item {
                id: swatch
                required property var modelData
                required property int index
                readonly property bool chosen: index === effectGrid.currentIndex
                objectName: "profileNameEffect_" + modelData.id
                width: Math.floor((column.width - 15) / 4)
                height: 52
                clip: true
                Accessible.role: Accessible.RadioButton
                Accessible.name: modelData.name
                Accessible.checkable: true
                Accessible.checked: chosen
                BoxSurface {
                    anchors.fill: parent
                }
                ProfileNameText {
                    id: sample
                    anchors.horizontalCenter: parent.horizontalCenter
                    y: 20 - height / 2
                    // Fancy effects never render a name below 30 px; the
                    // swatches all show "Aa" at 22 so they compare.
                    scale: sample.renderedPixelSize > 22 ? 22 / sample.renderedPixelSize : 1
                    text: "Aa"
                    fontFamily: tab.nameFamily
                    basePixelSize: 22
                    minPixelSize: 22
                    color: tab.render ? tab.render.nameColor : "black"
                    color2: tab.render ? tab.render.nameColor2 : "white"
                    effect: swatch.modelData.id
                    darkBox: tab.render !== null && tab.render.boxDark
                    // Glitter shows its sparkle while pointed at.
                    animate: effectMouse.containsMouse && ProfileRenderPolicy.animationsAllowed
                }
                Rectangle {
                    anchors.bottom: parent.bottom
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.margins: swatch.chosen ? 2 : 1
                    height: 16
                    color: swatch.chosen ? Theme.focusBorder : "#99000000"
                    Text {
                        anchors.centerIn: parent
                        text: swatch.modelData.name
                        color: "white"
                        font.family: Theme.uiFont
                        font.pixelSize: 10
                        renderType: Text.NativeRendering
                    }
                }
                Rectangle {
                    anchors.fill: parent
                    radius: 4
                    color: "transparent"
                    border.width: swatch.chosen ? 2 : 1
                    border.color: swatch.chosen ? Theme.focusBorder : Theme.inputBorder
                }
                MouseArea {
                    id: effectMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: effectGrid.activate(swatch.index)
                }
            }
        }

        Item { width: 1; height: 12 }
        Row {
            spacing: 8
            ProfileColorWell {
                objectName: "profileNameColourWell"
                width: tab.halfWidth
                label: "Name colour"
                inkRole: Profile.NameInk
                color: tab.draft ? tab.draft.nameColor : "black"
                editor: tab.editor
                page: tab.draft
                onPicked: colour => tab.draft.nameColor = colour
            }
            ProfileColorWell {
                objectName: "profileNameColour2Well"
                visible: tab.colour2Label.length > 0
                width: tab.halfWidth
                label: tab.colour2Label
                color: tab.draft ? tab.draft.nameColor2 : "white"
                editor: tab.editor
                page: tab.draft
                onPicked: colour => tab.draft.nameColor2 = colour
            }
        }

        Item { width: 1; height: 12 }
        Text {
            text: "Flourish"
            color: Theme.textSecondaryStrong
            font.family: Theme.uiFont
            font.pixelSize: 12
            renderType: Text.NativeRendering
        }
        Item { width: 1; height: 5 }
        ProfileTileGrid {
            id: flourishGrid
            objectName: "profileFlourishGrid"
            accessibleName: "Flourish"
            model: tab.profiles ? tab.profiles.flourishChoices : []
            columns: 7
            columnSpacing: 4
            ringRadius: 4
            currentIndex: tab.draft ? tab.draft.nameFlourish : 0
            onActivated: index => tab.draft.nameFlourish = model[index].id
            delegate: Rectangle {
                id: flourish
                required property var modelData
                required property int index
                readonly property bool chosen: index === flourishGrid.currentIndex
                objectName: "profileFlourish_" + modelData.id
                width: Math.floor((column.width - 24) / 7)
                height: 28
                radius: 4
                color: chosen ? Theme.navSelected : flourishMouse.containsMouse ? Theme.buttonHover : Theme.buttonBackground
                border.width: chosen ? 2 : 1
                border.color: chosen ? Theme.focusBorder : Theme.buttonBorder
                Accessible.role: Accessible.RadioButton
                Accessible.name: modelData.id === 0 ? "No flourish" : modelData.name
                Accessible.checkable: true
                Accessible.checked: chosen
                Text {
                    anchors.centerIn: parent
                    text: flourish.modelData.name
                    color: Theme.buttonText
                    font.family: Theme.uiFont
                    font.pixelSize: 13
                    renderType: Text.NativeRendering
                }
                MouseArea {
                    id: flourishMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: flourishGrid.activate(flourish.index)
                }
            }
        }

        Item { width: 1; height: 12 }
        Text {
            text: "Falling sparkle on your page"
            color: Theme.textSecondaryStrong
            font.family: Theme.uiFont
            font.pixelSize: 12
            renderType: Text.NativeRendering
        }
        Item { width: 1; height: 5 }
        ProfileSegmented {
            objectName: "profileAmbient"
            width: parent.width
            accessibleName: "Falling sparkle"
            options: tab.profiles ? tab.profiles.ambientChoices.map(choice => choice.name) : []
            fontPixelSize: 12
            currentIndex: tab.draft ? tab.draft.ambient : 0
            onActivated: index => tab.draft.ambient = index
        }
        Item { width: 1; height: 6 }
        Text {
            width: parent.width
            wrapMode: Text.Wrap
            text: "Falls behind your boxes, never over text. Still for anyone using Low memory mode or reduced motion."
            color: Theme.textSecondaryStrong
            font.family: Theme.uiFont
            font.pixelSize: 12
            renderType: Text.NativeRendering
        }
        Item { width: 1; height: 10 }
        ProfileReadabilityNotice {
            width: parent.width
            page: tab.draft
            editorTab: Profile.NameTab
            onShowMe: role => tab.editor.showMe(role)
        }
    }
}
