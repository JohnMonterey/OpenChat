# Microphone controls

Mute, camera and end/leave call use 30 × 30 icon buttons. Left-click retains the normal
action. Right-click the microphone, or focus it and press Menu / Shift+F10, to
open its context menu. The menu's top-left corner aligns with the microphone
button's left edge, 6 pixels below the button. Submenus cascade sideways and
stay within the window.
The microphone's corner chevron points down when closed and up while its menu
is visible, including while a submenu is open.

The menu shares the `MicrophoneSettings` instance used by the settings page:

- Input device: system default or a detected microphone; applies to the next call.
- Input volume: 0–200%, applied to the live call.
- Mic Enhancement: Studio Voice, noise reduction, automatic gain, compressor,
  noise gate and gate threshold.
- Voice effects: None, Radio, Walkie-Talkie, Telephone, Deep Voice, Tiny Voice,
  Robot, Intercom, Cave, Bathroom, Megaphone and Anonymous.
- Effect intensity: 0–100%; 0% bypasses the selected effect. Enhancement can
  remain enabled alongside an effect. Settings persist across launches.

Studio Voice combines a rumble filter, presence EQ, adaptive spectral noise
reduction, de-essing and compression. Deep/Tiny warp a smoothed vocal spectral
envelope while preserving fundamental pitch. Anonymous additionally shifts
pitch; it is a creative voice disguise, not a guarantee of anonymity.

The processor runs once on 48 kHz mono audio before each recipient's encoding.
Spectral noise reduction and formant/pitch modes add 2048 samples (about 43 ms)
of buffering. Other modes do not buffer the dry path. Buffers have fixed bounds;
processing uses no GPU and no network service. None with enhancement disabled
preserves the existing audio path. The noise gate acts before effects so room
tails can finish naturally. Muting clears delayed audio and skips processing,
preventing speech captured while muted from leaking after unmute.

The standalone DSP regression suite requires only a C++20 compiler:

```sh
g++ -std=c++20 -O2 -Isrc tests/tst_voiceeffects.cpp src/media/VoiceEffects.cpp -o /tmp/openchat-voiceeffects-test
/tmp/openchat-voiceeffects-test
```

It is also registered with CTest as `tst_voiceeffects`. Camera processing,
personas and acoustic echo cancellation are outside this audio-menu change.
