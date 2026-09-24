import QtQuick
import OpenChat
import OpenChat.Native

// Text (SPEC §14.8, `final-editor-text.png`): the heading face, from a sunken
// list where every row says "<Owner>'s Blurbs" in its own face (Pixel is for
// names only, so it is not listed), the body face (the body-safe four), the
// text size, and the body, label and link colours with their readability
// badges. The readability notice follows when the renderer adjusted any of
// them.
Item {
    id: tab
    objectName: "profileTextTab"

    property var profiles: null
    property Item editor: null

    readonly property var draft: profiles ? profiles.draft : null
    readonly property var fonts: profiles ? profiles.fontChoices : []
    // Faces in the mockups' order: sans, rounded, serif, typewriter, then the
    // display faces.
    readonly property var headingOrder: [Profile.InterfaceFont, Profile.RoundedFont, Profile.SerifFont,
                                         Profile.TypewriterFont, Profile.FutureFont, Profile.MarkerFont,
                                         Profile.ScriptFont, Profile.GothicFont]
    readonly property var headingFonts: headingOrder.map(id => fonts.find(font => font.id === id))
                                                    .filter(font => font !== undefined && font.headingCapable)
    readonly property var bodyFonts: [Profile.InterfaceFont, Profile.RoundedFont, Profile.TypewriterFont,
                                      Profile.SerifFont].map(id => fonts.find(font => font.id === id))
                                                         .filter(font => font !== undefined && font.bodySafe)
    readonly property string owner: profiles ? profiles.personFirstName : ""
    readonly property string sample: owner.length === 0 ? "Blurbs"
                                     : owner + (/s$/i.test(owner) ? "'" : "'s") + " Blurbs"
    readonly property int thirdWidth: Math.floor((width - 32 - 12) / 3)

    function focusField(field) {
        headings.forceActiveFocus(Qt.OtherFocusReason);
    }

    implicitHeight: column.y + column.implicitHeight + 16

    Column {
        id: column
        x: 16
        y: 14
        width: tab.width - 32

        Text {
            text: "Text"
            color: Theme.categoryText
            font.family: Theme.uiFont
            font.pixelSize: 17
            renderType: Text.NativeRendering
            Accessible.role: Accessible.Heading
            Accessible.name: text
        }
        Item { width: 1; height: 10 }
        Text {
            text: "Headings"
            color: Theme.textSecondaryStrong
            font.family: Theme.uiFont
            font.pixelSize: 12
            renderType: Text.NativeRendering
        }
        Item { width: 1; height: 5 }
        // The faces, each in itself, in a sunken list.
        Rectangle {
            width: parent.width
            height: headings.implicitHeight + 2
            radius: 5
            color: Theme.fieldBackground
            border.width: 1
            border.color: Theme.inputBorder
            Rectangle { x: 5; y: 1; width: parent.width - 10; height: 1; color: Theme.insetTop }

            ProfileTileGrid {
                id: headings
                objectName: "profileHeadingList"
                x: 1
                y: 1
                accessibleName: "Heading face"
                model: tab.headingFonts
                columns: 1
                columnSpacing: 0
                rowSpacing: 0
                ringRadius: 3
                currentIndex: {
                    for (let i = 0; i < tab.headingFonts.length; ++i) {
                        if (tab.draft && tab.headingFonts[i].id === tab.draft.headingFont)
                            return i;
                    }
                    return -1;
                }
                onActivated: index => tab.draft.headingFont = tab.headingFonts[index].id
                delegate: Item {
                    id: row
                    required property var modelData
                    required property int index
                    readonly property bool chosen: index === headings.currentIndex
                    readonly property bool bold: modelData.headingBold !== undefined ? modelData.headingBold
                        : modelData.category === "sans" || modelData.category === "monospace"
                    objectName: "profileHeadingFont_" + modelData.id
                    width: column.width - 2
                    height: 30
                    Accessible.role: Accessible.RadioButton
                    Accessible.name: modelData.name
                    Accessible.checkable: true
                    Accessible.checked: chosen

                    Rectangle {
                        visible: row.chosen
                        anchors.fill: parent
                        anchors.margins: 1
                        radius: 3
                        border.width: 1
                        border.color: Theme.focusBorder
                        gradient: Gradient {
                            GradientStop { position: 0; color: Theme.selectedTop }
                            GradientStop { position: 1; color: Theme.selectedBottom }
                        }
                    }
                    Rectangle {
                        visible: row.index > 0 && !row.chosen && row.index !== headings.currentIndex + 1
                        x: 8
                        width: parent.width - 16
                        height: 1
                        color: Theme.softRule
                    }
                    Text {
                        x: 12
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - 100
                        elide: Text.ElideRight
                        text: tab.sample
                        textFormat: Text.PlainText
                        color: Theme.textPrimary
                        font.family: row.modelData.headingFamily.length > 0 ? row.modelData.headingFamily : Theme.uiFont
                        font.pixelSize: Math.round(16 * (row.modelData.headingFactor || 1))
                        font.bold: row.bold
                        renderType: Text.NativeRendering
                    }
                    Text {
                        anchors.right: parent.right
                        anchors.rightMargin: row.chosen ? 30 : 10
                        anchors.verticalCenter: parent.verticalCenter
                        text: row.modelData.name
                        color: Theme.textSecondaryStrong
                        font.family: Theme.uiFont
                        font.pixelSize: 11
                        renderType: Text.NativeRendering
                    }
                    ProfileEditorRail.Glyph {
                        visible: row.chosen
                        anchors.right: parent.right
                        anchors.rightMargin: 9
                        anchors.verticalCenter: parent.verticalCenter
                        width: 14
                        height: 14
                        kind: "check"
                        ink: Theme.focusBorder
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: headings.activate(row.index)
                    }
                }
            }
        }

        Item { width: 1; height: 12 }
        Text {
            text: "Body text"
            color: Theme.textSecondaryStrong
            font.family: Theme.uiFont
            font.pixelSize: 12
            renderType: Text.NativeRendering
        }
        Item { width: 1; height: 5 }
        ProfileSegmented {
            objectName: "profileBodyFont"
            width: parent.width
            accessibleName: "Body text face"
            options: tab.bodyFonts.map(font => font.name)
            fontPixelSize: 11
            currentIndex: {
                for (let i = 0; i < tab.bodyFonts.length; ++i) {
                    if (tab.draft && tab.bodyFonts[i].id === tab.draft.bodyFont)
                        return i;
                }
                return -1;
            }
            onActivated: index => tab.draft.bodyFont = tab.bodyFonts[index].id
        }
        Item { width: 1; height: 10 }
        Text {
            text: "Size"
            color: Theme.textSecondaryStrong
            font.family: Theme.uiFont
            font.pixelSize: 12
            renderType: Text.NativeRendering
        }
        Item { width: 1; height: 5 }
        ProfileSegmented {
            objectName: "profileTextSize"
            width: parent.width
            accessibleName: "Text size"
            options: ["Small", "Normal", "Large"]
            fontPixelSize: 12
            currentIndex: tab.draft ? tab.draft.textSize : 1
            onActivated: index => tab.draft.textSize = index
        }
        Item { width: 1; height: 12 }
        Row {
            spacing: 6
            ProfileColorWell {
                objectName: "profileBodyColourWell"
                width: tab.thirdWidth
                label: "Body"
                pickerTitle: "Body colour"
                inkRole: Profile.BodyInk
                color: tab.draft ? tab.draft.bodyColor : "black"
                editor: tab.editor
                page: tab.draft
                onPicked: colour => tab.draft.bodyColor = colour
            }
            ProfileColorWell {
                objectName: "profileLabelColourWell"
                width: tab.thirdWidth
                label: "Labels"
                pickerTitle: "Label colour"
                inkRole: Profile.LabelInk
                color: tab.draft ? tab.draft.labelColor : "black"
                editor: tab.editor
                page: tab.draft
                onPicked: colour => tab.draft.labelColor = colour
            }
            ProfileColorWell {
                objectName: "profileLinkColourWell"
                width: tab.thirdWidth
                label: "Links"
                pickerTitle: "Link colour"
                inkRole: Profile.LinkInk
                color: tab.draft ? tab.draft.linkColor : "black"
                editor: tab.editor
                page: tab.draft
                onPicked: colour => tab.draft.linkColor = colour
            }
        }
        Item { width: 1; height: 10 }
        ProfileReadabilityNotice {
            width: parent.width
            page: tab.draft
            editorTab: Profile.TextTab
            onShowMe: role => tab.editor.showMe(role)
        }
    }
}
