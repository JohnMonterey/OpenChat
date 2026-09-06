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

## Why not the camera codec

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
QScreenCapture / QWindowCapture
        │  mapped read-only, never copied
        ▼
ScreenTileEncoder     hash → changed tiles → PNG / JPEG / solid
        │  one payload, however many peers
        ▼
CallScreenSession     AES-256-GCM per peer, per direction
        │
        ▼
SyncEngine datagram → relay → SyncEngine datagram
        │
        ▼
CallScreenSession     authenticate → decode tiles → blit
        │
        ▼
ScreenCanvas          one shared surface, written in place
        │  dirty rectangle only
        ▼
CallVideoItem
```

Capture is the platform's own, through `QScreenCapture` and `QWindowCapture`;
nothing here takes screenshots in a loop. Frames are **pulled** on a timer at the
encoder's chosen rate rather than pushed, so a 240 Hz display produces a 30 fps
share and nothing queues behind a busy encoder — what goes out is always the
newest picture, never the oldest stale one. The frame is mapped read-only and the
encoder reads the tiles it needs straight out of the compositor's buffer: at 1:1
there is no copy of a captured pixel anywhere in the sending path. Only the
downscaling path allocates, and only one tile-sized scratch, once per geometry.

The relay's part is unchanged and deliberately minimal: media rides the existing
signed `EnvelopeMessageKind::CallMedia` datagram route, which is never stored,
never sequenced and never acknowledged. **No new envelope kind was added, so no
relay redeployment is needed.** The relay routes opaque sealed bytes and cannot
read, let alone re-encode, a single pixel.

## Wire format

Version byte `3` on the existing 22-byte media header, beside audio's `1` and the
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

The view's texture is the size of the *item*, not of the far end's display, so a
4K share in a 400-pixel panel costs 400 pixels of video memory. Only the changed
rectangle is repainted and re-uploaded, a heartbeat from a still desktop costs
nothing at all, and an item that is not visible is not painted.

Stopping a share, ending a call, a peer leaving, or the app changing calls
releases all of it: hashes, scratches, canvas, encoder, capture session and
timers. The engine refuses screen frames until a share is explicitly armed
again, so a capture callback still in flight when the user presses stop cannot
quietly start the whole thing up again.

## Instrumentation

`CallController::screenShareDiagnostics()` reports resolution, rung, frame rate,
quality, tiles sent against total, bytes and microseconds for the last update,
frames sent, idle and paced out, measured loss, round-trip time, the viewer's
window size, and what has been received. The `openchat.screenshare` logging
category is off unless `QT_LOGGING_RULES` turns it on, so a shipped build says
nothing about a running share.

## Validation

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
- `tst_qmlload`: the button beside the camera in every call state, and a share
  raising and releasing the stage across the C++/QML boundary.
- `tst_e2e::callCarriesAudioVideoAndAScreenOverRealTls`: a share through real
  TLS, a real relay and real PostgreSQL, including restart at a new geometry and
  the relay storing none of it. Requires the PostgreSQL test service.
- `OpenChat --call-screen --capture <path.png>`: the surface with a received
  share, rendered without opening a display, at 1100x780 and at the minimum
  720x560. `--call-picker` renders the source picker over it, listing this
  machine's real displays and windows without capturing any of them.
- `openchat-call-check --screen`: one side of a real call against a real relay,
  so two machines can verify a share over an actual network path. The answering
  side reports whether the lossless regions arrived byte for byte.

Verified 2026-09-06 against `https://chat.rigidstudios.de/v1`: 1280×800 received
at full resolution, every lossless sample point exact, photographic drift 2/765,
124 KiB for twelve seconds of a near-static desktop, and audio results identical
to a run with no share at all.

The automated tests do not cover real capture hardware, OS capture-permission
prompts, or Wayland portal flows. For a hardware check, run two clients, start a
call, share a display and then a window, unplug a monitor mid-share, close the
shared window, and confirm the button returns to **Share screen** each time.
