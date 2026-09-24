import QtQuick
import OpenChat
import OpenChat.Native

// The editor's live preview (SPEC §14.4): a 30 px caption strip over the real
// page renderer in preview mode at true scale (never transform-scaled), and
// the try-on pill while a preset is being tried on. The page shown is the
// draft, or the draft with the tried-on preset (`profiles.tryOn`); switching
// between them fades the body out and back in over 60 + 60 ms (ARCH §12),
// instantly when animations are off.
//
// The page is the page kit's ProfilePageView. It is created from its file and
// handed every frozen input it declares (ARCH §8.1: page, profiles, the three
// controllers, songPlayer, mode, editingTarget), and its editRequested(target)
// is passed on, so the editor also loads while the page kit is still the
// scaffold's placeholder.
Item {
    id: frame
    objectName: "profilePreviewFrame"

    property var profiles: null
    property var chatController: null
    property var contactController: null
    property var callController: null
    property var songPlayer: null
    // The preview target whose field has focus in the panel ("Editing").
    property string editingTarget: ""
    signal editRequested(string target)

    readonly property var wantedPage: profiles ? (profiles.tryOnPreset >= 0 ? profiles.tryOn : profiles.draft) : null
    // What the view shows: follows wantedPage through the fade.
    property var shownPage: wantedPage
    readonly property bool animated: ProfileRenderPolicy.animationsAllowed

    // SPEC §3.1 at the preview's own width.
    readonly property int margin: Math.max(16, Math.min(32, 16 + Math.round((width - 720) / 4)))
    readonly property int contentWidth: Math.min(940, width - 2 * margin)
    readonly property bool roomForTwoColumns: contentWidth >= 600
    readonly property bool singleLayout: profiles !== null && profiles.draft.layout === Profile.SingleLayout
    readonly property string caption: roomForTwoColumns || singleLayout
                                      ? "Live preview: what your contacts will see after you save"
                                      : width >= 460 ? "Live preview, one column here: contacts with a wider window see two"
                                                     : "One column here; wider windows show two"
    readonly property Item view: viewLoader.item

    function focusPreview() {
        if (view)
            view.forceActiveFocus(Qt.TabFocusReason);
        else
            viewArea.forceActiveFocus(Qt.TabFocusReason);
    }

    onWantedPageChanged: {
        if (!animated || shownPage === null) {
            fade.stop();
            viewArea.opacity = 1;
            shownPage = wantedPage;
            return;
        }
        fade.restart();
    }

    SequentialAnimation {
        id: fade
        NumberAnimation { target: viewArea; property: "opacity"; to: 0; duration: 60; easing.type: Easing.InOutQuad }
        ScriptAction { script: frame.shownPage = frame.wantedPage }
        NumberAnimation { target: viewArea; property: "opacity"; to: 1; duration: 60; easing.type: Easing.InOutQuad }
    }

    Rectangle {
        id: captionStrip
        objectName: "profilePreviewCaption"
        z: 3
        width: parent.width
        height: 30
        color: Theme.panelBackground
        Rectangle { y: parent.height - 1; width: parent.width; height: 1; color: Theme.rule }

        Row {
            x: 12
            anchors.verticalCenter: parent.verticalCenter
            spacing: 7
            ProfileEditorRail.Glyph {
                anchors.verticalCenter: parent.verticalCenter
                width: 14
                height: 14
                kind: "eye"
                ink: Theme.iconInk
                stroke: 1.3
            }
            Text {
                objectName: "profilePreviewCaptionText"
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(implicitWidth, frame.width - 40 - (hint.visible ? hint.implicitWidth + 24 : 0))
                elide: Text.ElideRight
                text: frame.caption
                color: Theme.textSecondaryStrong
                font.family: Theme.uiFont
                font.pixelSize: 12
                renderType: Text.NativeRendering
            }
        }
        Text {
            id: hint
            objectName: "profilePreviewHint"
            visible: frame.width >= 600
            anchors.right: parent.right
            anchors.rightMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            text: "Click any box to edit it"
            color: Theme.textSecondaryStrong
            font.family: Theme.uiFont
            font.pixelSize: 12
            renderType: Text.NativeRendering
        }
    }

    Item {
        id: viewArea
        objectName: "profilePreviewArea"
        y: captionStrip.height
        width: parent.width
        height: parent.height - captionStrip.height
        clip: true
        activeFocusOnTab: false

        Loader {
            id: viewLoader
            anchors.fill: parent
            source: Qt.resolvedUrl("ProfilePageView.qml")
            onLoaded: frame.bindView(item)
        }
    }

    // Binds the view's frozen inputs (those it declares) to the frame.
    function bindView(item) {
        const inputs = {
            page: () => frame.shownPage,
            profiles: () => frame.profiles,
            chatController: () => frame.chatController,
            contactController: () => frame.contactController,
            callController: () => frame.callController,
            songPlayer: () => frame.songPlayer,
            mode: () => "preview",
            editingTarget: () => frame.editingTarget
        };
        for (const name in inputs) {
            if (name in item)
                item[name] = Qt.binding(inputs[name]);
        }
        if (typeof item.editRequested === "function")
            item.editRequested.connect(frame.editRequested);
    }

    // "Trying on Chrome Y2K. Click to keep it."
    Rectangle {
        id: tryOnPill
        objectName: "profileTryOnPill"
        readonly property int preset: frame.profiles ? frame.profiles.tryOnPreset : -1
        readonly property string presetName: {
            if (!frame.profiles || preset < 0)
                return "";
            const found = frame.profiles.presets.find(entry => entry.id === preset);
            return found ? found.name : "";
        }
        visible: preset >= 0
        z: 4
        anchors.horizontalCenter: parent.horizontalCenter
        y: parent.height - height - 18
        width: pillRow.implicitWidth + 30
        height: 34
        radius: 17
        border.width: 1
        border.color: Theme.tooltipBorder
        gradient: Gradient {
            GradientStop { position: 0; color: Theme.tooltipTop }
            GradientStop { position: 0.48; color: Theme.tooltipMid }
            GradientStop { position: 1; color: Theme.tooltipBottom }
        }
        Accessible.role: Accessible.Button
        Accessible.name: pillText.text
        Accessible.onPressAction: frame.profiles.applyPreset(preset)

        Rectangle {
            z: -1
            y: 2
            width: parent.width
            height: parent.height
            radius: 17
            color: Theme.tooltipShadowFill
        }
        Row {
            id: pillRow
            anchors.centerIn: parent
            spacing: 8
            ProfileEditorRail.Glyph {
                anchors.verticalCenter: parent.verticalCenter
                width: 15
                height: 15
                kind: "palette"
                ink: Theme.categoryText
                stroke: 1.4
            }
            Text {
                id: pillText
                objectName: "profileTryOnPillText"
                anchors.verticalCenter: parent.verticalCenter
                text: "Trying on " + tryOnPill.presetName + ". Click to keep it."
                color: Theme.textPrimary
                font.family: Theme.uiFont
                font.pixelSize: 13
                renderType: Text.NativeRendering
            }
        }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: frame.profiles.applyPreset(tryOnPill.preset)
        }
    }
}
