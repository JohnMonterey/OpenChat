# Camera video in calls

The conversation's video button places the same call as the phone button and
enables the local camera. During a call, **Camera on / Camera off** controls only
the local camera. Answering an incoming call never starts the recipient's camera.
Permission denial, a missing camera, or capture failure leaves the voice call up
and shows an error below the controls. Turning video off or ending a call releases
the camera, including cancelling a pending permission request's continuation.

Camera frames are sampled at up to 15 fps, scaled proportionally to a maximum
640-pixel edge, and encoded as independent JPEG frames. Each frame travels through
the existing signed, ephemeral call-media relay route, without MLS ratchet steps,
outbox persistence, or inbox storage. Frames above the 96 KiB payload budget are
scaled down once more. The relay client's pending socket queue is bounded by
dropping disposable media when it exceeds 128 KiB.

Video uses the existing 22-byte media header shape with version byte `2`; audio
continues using version `1`. Video flag `1` carries JPEG data; flag `0` carries an
empty camera-off payload. HKDF labels `openchat/call/v1/video/caller` and
`openchat/call/v1/video/callee` separate video keys from both audio and the opposite
direction. The authenticated header includes the call ID, sequence and flags.
Sequences persist across camera toggles and never wrap. Receivers reject stale,
replayed, malformed, oversized and unauthenticated frames before displaying them,
and check JPEG dimensions before allocating decoded pixels. A missing video
stream returns to the avatar after 2.5 seconds, even if the off packet was lost.
Older voice clients ignore version `2` packets and continue carrying audio.

Each participant's existing avatar frame expands independently to the camera's
aspect ratio, preserving the speaking ring and name. The renderer fits the whole
image and rounds its corners. Only the local preview is mirrored. The layout
reserves room for chat and controls at the minimum 720 × 560 window size.

Validation:

- `tst_videocall`: image content and aspect ratios, both directions, packet bounds,
  key separation, toggles, loss, reordering, replay and tampering.
- `tst_callengine`: simultaneous audio and video, independent camera state,
  stale-video timeout, and hangup cleanup.
- `tst_qmlload`: landscape/portrait layout, controls, and return to avatars at
  three window sizes.
- `tst_e2e::callCarriesAudioAndVideoOverRealTls`: both media types through the real
  TLS relay; requires the existing PostgreSQL test service.
- `OpenChat --call-video --capture <path.png>`: synthetic landscape/portrait preview
  that does not access a physical camera.

For a hardware check, run two clients against a relay, start a voice call, enable
each camera independently, mute while streaming, then turn cameras off and end
the call. Also check camera permission denial and unplugging a camera. The preview
and generated-frame tests do not validate camera hardware or OS permission prompts.
