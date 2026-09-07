# Custom Vocal FX UI

Settings → Audio & Video → Custom Vocal FX opens the dedicated preset editor.
Entering a settings category replaces the sidebar with Back and its subcategories;
the overview on the right provides matching navigation rows. Input and Output
are separate Audio & Video pages. Presets
have ten ordered slots, with bypass and wet/dry controls. They are saved
automatically through Qt Settings under `CustomVocalFX`. The microphone context
menu shares these documents and keeps custom preset selection mutually exclusive
with the existing built-in single effects.

This is the design implementation on main. Built-in single effects still use
`MicrophoneSettings`; custom chain selections are previews until native hosting
is connected. No system scan occurs automatically, and no plugin results or scan
progress are fabricated. The first visit offers Scan / Not now; scanning remains
available from the editor and slot picker afterwards.

## Native integration

Attach a QObject to the `VocalFx.backend` property with the following interface:

| Member | Contract |
| --- | --- |
| `scanning` | Notifying boolean while an explicitly requested scan runs. |
| `scanned` | Notifying boolean indicating a completed plugin inventory. |
| `scanError` | Notifying string; empty on success. |
| `plugins` | Notifying list of `{id, name, format, category}` records. Use stable, namespaced plugin IDs; format is VST or VST3. |
| `scanPlugins()` | Start an asynchronous scan. Persist inventory/scan state in the native service. |
| `applyPreset(preset)` | Apply the complete ordered document, also called after edits to the selected active chain. |
| `clearPreset()` | Remove the custom chain when switching to a built-in single effect. |

A preset contains `{id, name, slots}`. Each slot contains
`{effectId, name, enabled, mix}`; an empty `effectId` means an unused slot,
`builtin:<id>` references `MicrophoneSettings.voiceEffects`, and `mix` ranges from
zero (dry) to one (wet). The picker groups plugin records by category, falling
back to format, and searches across name, format, and category.

Active custom selection is deliberately session-only. Once hosting is connected,
the backend should report activation failures and missing plugins before the UI
claims that a chain is processing audio.
