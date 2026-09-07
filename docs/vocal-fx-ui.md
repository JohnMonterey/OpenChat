# Custom Vocal FX UI

Settings → Audio & Video → Custom Vocal FX opens the dedicated preset editor.
Entering a settings category replaces the sidebar with Back and its subcategories;
the overview on the right provides matching navigation rows. Input and Output
are separate Audio & Video pages. Presets
have ten ordered slots, with bypass and wet/dry controls. They are saved
automatically through Qt Settings under `CustomVocalFX`. The microphone context
menu shares these documents and keeps custom preset selection mutually exclusive
with the existing built-in single effects.

Built-in single effects still use `MicrophoneSettings`. Custom chains are hosted
natively by `VoiceEffectHost` (`src/app/VoiceEffectHost.h`), which is attached to
`VocalFx.backend` by default, so a selected chain of plugins processes the
outgoing voice on the next call. No system scan occurs automatically, and no
plugin results or scan progress are fabricated. The first visit offers
Scan / Not now; scanning remains available from the editor and slot picker
afterwards.

Two things a preset can express are deliberately not hosted yet, and the editor
should not be read as claiming otherwise:

- A `builtin:` slot inside a custom chain does nothing. Built-in effects live in
  `openchat_media`, the plugin chain lives in `openchat_effects`, and the two are
  kept apart on purpose (see the `openchat_effects` comment in CMakeLists.txt).
  Built-in effects remain available as the single-effect selection.
- A chain applied while a call is already running takes effect on the next call.
  Rebuilding a chain mid-conversation means activating plugins on the frame path.

## Native integration

`VoiceEffectHost` implements this interface; it is documented here because the
editor depends on the contract rather than on that class. Attach any QObject to
the `VocalFx.backend` property with the following interface:

| Member | Contract |
| --- | --- |
| `scanning` | Notifying boolean while an explicitly requested scan runs. |
| `scanned` | Notifying boolean indicating a completed plugin inventory. |
| `scanError` | Notifying string; empty on success. |
| `plugins` | Notifying list of `{id, name, format, category}` records. IDs are `AudioPluginId::toString()` — `vst3:/path/Bundle.vst3#<class UID>` — so a slot names one plugin inside one file rather than an index that shifts. `format` is `VST3` or `CLAP`; `category` may be empty, and the picker then groups by format. |
| `scanPlugins()` | Start an asynchronous scan. Persist inventory/scan state in the native service. |
| `applyPreset(preset)` | Apply the complete ordered document, also called after edits to the selected active chain. |
| `clearPreset()` | Remove the custom chain when switching to a built-in single effect. |

A preset contains `{id, name, slots}`. Each slot contains
`{effectId, name, enabled, mix}`; an empty `effectId` means an unused slot,
`builtin:<id>` references `MicrophoneSettings.voiceEffects`, and `mix` ranges from
zero (dry) to one (wet). The picker groups plugin records by category, falling
back to format, and searches across name, format, and category.

Active custom selection is deliberately session-only.

`VoiceEffectHost` reports activation failures and missing plugins rather than
letting the UI claim a chain is processing audio: `applyPreset` builds the chain
once as a rehearsal and publishes `chainError` and `activeStageCount` from what
actually loaded.

## Scanning, consent and what runs when

Scanning is not loading for a call, but it is still running somebody's code:
`PluginScanner::describe` opens each bundle to ask what it publishes. A scan is
therefore gated on the path checks — nobody but this user and root may be able
to rewrite the file — and runs off the GUI thread, since opening every plugin on
a machine with a DAW installed takes seconds.

Consent is recorded against the file's bytes at the moment a plugin is put into
a chain and switched on, which is the point where the user has actually named
it. Scanning alone records nothing, and a plugin whose bytes changed since it
was approved is refused until it is chosen again. The inventory and the consents
are persisted by the host (`QSettings`, group `VoiceEffectHost`), so a scan is
asked for once rather than on every launch.

A plugin runs in this process and can read everything this process holds; see
the SECURITY section of `src/effects/VoiceEffect.h`.
