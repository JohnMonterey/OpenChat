pragma Singleton

import QtQuick
import QtCore
import OpenChat.Native

// UI-owned preset documents. Native hosting can attach a backend without
// changing the editor or importing the VST branch into this design pass.
QtObject {
    id: fx
    readonly property int slotCount: 10
    property var backend: null
    property var presets: []
    property string selectedPresetId: ""
    property string activePresetId: ""
    property string scanError: ""
    readonly property bool scanning: backend ? backend.scanning : false
    readonly property bool scanned: backend ? backend.scanned : false
    readonly property var plugins: scanned && backend ? backend.plugins : []
    readonly property var selectedPreset: presets.find(p => p.id === selectedPresetId) || null
    readonly property var builtins: MicrophoneSettings.voiceEffects.filter(e => e.id !== "none").map(e => ({
        id: "builtin:" + e.id, name: e.name, format: "Built-in", category: builtinCategory(e.id)
    }))
    readonly property var library: builtins.concat(plugins)
    property Settings storage: Settings {
        category: "CustomVocalFX"
        property string documents: "[]"
        property string selectedId: ""
        property bool scanPromptDismissed: false
    }
    property Connections microphoneConnection: Connections {
        target: MicrophoneSettings
        function onProcessingChanged() {
            if (MicrophoneSettings.voiceEffect !== "none") fx.activePresetId = "";
        }
    }

    Component.onCompleted: {
        try {
            const saved = JSON.parse(storage.documents);
            if (Array.isArray(saved)) {
                presets = saved.filter(p => p && typeof p.id === "string" && typeof p.name === "string"
                                       && Array.isArray(p.slots) && p.slots.length === slotCount
                                       && p.slots.every(s => s && typeof s.effectId === "string"
                                                       && typeof s.name === "string"
                                                       && typeof s.enabled === "boolean"
                                                       && typeof s.mix === "number" && s.mix >= 0 && s.mix <= 1));
            }
        } catch (error) { presets = []; }
        selectedPresetId = presets.some(p => p.id === storage.selectedId)
                         ? storage.selectedId : (presets.length ? presets[0].id : "");
    }
    function builtinCategory(id) {
        if (["deep", "tiny", "anonymous"].includes(id)) return "Pitch & character";
        if (["cave", "bathroom"].includes(id)) return "Space & ambience";
        return "Radio & modulation";
    }
    function persist() {
        storage.documents = JSON.stringify(presets);
        storage.selectedId = selectedPresetId;
    }
    function emptySlot() { return { effectId: "", name: "", enabled: true, mix: 1 }; }
    function selectPreset(id) { selectedPresetId = id; storage.selectedId = id; }
    function createPreset() {
        let n = 1;
        while (presets.some(p => p.name === "My Preset " + n)) ++n;
        const preset = { id: "preset-" + Date.now() + "-" + Math.random().toString(36).slice(2, 8),
                         name: "My Preset " + n, slots: Array.from({length: slotCount}, () => emptySlot()) };
        presets = presets.concat([preset]);
        selectedPresetId = preset.id;
        persist();
    }
    function renamePreset(name) {
        name = name.trim().slice(0, 80);
        if (!selectedPreset || !name) return;
        presets = presets.map(p => p.id === selectedPresetId ? Object.assign({}, p, {name: name}) : p);
        persist();
    }
    function duplicatePreset() {
        if (!selectedPreset) return;
        const copy = JSON.parse(JSON.stringify(selectedPreset));
        copy.id = "preset-" + Date.now() + "-" + Math.random().toString(36).slice(2, 8);
        const base = copy.name + " copy";
        copy.name = base;
        let n = 2;
        while (presets.some(p => p.name === copy.name)) copy.name = base + " " + n++;
        presets = presets.concat([copy]);
        selectedPresetId = copy.id;
        persist();
    }
    function deletePreset() {
        if (activePresetId === selectedPresetId) selectBuiltin("none");
        presets = presets.filter(p => p.id !== selectedPresetId);
        selectedPresetId = presets.length ? presets[0].id : "";
        persist();
    }
    function updateSlot(index, changes) {
        if (!selectedPreset || index < 0 || index >= slotCount) return;
        const slots = selectedPreset.slots.map((s, i) => i === index ? Object.assign({}, s, changes) : s);
        presets = presets.map(p => p.id === selectedPresetId ? Object.assign({}, p, {slots: slots}) : p);
        persist();
        if (backend && activePresetId === selectedPresetId) backend.applyPreset(selectedPreset);
    }
    function moveSlot(index, offset) {
        if (!selectedPreset || index + offset < 0 || index + offset >= slotCount) return;
        const slots = selectedPreset.slots.slice();
        const other = slots[index + offset];
        slots[index + offset] = slots[index];
        slots[index] = other;
        presets = presets.map(p => p.id === selectedPresetId ? Object.assign({}, p, {slots: slots}) : p);
        persist();
        if (backend && activePresetId === selectedPresetId) backend.applyPreset(selectedPreset);
    }
    function selectBuiltin(id) {
        if (backend) backend.clearPreset();
        activePresetId = "";
        MicrophoneSettings.voiceEffect = id;
    }
    function activatePreset(id) {
        const preset = presets.find(p => p.id === id);
        if (!preset) return;
        MicrophoneSettings.voiceEffect = "none";
        activePresetId = id;
        if (backend) backend.applyPreset(preset);
    }
    function scanPlugins() {
        storage.scanPromptDismissed = true;
        scanError = "";
        if (backend) backend.scanPlugins();
        else scanError = "Plugin scanning is not available in this build. You can still design chains with built-in FX.";
    }
}
