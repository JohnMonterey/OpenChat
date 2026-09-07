import QtQuick
import QtQuick.Controls.Basic
import QtQml.Models
import OpenChat

Popup {
    id: picker
    objectName: "vocalFxPicker"
    property int slotIndex: -1
    property string presetId: ""
    property string category: "All categories"
    readonly property var categories: ["All categories"].concat(
        Array.from(new Set(VocalFx.library.map(e => e.category || e.format))).sort())
    readonly property var results: {
        const query = search.text.trim().toLowerCase();
        const effects = VocalFx.library.filter(e =>
            (category === "All categories" || (e.category || e.format) === category)
            && (e.name + " " + e.format + " " + (e.category || "")).toLowerCase().includes(query));
        effects.sort((a, b) => (a.category || a.format).localeCompare(b.category || b.format)
                              || a.name.localeCompare(b.name));
        let rows = [], previous = "";
        for (const effect of effects) {
            const group = effect.category || effect.format;
            if (group !== previous) rows.push({header: true, name: group});
            rows.push(Object.assign({header: false}, effect));
            previous = group;
        }
        return rows;
    }
    parent: Overlay.overlay
    width: 350
    height: 560
    padding: 8
    margins: 8
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    function openFor(item, index) {
        // Keep geometry fixed for this opening, just like the call FX menu.
        width = Math.min(350, parent.width - 16);
        height = Math.min(560, parent.height - 16);
        slotIndex = index;
        presetId = VocalFx.selectedPresetId;
        category = "All categories";
        search.text = "";
        const point = item.mapToItem(parent, 0, item.height + 4);
        x = Math.max(8, Math.min(point.x, parent.width - width - 8));
        y = Math.max(8, Math.min(point.y, parent.height - height - 8));
        open();
    }
    onOpened: search.focusInput()
    background: Rectangle {
        color: Theme.contentBackground
        radius: 5
        border.color: Theme.inputBorder
    }
    contentItem: Column {
        id: body
        spacing: 8
        Text {
            text: "FX Slot " + (picker.slotIndex + 1)
            color: Theme.textPrimary
            font.family: Theme.uiFont
            font.pixelSize: 14
        }
        AeroTextField {
            id: search
            width: parent.width
            height: 32
            fontPixelSize: 13
            placeholder: "Search effects and plugins"
            Keys.onDownPressed: { resultList.forceActiveFocus(); resultList.currentIndex = 0; }
        }
        AeroButton {
            width: parent.width
            height: 28
            fontPixelSize: 12
            label: picker.category + "  ▾"
            onClicked: categoriesMenu.popup(this, 0, height)
            AeroMenu {
                id: categoriesMenu
                width: picker.availableWidth
                Instantiator {
                    model: picker.categories
                    delegate: AeroMenuItem {
                        required property string modelData
                        text: modelData
                        checkable: true
                        checked: picker.category === modelData
                        onTriggered: picker.category = modelData
                    }
                    onObjectAdded: (index, object) => categoriesMenu.insertItem(index, object)
                    onObjectRemoved: (index, object) => categoriesMenu.removeItem(object)
                }
            }
        }
        ListView {
            id: resultList
            width: parent.width
            height: Math.max(64, Math.min(270, picker.availableHeight - 192 - scanMessage.height))
            clip: true
            model: picker.results
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            delegate: AeroMenuItem {
                required property var modelData
                width: resultList.width - 12
                height: modelData.header ? 27 : 32
                text: modelData.name + (modelData.header || modelData.format === "Built-in" ? "" : "  ·  " + modelData.format)
                enabled: !modelData.header
                leftPadding: modelData.header ? 8 : 16
                onTriggered: {
                    if (VocalFx.selectedPresetId === picker.presetId)
                        VocalFx.updateSlot(picker.slotIndex, {effectId: modelData.id, name: modelData.name, enabled: true});
                    picker.close();
                }
            }
            Text {
                anchors.centerIn: parent
                visible: picker.results.length === 0
                text: "No matching effects"
                color: Theme.textSecondary
                font.family: Theme.uiFont
                font.pixelSize: 13
            }
        }
        Rectangle { width: parent.width; height: 1; color: Theme.rule }
        Text {
            id: scanMessage
            width: parent.width
            text: VocalFx.scanError || (VocalFx.backend && VocalFx.backend.scanError)
                  || (VocalFx.scanning ? "Scanning system plugins…"
                      : VocalFx.scanned ? VocalFx.plugins.length + " system plugins found."
                      : "Include your system plugins? Scan to find installed VST / VST3 effects.")
            wrapMode: Text.WordWrap
            color: Theme.textSecondary
            font.family: Theme.uiFont
            font.pixelSize: 12
        }
        AeroButton {
            width: parent.width
            height: 30
            fontPixelSize: 13
            enabled: !VocalFx.scanning
            label: VocalFx.scanning ? "Scanning…" : VocalFx.scanned ? "Scan again" : "Scan system plugins"
            onClicked: VocalFx.scanPlugins()
        }
        AeroMenuItem {
            width: parent.width
            height: 28
            text: "Clear slot"
            onTriggered: {
                if (VocalFx.selectedPresetId === picker.presetId)
                    VocalFx.updateSlot(picker.slotIndex, VocalFx.emptySlot());
                picker.close();
            }
        }
    }
}
