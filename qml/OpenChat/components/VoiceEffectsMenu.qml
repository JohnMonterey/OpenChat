import QtQuick
import QtQuick.Controls.Basic
import QtQml.Models
import OpenChat
import OpenChat.Native

AeroMenu {
    id: menu
    objectName: "voiceEffectsMenu"
    title: "Voice Effects"
    width: 290
    // Snapshot the unfiltered size once per opening. Results can shrink without
    // moving the popup (and its search field) to a different screen position.
    property real openingHeight: 580
    height: openingHeight
    readonly property string query: search.text.trim().toLowerCase()
    readonly property var matchingPresets: VocalFx.presets.filter(p => p.name.toLowerCase().includes(query))
    readonly property var matchingBuiltins: MicrophoneSettings.voiceEffects.filter(e => e.name.toLowerCase().includes(query))
    onAboutToShow: {
        search.text = "";
        results.positionViewAtBeginning();
        results.forceLayout();
        const available = Overlay.overlay ? Overlay.overlay.height - 2 * margins : 600;
        openingHeight = Math.min(results.contentHeight + 44 + topPadding + bottomPadding, available);
    }

    // Keep the input outside the scrolling menu model. Filtering only hides
    // stable menu items, so it never removes/reinserts the focused input or
    // changes menu indices underneath the text editor.
    contentItem: Item {
        implicitWidth: 280
        AeroTextField {
            id: search
            x: 4; y: 4
            width: parent.width - 8
            height: 32
            fontPixelSize: 13
            placeholder: "Search voice effects"
        }
        ListView {
            id: results
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.topMargin: 44
            anchors.bottom: parent.bottom
            model: menu.contentModel
            currentIndex: menu.currentIndex
            clip: true
            interactive: contentHeight > height
            boundsBehavior: Flickable.StopAtBounds
            ScrollIndicator.vertical: ScrollIndicator {}
        }
    }
    AeroMenuItem { text: "Custom Vocal FX"; enabled: false }
    MenuSeparator { contentItem: Rectangle { implicitHeight: 1; color: Theme.rule } }
    Instantiator {
        model: VocalFx.presets
        delegate: AeroMenuSetting {
            settingsMenu: menu
            required property var modelData
            text: modelData.name
            visible: modelData.name.toLowerCase().includes(menu.query)
            height: visible ? implicitHeight : 0
            selectionOnly: true
            checked: VocalFx.activePresetId === modelData.id
            onTriggered: VocalFx.activatePreset(modelData.id)
        }
        onObjectAdded: (index, object) => menu.insertItem(index + 2, object)
        onObjectRemoved: (index, object) => menu.removeItem(object)
    }
    AeroMenuItem {
        text: VocalFx.presets.length ? "No matching presets" : "Create presets in Audio & Video"
        enabled: false
        visible: menu.matchingPresets.length === 0
        height: visible ? implicitHeight : 0
    }
    MenuSeparator { contentItem: Rectangle { implicitHeight: 1; color: Theme.rule } }
    AeroMenuItem { text: "Built-in FX"; enabled: false }
    MenuSeparator { contentItem: Rectangle { implicitHeight: 1; color: Theme.rule } }
    Instantiator {
        model: MicrophoneSettings.voiceEffects
        delegate: AeroMenuSetting {
            settingsMenu: menu
            required property var modelData
            objectName: "voiceEffect_" + modelData.id
            text: modelData.name
            visible: modelData.name.toLowerCase().includes(menu.query)
            height: visible ? implicitHeight : 0
            selectionOnly: true
            checked: VocalFx.activePresetId === "" && MicrophoneSettings.voiceEffect === modelData.id
            onTriggered: VocalFx.selectBuiltin(modelData.id)
        }
        onObjectAdded: (index, object) => menu.insertItem(index + 6 + VocalFx.presets.length, object)
        onObjectRemoved: (index, object) => menu.removeItem(object)
    }
    AeroMenuItem {
        text: "No matching built-in effects"
        enabled: false
        visible: menu.matchingBuiltins.length === 0
        height: visible ? implicitHeight : 0
    }
    Item {
        implicitHeight: visible ? previewLabel.implicitHeight + 16 : 0
        visible: VocalFx.activePresetId !== "" && !VocalFx.backend
        width: parent ? parent.width : 280
        Text {
            id: previewLabel
            x: 10; y: 8
            width: parent.width - 20
            text: "Preset preview only. Chain playback is not available in this build."
            wrapMode: Text.WordWrap
            color: Theme.textSecondary
            font.family: Theme.uiFont
            font.pixelSize: 12
        }
    }
}
