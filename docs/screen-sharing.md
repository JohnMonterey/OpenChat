# Screen sharing in calls

**Share screen**, next to **Camera on** in the call controls, offers every
connected display and every capturable window. Picking one starts the share;
dismissing the picker starts nothing. The button reads **Stop sharing** while a
share is running and is greyed with an explanation on a machine whose platform
has no capture at all. Permission denial, a closed window, an unplugged monitor
or a capture failure ends the share, restores the button, releases every buffer,
and reports the reason under the controls — the voice call and the camera are
untouched by any of it.

A screen is a second video source beside the camera, not a replacement for one:
microphone, camera and screen all run at once, and the receiving end can tell
the two pictures apart because they are separate streams with separate keys. An
incoming share appears by itself on a stage below the participants, in the same
`CallVideoItem` the camera tiles use, and disappears the moment it stops. In a
group, one member's share holds the stage until they stop; the next takes it.

A share carries the computer's sound too, unless **Share sound** in the picker is
switched off (remembered between shares). See [Sound](#sound).

## A video stream, not re-sent squares

Screen sharing is a VP9 video stream (`src/call/ScreenVideoCodec`), in libvpx's
real-time screen-content mode, encoded and decoded off the GUI thread. It
replaced the tile encoder described further down as the default, because the
tile design cannot follow anything that moves.

A scrolled page or a playing video changes every tile on every frame, and the
tile budget covers a handful of them per frame. So the viewer sees the picture
fill in over a second or more, always behind. A video codec encodes motion as
motion: a scroll is a few motion vectors rather than a hundred re-sent squares.
`openchat-screen-bench` measures what the viewer actually sees, as PSNR of their
picture against the sharer's screen at that moment (above ~35 dB is current):

| 1080p, 30 fps | tiles, starting rung | tiles, best rung | VP9, starting rate |
|---|---|---|---|
| scrolling a page | 12.3 dB at 3.3 Mbit/s | 12.4 dB at 8.1 Mbit/s | **35.3 dB at 2.9 Mbit/s** |
| a video playing | 17.4 dB at 3.4 Mbit/s | 18.0 dB at 8.4 Mbit/s | **36.4 dB at 2.9 Mbit/s** |
| typing | 32.3 dB at 1.9 Mbit/s | 41.8 dB at 3.2 Mbit/s | **36.8 dB at 1.0 Mbit/s** |

VP9 encodes a 1080p frame in about 5 ms on four threads of a desktop CPU.
`OpenChat --screen-share-check` reports the time on the machine it runs on, and
warns when that is longer than a frame.

A slower machine adapts on its own. The encoder thread keeps a smoothed
measure of encoding time as a share of the time between frames (keyframes
left out). Above 70% it moves libvpx from speed 8 to speed 9, which costs a
little sharpness and saves about a third of the time. If it is still above 85%
at speed 9, the encoder holds itself one rung lower (fewer pixels and frames)
than the link allows. It tries a rung higher again after twenty calm seconds.
The diagnostics line shows the speed, the load, and `cpu` when the rung is
held down this way.

**Sending.**

- The capture's frame is converted to 4:2:0 (full-range BT.709, on up to four
  threads) and handed to the encoder thread. A frame still waiting for a busy
  encoder is replaced by the newer one, never queued behind it.
- Encoding is constant-bitrate with no look-ahead. Keyframes are sent only at
  the start, on a new size, or when a viewer asks for one, and are capped at
  three frames' worth: they arrive in well under a second, slightly soft, and
  the following frames sharpen them.
- A still screen keeps being encoded for three seconds after its last change,
  while the codec sharpens it, then once a second as a heartbeat. After that it
  costs a few hundred bytes a second.
- Each encoded frame is sealed per peer as fragments of at most 24 KiB.

**The pacer.** The relay connection is one TCP socket shared with the call's
voice.

- `RelayClient` silently drops any media datagram offered while 128 KB is
  already waiting on that socket. "Waiting" includes the TLS socket's encrypted
  buffer. Over `wss://` a write is encrypted at once and then waits there, and
  `QWebSocket::bytesToWrite()` does not count it: measured against a stalled
  relay, it read 0 with 23 MB queued. Both this gate and the pacer below read
  `RelayClient::pendingSendBytes()`, which adds `encryptedBytesToWrite()`. Under the old design that meant a share
  outrunning the uplink lost packets, including voice. Every gap made the viewer
  ask for everything again, which flooded the link further.
- Now screen packets wait in the engine's own queue (`CallEngine`'s pacer).
  They reach the socket only while less than 32 KB is waiting there, so a voice
  packet, which bypasses the pacer, never sits behind more than that plus one
  fragment.
- A new frame is encoded only while the queue holds less than 150 ms of the
  stream per peer (in a group call each frame is queued once per member). When
  the link is the bottleneck the share loses frames, not timeliness.
- When the queue has been backed up for most of a second, the rate it actually
  drained becomes a cap on the encoder's bitrate. The cap is raised again after
  three calm seconds.

**Congestion further along** (the relay, or the viewer's downlink) shows up as
delay, not loss, since everything is TCP. The viewer's report now also says how
long it held the newest packet before reporting, so the sender measures a true
round trip. More than 350 ms above the lowest of the last half minute costs a
rung; staying within 120 ms of it for three seconds of saturated sending earns
one back.

**Receiving.** Fragments are reassembled on the GUI thread, which is cheap. A
missing or out-of-order fragment breaks the stream: the decoder waits for a
keyframe and the report asking for one goes out at once, not with the next
periodic report.

- A loss breaks the stream once. The rest of that frame is dropped quietly,
  and a new frame starts only with its first fragment.
- A request that goes unanswered (the report, or the keyframe answering it, was
  dropped on the way) is asked again after 1.5 s if frames keep arriving that
  cannot be decoded. Without that, the share would stay frozen while still
  looking alive.
- A frame the decoder rejects restarts the decoder. A keyframe already queued
  behind the bad frame is kept, so the stream recovers without asking.
- A decoder that falls 45 frames behind drops its queue and asks for a keyframe.
  If the frame arriving then is itself a keyframe, it starts from that instead.

- Decoding and conversion back to RGB run on a decoder thread, and only the
  newest picture is handed to the view. A decoder that stays a frame behind
  still shows a picture at least every 100 ms.
- The picture replaces the canvas's contents whole, with no copy.
- `CallVideoItem` uploads it to the GPU once and lets the GPU scale it, with
  mipmaps when it is shown much smaller. The old view scaled the whole desktop
  with QPainter on every update.

**Fallbacks.** The tile encoder below still exists:

- A build without libvpx uses it (CMake warns loudly). Linux uses the
  distribution's libvpx, Windows `mingw-w64-libvpx` (in the rootless
  toolchain), macOS `brew install libvpx`.
- The engine switches to it mid-share if the VP9 encoder fails on a machine.
- `OPENCHAT_SCREEN_CODEC=tiles` forces it.
- Receivers decode both wire versions. A client from before this change
  receives nothing from a VP9 sender; both ends need this build.

## The tile encoder

A desktop is not a face. It is mostly still, it is full of flat colour and sharp
text, and re-encoding a whole 1080p display thirty times a second to resend
identical pixels would be the most expensive thing this application does, for no
benefit. So the frame is cut into 128-pixel tiles and each tile is hashed; only
tiles that actually changed are encoded. A tile of one colour costs three bytes.
A tile of UI or text — sixteen distinct colours or fewer — goes out as PNG, so
glyphs stay exactly as sharp as they were. Anything busier is photographic and
goes out as JPEG. A motionless desktop costs an eight-byte heartbeat per second.

## The path a frame takes

```
Windows: DXGI Desktop Duplication / Windows.Graphics.Capture
macOS:   ScreenCaptureKit
Linux:   QScreenCapture / QWindowCapture
        │  mapped read-only, never copied
        ▼
ScreenVideoEncoder    → I420 (parallel) → VP9 on its own thread
  (or ScreenTileEncoder: hash → changed tiles → PNG / JPEG / solid)
        │  one stream, however many peers
        ▼
CallScreenSession     ≤24 KiB fragments, AES-256-GCM per peer, per direction
        │
        ▼
CallEngine pacer      waits for room in the socket; voice goes around it
        │
        ▼
SyncEngine datagram → relay → SyncEngine datagram
        │
        ▼
CallScreenSession     authenticate → reassemble (or decode tiles → blit)
        │
        ▼
ScreenVideoDecoder    VP9 → RGB on its own thread, newest picture only
        │
        ▼
ScreenCanvas          one shared surface
        │
        ▼
CallVideoItem         uploaded once as a texture, scaled by the GPU
```

Capture is the platform's own (see the next section); nothing here takes
screenshots in a loop except as the last Windows fallback. Frames are **pulled** on a timer at the
encoder's chosen rate rather than pushed, so a 240 Hz display produces a 30 fps
share and nothing queues behind a busy encoder — what goes out is always the
newest picture, never the oldest stale one. The frame is mapped read-only and the
encoder reads the tiles it needs straight out of the compositor's buffer: at 1:1
there is no copy of a captured pixel anywhere in the sending path. Only the
downscaling path allocates, and only one tile-sized scratch, once per geometry.

## Capture on each platform

Qt Multimedia captures a desktop only through its FFmpeg backend, and the Qt
builds this application ships on do not all have it. The Windows package's Qt
carries the Media Foundation backend alone, and Homebrew's macOS Qt is
configured with `-DQT_FEATURE_ffmpeg=OFF`. On both, `QScreenCapture::start()`
is accepted and then **nothing happens: no frame and no error**. That is why
screen sharing did not work on Windows at all. So `QtScreenCapture` (which
still owns pacing, failure reporting and lifetime) hands Windows and macOS to
`NativeScreenCapture`, and keeps Qt's own path only for Linux, where the
distributions' Qt ships the FFmpeg backend.

**Windows** (`src/call/NativeScreenCaptureWin.cpp`):

- Screens use **DXGI Desktop Duplication** on a Direct3D 11 device created on
  the adapter that drives the display. Duplication is refused from any other
  adapter, which is what bites hybrid-GPU laptops. `DuplicateOutput1` asks for
  8-bit BGRA, so HDR and wide-colour desktops are converted by Windows.
  `AcquireNextFrame(0)` never blocks, which fits the pull model exactly. Desktop
  Duplication reports the mouse pointer beside the image rather than in it, so
  the pointer is drawn in (colour, monochrome and masked-colour shapes) and the
  pixels under it are put back afterwards. Rotated (portrait) displays are
  turned upright.
- When the lock screen, a UAC prompt, a mode change or a full-screen program
  takes the output, the duplication is lost. It is reopened every 250 ms, the
  last picture keeps going out in the meantime, and the share ends only if the
  loss lasts 20 s.
- If Desktop Duplication refuses a display outright, capture falls back first
  to **Windows.Graphics.Capture** for the same monitor, then to a **GDI screen
  copy**. That covers a display on another GPU, HDR on older Windows, virtual
  machines, some remote sessions and Wine. The report and the log say which
  fallback was taken and why.
- Single windows use **Windows.Graphics.Capture** (Windows 10 1903 or later)
  through C++/WinRT. The yellow capture border is turned off where Windows 11
  allows it. Windows are listed with `EnumWindows`: visible, not minimised, not
  cloaked, not owned, not tool windows, not OpenChat's own.

**macOS** (`src/call/NativeScreenCaptureMac.mm`): **ScreenCaptureKit** (12.3 or
later) for displays and windows, scaled on the GPU to the encoder's 1920-pixel
ceiling, with the pointer included. Before the picker opens, the controller
checks Screen Recording permission (`CGPreflightScreenCaptureAccess`). Without
it macOS lists windows without titles and captures only the wallpaper. On the
first refusal the controller asks macOS to show its own prompt, puts the reason
under the call controls, and offers **Open System Settings**. macOS applies the
grant only after a relaunch, and an unbundled binary started from Terminal
needs the permission granted to Terminal. Older macOS falls back to Qt's path.
*This file has not yet been compiled on a Mac;* configure with
`-DOPENCHAT_NATIVE_SCREEN_CAPTURE=OFF` to build without it.

These native APIs deliver a frame only when something changed, but the encoder
heartbeats and answers resend requests only when it is handed a frame. So
`QtScreenCapture` hands a still screen's last frame over again every 200 ms. A
native capture is trusted to report its own liveness once it has produced a
first frame; until then the 8-second watchdog applies, and a capture that
starts but never produces a picture is reported by name.

`OPENCHAT_SCREEN_CAPTURE` overrides the choice for diagnosis: `qt` (Qt
Multimedia everywhere), and on Windows `wgc` (Windows.Graphics.Capture for
screens too) or `gdi` (the GDI copy).

**When sharing does not work on a tester's machine**, ask them to run
`OpenChat --screen-share-check` (on Windows, `OpenChat.exe` from the unpacked
folder in a console). It prints the capture path, the permission state, and
what the picker would offer. Then it captures every screen and one window for
three seconds each and saves the first frame of each as a PNG in the temp
folder, so a sideways, black or pointer-less picture can be seen rather than
guessed. Window titles are never printed.

The relay's part is unchanged and deliberately minimal: media rides the existing
signed `EnvelopeMessageKind::CallMedia` datagram route, which is never stored,
never sequenced and never acknowledged. **No new envelope kind was added, so no
relay redeployment is needed.** The relay routes opaque sealed bytes and cannot
read, let alone re-encode, a single pixel.

## Sound

What the sharer's computer plays (a video, a game, music) goes out beside the
picture, and the far end hears it in stereo on top of the call. OpenChat's own
output is never captured. That output is the other people in the call, and
capturing it would send each of them their own voice back a moment late.

**Capturing** (`src/call/ScreenAudioCapture.h`, one file per platform):

- **Windows**: WASAPI process loopback (Windows 10 2004 and later). A screen
  share captures every process except OpenChat's process tree. A window share
  captures only the tree of the process that owns the window, which covers a
  browser's separate audio process. Exceptions: a window of OpenChat itself,
  or of a Store app (whose window belongs to the frame host), shares
  everything but OpenChat instead. Older Windows shares without sound and says
  why. `OPENCHAT_SCREEN_AUDIO=endpoint` switches to the whole-output loopback
  for testing only. It captures the call too, so nothing picks it otherwise.
- **Linux**: the PulseAudio client API, which PipeWire's pulse server speaks.
  The output's monitor cannot be used, because it contains OpenChat's playback.
  Instead, every application stream except those whose `application.process.id`
  is OpenChat's gets its own monitor stream, and they are summed. Streams are
  followed as applications start, stop and move between outputs. A Wayland
  client cannot tell which application owns a window, so a window share carries
  the same sound as a screen share.
- **macOS**: not yet; the picker says so.

Whatever the platform hands over, and however irregular its pieces, becomes one
20 ms stereo frame every 20 ms (`ScreenAudioFramer`). Each source keeps a 40 ms
cushion. A source running ahead of the clock is trimmed back rather than
allowed to build up delay. Silence is sent too, so the far end hears the
share's sound as on until it actually stops.

**Sending.** Stereo Opus at 128 kbit/s in music mode (`ScreenAudioEncoder`),
encoded once and sealed per peer under its own key domain
(`openchat/call/v1/screenaudio/...`). Wire version `5` uses the voice frame's
header: version, flags, call id, sequence, then the sealed Opus packet. At about
300 bytes a packet it takes the UDP path when there is one, like voice, and
bypasses the picture's pacer. Muting the microphone does not mute the share's
sound. The sound starts and stops with the share, and **Share sound** turns it
on and off mid-share.

**Receiving.** Each peer's `ScreenAudioSession` opens, reorders (a jitter buffer
starting at 120 ms, about what the picture takes to arrive) and decodes. The
`ScreenAudioMixer` sums every share arriving with sound, and the speaker pulls
it once per frame, right after the voices, through `pullStereoOverlay`. The
playback pump adds it to the stereo output after the call's mono has been
widened. A share's stop notice cuts its sound at once. Sound that stopped for
over a second starts afresh, rather than replaying the tail left in the buffer.

**On screen.** The received share shows a speaker in its corner while sound is
arriving. Clicking it mutes; hovering shows a volume slider (the volume is
remembered). Your own preview's caption reads "· with sound" while sound goes
out. If sound could not start, a note under the call controls says why and the
picture carries on.

## Wire format

A VP9 share uses version byte `4` on the same header, with the same keys and
sequence numbers as version 3, content flag only. Each fragment's payload is:

- kind `1`, flags (bit 0: keyframe);
- width and height;
- frame number;
- capture time;
- fragment index and count;
- then the fragment's bytes.

Reports and the stop notice stay version 3. A report carries two extra bytes
after the original twenty: how long the receiver held the newest packet.
Older senders read the first twenty and stop.

A tile share uses version byte `3` on the existing 22-byte media header, beside audio's `1` and the
camera's `2`, so older clients ignore a share and keep carrying voice. Flag `1`
carries picture, flag `2` a receiver's report, and no flags at all means the
share stopped. The payload is `width`, `height`, tile shift, a generation counter
and a tile count, then per tile an index, an encoding and a length.

Four HKDF domains keep four streams apart —
`openchat/call/v1/screen/{caller,callee}` and
`openchat/call/v1/screenfb/{caller,callee}` — because a call can carry a
microphone, a camera, a screen and its reports at once and all four number their
frames from zero. Sharing a key between any two would be the (key, nonce)
collision AES-GCM does not survive. In a group each pair keys from its own
`deriveGroupPairSecret`, exactly as audio and camera do, so the picture is
encoded once for the whole mesh and only the seal is repeated per member.

Receivers check everything a sender declares — canvas size, tile shift, tile
count, index, declared length, and a decoded tile's own dimensions — before
allocating anything for it, and reject stale, replayed, forged and misaddressed
packets. An authenticated peer is still not allowed to hand over a
decompression bomb.

## Quality, and what gives first

The share starts on the middle rung and earns its way up. Reports flow back
along the same path twice a second carrying what arrived, how big the viewer's
window is, and whether the picture needs resending. Loss is measured between two
of the receiver's own acknowledgements — never against what was sent, because
that difference is whatever is still on the wire, and on a slow-moving share
that in-flight frame is most of the window.

| rung | JPEG | fps | scale | ceiling  |
|-----:|-----:|----:|------:|---------:|
| 0    | 92   | 30  | 1:1   | 1.9 MB/s |
| 1    | 86   | 30  | 1:1   | 1.2 MB/s |
| 2    | 78   | 20  | 1:1   | 600 KB/s |
| 3    | 68   | 15  | 1:2   | 300 KB/s |
| 4    | 55   | 10  | 1:2   | 150 KB/s |

Bitrate falls first, then frame rate, then resolution. The rates are ceilings,
not targets: a still desktop sends almost nothing at every rung. Content that
keeps moving — a game, a video — is detected from the changed-tile ratio and
buys 60 fps out of the *same* byte ceiling rather than out of more bandwidth.

Nothing is ever queued. Each frame is given a byte budget, tiles are encoded from
a rotating cursor until it is spent, and whatever did not fit stays marked and
goes next time. A tile that changes three times before it can be sent is sent
once, in its newest state. One datagram is capped at 64 KiB — sized by latency,
not by the relay's 1 MiB limit, because a screen update shares one ordered
connection with the call's audio and a large one is time during which no 20 ms
voice frame can be sent behind it.

Resolution is capped at a 1920-pixel edge before any adaptation, so a 4K desktop
goes out as 1080p, and capped again by the size the viewer is actually drawing
it. A share nobody is displaying drops to 2 fps rather than encoding pixels for
a closed window.

## Where the memory goes, and when it comes back

The sending half holds a tile hash per tile and, only when downscaling, one
tile-sized scratch. The receiving half holds exactly one canvas. That canvas is a
shared, mutable surface rather than a QImage value: handing a copy-on-write image
to the view would mean the next tile detaches it and copies a whole desktop, so
instead every holder keeps a `ScreenCanvasPtr` and the session writes tiles into
the one buffer. Its geometry never changes — a resolution change produces a *new*
canvas — so a view still painting the old one can never be left pointing at freed
pixels.

The view uploads each new picture once, as a texture the size of the picture,
and the GPU scales it into the item, with mipmaps when it is drawn much smaller.
Scaling on the CPU, as the view used to, cost more than decoding at 30 fps.
A picture that has not changed is never uploaded again, so a heartbeat from a
still desktop costs nothing, and an item that is not visible is not drawn. A
VP9 share replaces the canvas's contents whole each frame rather than writing
into them, so the picture is handed from decoder to canvas to texture without
a copy.

Stopping a share, ending a call, a peer leaving, or the app changing calls
releases all of it: hashes, scratches, canvas, encoder, capture session and
timers. The engine refuses screen frames until a share is explicitly armed
again, so a capture callback still in flight when the user presses stop cannot
quietly start the whole thing up again.

## Enlarging a picture, and filling the window with the call

Every camera tile and every screen pane carries a small zoom chip in its
bottom-right corner (`MediaIconButton`). Pressing it grows that picture to the
largest rectangle of its own aspect that fits the window, over a scrim that
darkens everything else and swallows every click; a click anywhere off the
picture, or Escape, shrinks it back onto the tile it came from. The picture
grows from exactly where the tile is and returns to exactly there, so the eye
follows one object rather than watching one vanish and another appear.

The enlarged picture (`MediaZoomOverlay`, one instance at the window level) is
a *second* `CallVideoItem`, not the tile re-parented: it follows the tile
through the view's `source` property, which copies frame, canvas and mirroring
in C++ from the source's own change signals. That keeps thirty camera frames a
second out of QML bindings, and it matters more for a share: a `ScreenCanvasPtr`
read through a two-level binding is wrapped in a JavaScript object that keeps a
desktop's worth of pixels alive until the next garbage collection, whereas the
C++ path lets go of them the moment the share stops. While the copy is up the
tile underneath is `paused` — it is under the scrim, so repainting it would be
work nobody could see — and it repaints itself whole when resumed.

Enlarging the far end's share changes what the sender is told: the stage
reports the enlarged size as the view size, so the share is encoded for most of
the window rather than for the strip, and the strip's size again when it
closes. A share that stops, or a camera that turns off, takes its enlarged copy
down at once rather than leaving an empty frame on screen.

The chip in the bottom-right corner of the call surface fills the window with
the call. The sidebar and the conversation are hidden — not shrunk or scrolled
away, so nothing about them is laid out or painted — and the call surface
becomes the whole pane, centring its content and letting the pictures grow into
the room: a lone camera row takes what the controls leave, and with a share on
stage the cameras stay modest and the share takes the rest. Escape, the chip
again, or the end of the call give the window back. At the minimum window width
the live-call action row only just fits the pane, so when it would run under the
chip it closes up and leans left instead.

## Instrumentation

`CallController::screenShareDiagnostics()` reports resolution, rung, frame rate,
quality, tiles sent against total, bytes and microseconds for the last update,
frames sent, idle and paced out, measured loss, round-trip time, the viewer's
window size, and what has been received. For a VP9 share it reports the
bitrate and link cap, libvpx's speed and encoder load, keyframes, frames held
back by the pacer, and the bytes queued. The `openchat.screenshare` logging
category is off unless `QT_LOGGING_RULES` turns it on, so a shipped build says
nothing about a running share.

## Validation

- `tst_screenaudio`:
  - A tone on the left comes out on the left, at least 26 dB clear of the
    right.
  - Replayed, tampered, wrong-version, wrong-call, reflected and voice-keyed
    packets are refused.
  - Sound that stops stops being active, and the mixer follows the volume.
  - The framer keeps one frame per 20 ms through bursts and gaps, trims a source
    that runs ahead, and sums sources.
  - 44.1 kHz float mono converts to 48 kHz stereo at the same pitch.
  - `ownPlaybackIsNeverCapturedButOthersIs`, against a real sound server:
    OpenChat's own tone is never captured, and another program's is. Point
    `OPENCHAT_TEST_PULSE_SINK` at a null sink to run it (`pactl load-module
    module-null-sink sink_name=...`). With the process check disabled it fails
    ("captured our own tone at 16383").
- `tst_callengine::aSharesSoundReachesThePeerInStereoAndStopsWithIt` and
  `tst_groupcall::aSharesSoundIsEncodedOnceAndHeardByEveryMember`: the engine's
  send and receive halves, one-to-one and in a mesh.
- `OpenChat --screen-share-check` ends with the sound: three seconds through the
  same capture a call uses, with the frame count and the loudest level.
- `tst_screenvideo`: colour survives the trip through I420, and a larger source
  is scaled into it. A scrolling desktop survives encode and decode (above
  28 dB). The first frame is the only keyframe until one is asked for. The
  decoder waits for a keyframe and asks for one when the stream breaks. A
  keyframe spanning several fragments is reassembled, a tampered fragment is
  refused, and a lost fragment gets its keyframe request out at once. A loss
  mid-frame asks once, not twice. A viewer still waiting well after asking asks
  again. A keyframe queued behind an undecodable frame still shows. RGBA8888
  captures keep their colours, at 1:1 and scaled. A still screen stops costing
  frames after it sharpens.
- `tst_relayclient::aCongestedTlsLinkShowsItsBacklogAndDropsMedia`: against a
  real TLS relay that stops reading, the backlog is visible to the pacer and
  the datagram gate keeps it under 512 KB. Reading `bytesToWrite()` alone, it
  shows 0 and the test fails.
- `tst_callengine::screenDataWaitsForRoomWhileVoiceGoesStraightThrough`: with
  the socket full, nothing screen-sized reaches it, frames are held back
  instead of piling up, voice still goes out at once, and everything held
  arrives intact once there is room. The engine and group-call share tests run
  on both encoders.
- `openchat-screen-bench`: the before/after table above.
- `tst_screenshare`: reconstruction, delta efficiency, lossless text, flat-region
  cost, resolution capping at 1080p/1440p/4K/ultrawide, explicit frame pacing
  against a 240 Hz source, the packet ceiling, stop, restart, gap detection and
  repair, the quality ladder in both directions, in-flight frames not read as
  loss, an unwatched view, forged/replayed/misaddressed/malformed packets, both
  ends sharing at once, one encode for a whole mesh, every supported capture
  format, and sixty start/stop cycles measured against resident memory.
- `tst_callengine`: a share reaching the peer and clearing on stop, camera and
  screen together without disturbing voice, repeated start/stop, a dropped link
  losing nothing, and hangup releasing everything.
- `tst_groupcall`: one encode sealed for every member, and a member leaving
  mid-share.
- `tst_qmlload`: the button beside the camera in every call state, a share
  raising and releasing the stage across the C++/QML boundary, a camera
  enlarging over the window at its own aspect and shrinking back on a click
  outside, an enlarged share reported to the sender at the enlarged size and
  released the moment the share stops, and the call filling the window and
  giving it back on Escape and when the call ends.
- `tst_e2e::callCarriesAudioVideoAndAScreenOverRealTls`: a share through real
  TLS, a real relay and real PostgreSQL, including restart at a new geometry and
  the relay storing none of it. Requires the PostgreSQL test service.
- `OpenChat --call-screen --capture <path.png>`: the surface with a received
  share, rendered without opening a display, at 1100x780 and at the minimum
  720x560. `--call-picker` renders the source picker over it, listing this
  machine's real displays and windows without capturing any of them.
  `--call-zoom` enlarges the far end's share (or, with `--call-video`, its
  camera) over the window, and `--call-fullscreen` fills the window with the
  call; both combine with `--call-screen` and `--call-video`.
- `openchat-call-check --screen`: one side of a real call against a real relay,
  so two machines can verify a share over an actual network path. The answering
  side reports whether the lossless regions arrived byte for byte.

Verified 2026-09-06 against `https://chat.rigidstudios.de/v1`: 1280×800 received
at full resolution, every lossless sample point exact, photographic drift 2/765,
124 KiB for twelve seconds of a near-static desktop, and audio results identical
to a run with no share at all.

The automated tests do not cover real capture hardware, OS capture-permission
prompts, or Wayland portal flows. The Windows backend is cross-compiled and runs
under Wine, but Wine implements neither Desktop Duplication (`E_NOTIMPL`) nor
Windows.Graphics.Capture. There it proves enumeration, the refusal path and the
GDI fallback end to end, not real pixels. The macOS backend has not been
compiled on a Mac yet. For a hardware check, run two clients, start a
call, share a display and then a window, unplug a monitor mid-share, close the
shared window, and confirm the button returns to **Share screen** each time.
