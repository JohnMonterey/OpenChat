import QtQuick
import OpenChat
import OpenChat.Native

// The identity card (SPEC §5.1), first in the narrow column, with no strip:
// the owner's styled name on its own line, the photo beside the headline in
// curly quotes and up to three short info lines, then real presence ("Online
// Now!" only when the app says so), and under that the mood with its painted
// face and the sidebar's status line. On your own page the photo is where
// "Change picture" lives: hover or focus shades it with a camera and the
// words, and a click or Enter opens the picture dialog.
ProfileBox {
    id: card
    objectName: "profileIdentityBox"
    property var view: null
    readonly property var page: view ? view.page : null
    readonly property var profiles: view ? view.profiles : null
    readonly property bool preview: view ? view.preview : false
    readonly property bool ownPhoto: profiles !== null && profiles.isOwnProfile && !preview
    readonly property int photoSize: Math.max(96, Math.min(170, Math.round(innerWidth * 0.46)))
    readonly property real photoRadius: Math.min(6, radius + 2)
    readonly property int bodySize: render ? render.bodyPixelSize : 13
    readonly property string bodyFamily: render && render.bodyFamily.length > 0 ? render.bodyFamily : Theme.uiFont
    // The presence the app shows: "Online Now!" only from real presence.
    readonly property int presence: {
        if (!profiles)
            return 2;
        if (profiles.personPresence === 0 && !profiles.personOnline)
            return 2;
        return profiles.personPresence;
    }
    readonly property string moodWord: {
        if (!page || !profiles || page.mood <= 0)
            return "";
        const choices = profiles.moodChoices;
        for (let i = 0; i < choices.length; ++i) {
            if (choices[i].id === page.mood)
                return choices[i].label;
        }
        return "";
    }
    // For the editor's click targets.
    readonly property Item nameItem: nameSlot
    readonly property Item photoItem: photo
    readonly property Item headlineItem: headline
    readonly property Item infoItem: infoLines
    readonly property Item moodItem: moodRow
    readonly property Item statusItem: statusRow

    render: view ? view.render : null
    pad: 12
    Accessible.name: nameText.accessibleName

    component BodyText: Text {
        width: parent ? parent.width : 0
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
        color: card.render ? card.render.bodyColor : "black"
        style: card.render && card.render.textHalo ? Text.Outline : Text.Normal
        styleColor: card.render ? card.render.haloColor : "transparent"
        font.family: card.bodyFamily
        font.pixelSize: card.bodySize
        lineHeight: 1.18
        renderType: Text.NativeRendering
    }
    component LabelText: Text {
        textFormat: Text.PlainText
        color: card.render ? card.render.labelColor : "black"
        style: card.render && card.render.textHalo ? Text.Outline : Text.Normal
        styleColor: card.render ? card.render.haloColor : "transparent"
        font.family: card.render && card.render.labelFamily.length > 0 ? card.render.labelFamily : Theme.uiFont
        font.pixelSize: card.render ? card.render.labelPixelSize : 13
        font.bold: card.render ? card.render.labelBold : true
        renderType: Text.NativeRendering
    }

    Column {
        width: parent.width
        spacing: 10

        // The name's glyphs start at the text edge; glows and outlines bleed
        // into the padding (the painted item reaches past this line box).
        Item {
            id: nameSlot
            width: parent.width
            height: Math.max(1, nameText.implicitHeight - 2 * nameText.glyphTop)
            ProfileNameText {
                id: nameText
                objectName: "profileNameText"
                x: -glyphLeft
                y: -glyphTop
                text: card.page && card.page.displayName.length > 0 ? card.page.displayName
                                                                    : (card.profiles ? card.profiles.personName : "")
                flourish: card.render ? card.render.nameFlourish : 0
                fontFamily: card.render && card.render.nameFamily.length > 0 ? card.render.nameFamily : Theme.uiFont
                basePixelSize: card.render ? card.render.nameBasePixelSize : 28
                minPixelSize: card.render ? card.render.nameMinPixelSize : -1
                color: card.render ? card.render.nameColor : "black"
                color2: card.render ? card.render.nameColor2 : "white"
                effect: card.render ? card.render.nameEffect : 0
                darkBox: card.render ? card.render.boxDark : false
                availableWidth: nameSlot.width
                animate: card.view ? card.view.animate : false
                Accessible.role: Accessible.StaticText
                Accessible.name: nameText.accessibleName
            }
        }

        Item {
            width: parent.width
            height: Math.max(card.photoSize, info.height)

            Item {
                id: photo
                objectName: "profilePhoto"
                width: card.photoSize
                height: card.photoSize
                activeFocusOnTab: card.ownPhoto
                readonly property bool shaded: card.ownPhoto && (photoArea.containsMouse || activeFocus)

                Accessible.role: card.ownPhoto ? Accessible.Button : Accessible.Graphic
                Accessible.name: card.ownPhoto ? "Change picture" : (card.profiles ? card.profiles.personName : "")
                Accessible.onPressAction: if (card.ownPhoto) card.view.requestChangePicture()
                Keys.onPressed: event => {
                    if (card.ownPhoto && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                                          || event.key === Qt.Key_Space)) {
                        card.view.requestChangePicture();
                        event.accepted = true;
                    }
                }

                Avatar {
                    anchors.fill: parent
                    avatarKey: card.profiles ? card.profiles.personAvatarKey : "userpfp_none"
                    cornerRadius: card.photoRadius
                }
                Rectangle {
                    anchors.fill: parent
                    radius: card.photoRadius
                    color: "transparent"
                    border.width: 1
                    border.color: card.render && card.render.boxDark ? "#40ffffff" : "#33000000"
                }
                // Darkening plus a glyph means "change picture", and only here.
                Rectangle {
                    objectName: "profileChangePictureShade"
                    anchors.fill: parent
                    radius: card.photoRadius
                    color: "#8c000000"
                    visible: photo.shaded
                    Column {
                        anchors.centerIn: parent
                        spacing: 6
                        ProfileGlyph {
                            anchors.horizontalCenter: parent.horizontalCenter
                            width: 26
                            height: 26
                            kind: "camera"
                            ink: "white"
                        }
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: "Change picture"
                            textFormat: Text.PlainText
                            color: "white"
                            font.family: Theme.uiFont
                            font.pixelSize: 13
                            font.bold: true
                            renderType: Text.NativeRendering
                        }
                    }
                }
                ProfileFocusRing {
                    anchors.fill: parent
                    shown: photo.activeFocus
                    radius: card.photoRadius
                    onDark: card.render ? card.render.boxDark : false
                }
                MouseArea {
                    id: photoArea
                    anchors.fill: parent
                    enabled: card.ownPhoto
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: card.view.requestChangePicture()
                }
            }

            Column {
                id: info
                x: card.photoSize + 12
                width: parent.width - x
                spacing: 0

                BodyText {
                    id: headline
                    objectName: "profileHeadline"
                    visible: text.length > 0
                    text: card.page && card.page.headline.length > 0 ? "“" + card.page.headline + "”" : ""
                    font.pixelSize: card.bodySize + 1
                    lineHeight: 1.12
                }
                Item {
                    visible: headline.text.length > 0
                    width: 1
                    height: 8
                }
                Column {
                    id: infoLines
                    width: parent.width
                    Repeater {
                        model: card.page ? [card.page.infoLine1, card.page.infoLine2, card.page.infoLine3] : []
                        BodyText {
                            required property string modelData
                            objectName: "profileInfoLine"
                            visible: modelData.length > 0
                            text: modelData
                            lineHeight: 1.22
                            maximumLineCount: 2
                            elide: Text.ElideRight
                        }
                    }
                }
                Item {
                    width: 1
                    height: 9
                }
                Row {
                    id: presenceRow
                    objectName: "profilePresenceRow"
                    spacing: 6
                    PresenceBead {
                        anchors.verticalCenter: parent.verticalCenter
                        beadSize: 12
                        presence: card.presence
                    }
                    Text {
                        objectName: "profilePresenceLabel"
                        anchors.verticalCenter: parent.verticalCenter
                        text: card.presence === 0 ? "Online Now!" : card.presence === 1 ? "Away"
                              : card.presence === 3 ? "Busy" : "Offline"
                        textFormat: Text.PlainText
                        color: card.render && card.render.presenceInks.length > card.presence
                               ? card.render.presenceInks[card.presence] : "#5f6b78"
                        font.family: Theme.uiFont
                        font.pixelSize: card.bodySize + 1
                        font.bold: true
                        renderType: Text.NativeRendering
                    }
                }
            }
        }

        Column {
            // (Not moodRow.visible: a child's visibility includes this one's.)
            visible: card.moodWord.length > 0 || statusText.text.length > 0
            width: parent.width
            spacing: 3

            Row {
                id: moodRow
                objectName: "profileMoodRow"
                visible: card.moodWord.length > 0
                spacing: 5
                LabelText {
                    id: moodLabel
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Mood:"
                }
                // On the label's baseline (the body's line height would
                // otherwise lift it off the line).
                BodyText {
                    anchors.baseline: moodLabel.baseline
                    width: implicitWidth
                    wrapMode: Text.NoWrap
                    lineHeight: 1
                    text: card.moodWord
                }
                ProfileMoodFace {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 15
                    height: 15
                    mood: card.page ? card.page.mood : 0
                    Accessible.ignored: true
                }
            }
            // The sidebar's personal message: live, and not part of the page.
            Item {
                id: statusRow
                objectName: "profileStatusRow"
                visible: statusText.text.length > 0
                width: parent.width
                height: Math.max(statusLabel.implicitHeight, statusText.y + statusText.implicitHeight)
                LabelText {
                    id: statusLabel
                    text: "Status:"
                }
                BodyText {
                    id: statusText
                    anchors.baseline: statusLabel.baseline
                    x: statusLabel.implicitWidth + 5
                    width: parent.width - x
                    text: card.profiles ? card.profiles.personStatusLine : ""
                }
            }
        }
    }
}
