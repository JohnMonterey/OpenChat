import QtQuick
import OpenChat
import OpenChat.Native

// About me (SPEC §14.11, `final-editor-about.png`): your picture (the app's
// avatar, which changes at once: it is not page data), the page's name and
// headline, three short lines, your mood, the two blurbs, and the Interests
// and Details tables folded away until opened ("4 of 6 filled"). Every field
// counts "n / max"; each field's focus period is one undo step; the preview
// marks the box the focused field fills ("Editing").
//
// The name is page-only (ARCH §7.2): saving never changes the name OpenChat
// shows elsewhere, and the field's hint says so.
Item {
    id: tab
    objectName: "profileAboutTab"

    property var profiles: null
    property Item editor: null

    readonly property var draft: profiles ? profiles.draft : null
    readonly property var limits: profiles ? profiles.limits : ({})
    readonly property bool popupOpen: moodMenu.opened || zodiacMenu.opened || hereForMenu.opened
    readonly property string editingTarget: {
        const focused = Window.activeFocusItem;
        for (let at = focused; at; at = at.parent) {
            if (at.editTarget !== undefined && typeof at.editTarget === "string")
                return at.editTarget;
            if (at === tab)
                break;
        }
        return "";
    }
    readonly property string handleHint: profiles && profiles.localHandle.length > 0
                                         ? "Shown on your profile page. Everywhere else you appear as @" + profiles.localHandle + "."
                                         : "Shown on your profile page only. Everywhere else you appear by your OpenChat name."

    function focusField(field) {
        switch (field) {
        case "photo":
            changePicture.forceActiveFocus(Qt.OtherFocusReason);
            break;
        case "headline":
            headline.focusInput();
            break;
        case "info":
            line1.focusInput();
            break;
        case "mood":
            moodButton.forceActiveFocus(Qt.OtherFocusReason);
            break;
        case "aboutMe":
            aboutMe.focusInput();
            break;
        case "meet":
            meet.focusInput();
            break;
        case "interests":
            interests.expanded = true;
            general.focusInput();
            break;
        case "details":
            details.expanded = true;
            hereForButton.forceActiveFocus(Qt.OtherFocusReason);
            break;
        default:
            displayName.focusInput();
            break;
        }
    }

    implicitHeight: column.y + column.implicitHeight + 16

    // A field with its label and counter, editing one text property of the
    // draft. `editTarget` names the preview box it fills.
    component Field: Column {
        id: fieldBlock
        property string label: ""
        property string key: ""
        property int limit: 0
        property bool multiLine: false
        property int minimumHeight: multiLine ? 60 : 32
        property string editTarget: ""
        property string hint: ""
        property bool showLabel: true
        readonly property string value: tab.draft && key.length > 0 ? tab.draft[key] : ""
        function focusInput() {
            input.focusInput();
        }
        width: column.width
        spacing: 4
        ProfileFieldLabel {
            visible: fieldBlock.showLabel
            width: parent.width
            text: fieldBlock.label
            count: fieldBlock.value.length
            limit: fieldBlock.limit
        }
        ProfileTextArea {
            id: input
            objectName: "profileField_" + fieldBlock.key
            width: parent.width
            multiLine: fieldBlock.multiLine
            minimumHeight: fieldBlock.minimumHeight
            maximumLength: fieldBlock.limit
            value: fieldBlock.value
            accessibleName: fieldBlock.label
            profiles: tab.profiles
            gestureKey: "field:" + fieldBlock.key
            onEdited: text => tab.draft[fieldBlock.key] = text
        }
        Text {
            visible: fieldBlock.hint.length > 0
            width: parent.width
            wrapMode: Text.Wrap
            text: fieldBlock.hint
            color: Theme.textSecondaryStrong
            font.family: Theme.uiFont
            font.pixelSize: 12
            renderType: Text.NativeRendering
        }
    }

    // A dropdown button: the value (with an optional leading item) and a chevron.
    component DropButton: Rectangle {
        id: drop
        property string text: ""
        property string accessibleLabel: ""
        property string editTarget: ""
        property int leading: 0
        default property alias leadingItem: leadingSlot.data
        signal opened()
        width: column.width
        height: 30
        radius: 4
        color: dropMouse.containsMouse ? Theme.buttonHover : Theme.buttonBackground
        border.width: activeFocus ? 2 : 1
        border.color: activeFocus ? Theme.focusBorder : Theme.buttonBorder
        activeFocusOnTab: true
        Accessible.role: Accessible.ComboBox
        Accessible.name: accessibleLabel + ", " + text
        Keys.onReturnPressed: opened()
        Keys.onEnterPressed: opened()
        Keys.onSpacePressed: opened()
        Keys.onDownPressed: opened()
        Rectangle { x: 4; y: 1; width: parent.width - 8; height: 1; color: Theme.gloss }
        Item {
            id: leadingSlot
            x: 10
            width: drop.leading
            height: parent.height
        }
        Text {
            x: drop.leading > 0 ? 32 : 10
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - x - 30
            elide: Text.ElideRight
            text: drop.text
            color: Theme.textPrimary
            font.family: Theme.uiFont
            font.pixelSize: 13
            renderType: Text.NativeRendering
        }
        ProfileEditorRail.Glyph {
            anchors.right: parent.right
            anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            width: 14
            height: 14
            kind: "down"
            ink: Theme.categoryChevron
            stroke: 1.6
        }
        MouseArea {
            id: dropMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: drop.opened()
        }
    }

    Column {
        id: column
        x: 16
        y: 14
        width: tab.width - 32

        Text {
            text: "About me"
            color: Theme.categoryText
            font.family: Theme.uiFont
            font.pixelSize: 17
            renderType: Text.NativeRendering
            Accessible.role: Accessible.Heading
            Accessible.name: text
        }
        Item { width: 1; height: 12 }
        Row {
            spacing: 12
            Avatar {
                width: 60
                height: 60
                cornerRadius: 5
                avatarKey: tab.profiles ? tab.profiles.personAvatarKey : "userpfp_none"
            }
            Column {
                anchors.verticalCenter: parent.verticalCenter
                spacing: 6
                ProfileEditorRail.Button {
                    id: changePicture
                    objectName: "profileChangePictureButton"
                    property string editTarget: "photo"
                    glyph: "camera"
                    label: "Change picture…"
                    fontPixelSize: 13
                    enabled: tab.editor !== null && tab.editor.avatarFileDialog !== null
                    onClicked: tab.editor.avatarFileDialog.open()
                }
                Text {
                    width: column.width - 72
                    wrapMode: Text.Wrap
                    text: "Your picture everywhere in OpenChat."
                    color: Theme.textSecondaryStrong
                    font.family: Theme.uiFont
                    font.pixelSize: 12
                    renderType: Text.NativeRendering
                }
            }
        }

        Item { width: 1; height: 14 }
        Field {
            id: displayName
            label: "Name"
            key: "displayName"
            limit: tab.limits.displayName || 48
            editTarget: "name"
            hint: tab.handleHint
        }
        Item { width: 1; height: 10 }
        Field {
            id: headline
            label: "Headline"
            key: "headline"
            limit: tab.limits.headline || 80
            editTarget: "headline"
        }
        Item { width: 1; height: 10 }
        ProfileFieldLabel {
            width: parent.width
            text: "Three short lines (each optional)"
        }
        Item { width: 1; height: 4 }
        Column {
            spacing: 4
            Field {
                id: line1
                label: "First line"
                showLabel: false
                key: "infoLine1"
                limit: tab.limits.infoLine || 40
                editTarget: "info"
            }
            Field {
                label: "Second line"
                showLabel: false
                key: "infoLine2"
                limit: tab.limits.infoLine || 40
                editTarget: "info"
            }
            Field {
                label: "Third line"
                showLabel: false
                key: "infoLine3"
                limit: tab.limits.infoLine || 40
                editTarget: "info"
            }
        }

        Item { width: 1; height: 10 }
        ProfileFieldLabel {
            width: parent.width
            text: "Mood"
        }
        Item { width: 1; height: 4 }
        DropButton {
            id: moodButton
            objectName: "profileMoodButton"
            editTarget: "mood"
            accessibleLabel: "Mood"
            leading: tab.draft && tab.draft.mood > 0 ? 16 : 0
            text: {
                const mood = tab.profiles && tab.draft
                             ? tab.profiles.moodChoices.find(choice => choice.id === tab.draft.mood) : undefined;
                return mood && mood.label.length > 0 ? mood.label : "No mood";
            }
            onOpened: moodMenu.popup(moodButton, 0, moodButton.height + 2)
            ProfileMoodFace {
                visible: tab.draft !== null && tab.draft.mood > 0
                anchors.verticalCenter: parent.verticalCenter
                width: 16
                height: 16
                mood: tab.draft ? tab.draft.mood : 0
            }
        }
        AeroMenu {
            id: moodMenu
            objectName: "profileMoodMenu"
            width: column.width
            height: Math.min(implicitHeight, 320)
            Instantiator {
                model: tab.profiles ? tab.profiles.moodChoices : []
                delegate: AeroMenuItem {
                    required property var modelData
                    objectName: "profileMood_" + modelData.id
                    text: modelData.id === 0 ? "No mood" : modelData.label
                    checkable: true
                    checked: tab.draft !== null && tab.draft.mood === modelData.id
                    onTriggered: tab.draft.mood = modelData.id
                }
                onObjectAdded: (index, object) => moodMenu.insertItem(index, object)
                onObjectRemoved: (index, object) => moodMenu.removeItem(object)
            }
        }

        Item { width: 1; height: 10 }
        Field {
            id: aboutMe
            label: "About me"
            key: "aboutMe"
            multiLine: true
            limit: tab.limits.aboutMe || 2000
            editTarget: "aboutMe"
        }
        Item { width: 1; height: 10 }
        Field {
            id: meet
            label: "Who I'd like to meet"
            key: "meet"
            multiLine: true
            limit: tab.limits.meet || 1000
            editTarget: "meet"
        }

        Item { width: 1; height: 10 }
        ProfileFold {
            id: interests
            objectName: "profileInterestsFold"
            property string editTarget: "interests"
            width: parent.width
            label: "Interests"
            note: (tab.draft ? tab.draft.filledInterestCount : 0) + " of 6 filled"
            Field { id: general; label: "General"; key: "interestGeneral"; multiLine: true; minimumHeight: 44; limit: tab.limits.interest || 300; editTarget: "interests" }
            Field { label: "Music"; key: "interestMusic"; multiLine: true; minimumHeight: 44; limit: tab.limits.interest || 300; editTarget: "interests" }
            Field { label: "Movies"; key: "interestMovies"; multiLine: true; minimumHeight: 44; limit: tab.limits.interest || 300; editTarget: "interests" }
            Field { label: "Television"; key: "interestTelevision"; multiLine: true; minimumHeight: 44; limit: tab.limits.interest || 300; editTarget: "interests" }
            Field { label: "Books"; key: "interestBooks"; multiLine: true; minimumHeight: 44; limit: tab.limits.interest || 300; editTarget: "interests" }
            Field { label: "Heroes"; key: "interestHeroes"; multiLine: true; minimumHeight: 44; limit: tab.limits.interest || 300; editTarget: "interests" }
        }
        Item { width: 1; height: 6 }
        ProfileFold {
            id: details
            objectName: "profileDetailsFold"
            property string editTarget: "details"
            width: parent.width
            label: "Details"
            note: (tab.draft ? tab.draft.filledDetailCount : 0) + " of 6 filled"

            Column {
                width: column.width
                spacing: 4
                ProfileFieldLabel { width: parent.width; text: "Here for" }
                DropButton {
                    id: hereForButton
                    objectName: "profileHereForButton"
                    editTarget: "details"
                    accessibleLabel: "Here for"
                    text: {
                        if (!tab.profiles || !tab.draft || tab.draft.hereFor === 0)
                            return "Choose…";
                        return tab.profiles.hereForChoices.filter(choice => (tab.draft.hereFor & choice.bit) !== 0)
                                                          .map(choice => choice.label).join(", ");
                    }
                    onOpened: hereForMenu.popup(hereForButton, 0, hereForButton.height + 2)
                }
            }
            Field { label: "Hometown"; key: "hometown"; limit: tab.limits.detail || 60; editTarget: "details" }
            Column {
                width: column.width
                spacing: 4
                ProfileFieldLabel { width: parent.width; text: "Zodiac sign" }
                DropButton {
                    id: zodiacButton
                    objectName: "profileZodiacButton"
                    editTarget: "details"
                    accessibleLabel: "Zodiac sign"
                    text: {
                        const sign = tab.profiles && tab.draft
                                     ? tab.profiles.zodiacChoices.find(choice => choice.id === tab.draft.zodiac) : undefined;
                        return sign && sign.name.length > 0 ? sign.name : "Not set";
                    }
                    onOpened: zodiacMenu.popup(zodiacButton, 0, zodiacButton.height + 2)
                }
            }
            Field { label: "Occupation"; key: "occupation"; limit: tab.limits.detail || 60; editTarget: "details" }
            Field { label: "Education"; key: "education"; limit: tab.limits.detail || 60; editTarget: "details" }
            Field { label: "Languages"; key: "languages"; limit: tab.limits.detail || 60; editTarget: "details" }
        }
    }

    AeroMenu {
        id: hereForMenu
        objectName: "profileHereForMenu"
        width: column.width
        Instantiator {
            model: tab.profiles ? tab.profiles.hereForChoices : []
            delegate: AeroMenuItem {
                required property var modelData
                objectName: "profileHereFor_" + modelData.bit
                text: modelData.label
                checkable: true
                checked: tab.draft !== null && (tab.draft.hereFor & modelData.bit) !== 0
                onTriggered: tab.draft.hereFor = tab.draft.hereFor ^ modelData.bit
            }
            onObjectAdded: (index, object) => hereForMenu.insertItem(index, object)
            onObjectRemoved: (index, object) => hereForMenu.removeItem(object)
        }
    }
    AeroMenu {
        id: zodiacMenu
        objectName: "profileZodiacMenu"
        width: column.width
        height: Math.min(implicitHeight, 320)
        Instantiator {
            model: tab.profiles ? tab.profiles.zodiacChoices : []
            delegate: AeroMenuItem {
                required property var modelData
                objectName: "profileZodiac_" + modelData.id
                text: modelData.id === 0 ? "Not set" : modelData.name
                checkable: true
                checked: tab.draft !== null && tab.draft.zodiac === modelData.id
                onTriggered: tab.draft.zodiac = modelData.id
            }
            onObjectAdded: (index, object) => zodiacMenu.insertItem(index, object)
            onObjectRemoved: (index, object) => zodiacMenu.removeItem(object)
        }
    }
}
