# Sending photos, videos, audio and files

Any chat, one-to-one or group, can carry photos, short videos, audio clips
and files. They are end-to-end encrypted like text: the relay only ever sees
sealed bytes.

This builds on [message-actions.md](message-actions.md) (shared message ids,
replies) and reuses the codecs of [profile-panels.md](profile-panels.md).

## What the user sees

- **The "+" button** sits left of the message field, a 40 px round Aero
  button level with the field's last line. It replaced the chevron that used
  to sit inside the field's right edge. Clicking it, or **Ctrl+O** in the
  composer, opens the attach menu and turns the "+" into a cross; clicking the
  cross, Esc, or a click anywhere else closes it again.
- **The attach menu** rises from the button: *Photo* (JPG, PNG, and WebP,
  BMP or TIFF where Qt can read them on this computer),
  *Video* (up to 1 minute), *Audio* (MP3, M4A, WAV… up to 5 minutes) and
  *File* (any file up to 16 MB), each with a coloured chip. Up and Down move
  between them and Enter picks one. *Video* is greyed, with *Not available on
  this computer*, in a build without libvpx. The menu is unavailable while a
  message is being edited or while the chat hides its messages.
- **Picking** opens the system file dialog for that kind; several files may be
  chosen at once. What a file becomes is decided by its name, not by the row
  it was picked from, so a PDF picked from *Photo* is still sent as a file.
- **The tray.** Picked files wait as cards above the field: a thumbnail for
  photos and videos (with the length on videos), a chip, name and size for
  audio and files, a progress bar while a file is being prepared and a small
  cross to take it out. A card explains itself when something changed on the
  way (*Only the first minute will be sent.*, *Couldn't prepare this as a
  photo — it will be sent as a file.*) or could not be done (*Files up to
  16 MB can be sent.*). At most 10 files wait at once.
- **Sending.** Enter sends every ready card, each as its own message, oldest
  first. Whatever was typed becomes the caption of the first one, and a reply
  that is being written answers the first one. Pressing Enter while a card is
  still being prepared shows *Sends when ready…* and sends as soon as all are;
  if none could be prepared the caption stays in the field rather than going
  out on its own. A text sent right after attachments waits behind them while
  they are sealed, so everyone reads them in the order they were sent.
- **Dropping and pasting.** Local files dropped anywhere on the open chat are
  staged the same way (*Drop to attach* shows while dragging). Ctrl+V or
  Shift+Insert with files copied in the file manager, or with a picture and
  no text beside it (a screenshot, a browser's *Copy image*), stages them;
  any other paste is ordinary text, so copied cells or slides, which carry a
  picture of themselves too, paste as their text.
- **In the chat**:
  - a *photo* fills its bubble (its shape kept within sensible limits) and
    opens large in a viewer on a click, with *Save* and *Copy image*;
  - a *video* shows its first picture, a play button and its length; it plays
    in the viewer with play/pause, a seek bar and the time;
  - *audio* is a row with a play button, a waveform and the time; clicking the
    waveform seeks. It keeps playing while the chat scrolls or new messages
    arrive, and stops when a call starts or another chat is opened;
  - a *file* shows its type, name and size, with *Save…*.

  A caption sits under the media. Without one, the time sits on the picture.
- **Progress.** While the bytes travel the bubble says so: *Sending… 40%*,
  *Waiting for the call to end*, *Waiting for Name* (nothing has arrived
  yet), *Receiving… 3 of 7*. The sender can stop a transfer with the cross
  on it (*You stopped sending this*; the others see *Name stopped sending
  this*). *Couldn't receive this photo* means the bytes did not match what
  the sender described; *Not enough space to receive this* means this device
  would not store more. *Not sent. Try again* under a failed send puts the
  file back in the tray, with its caption in the field.
- **Actions.** Hovering offers *Reply*, *Copy* (the caption, when there is
  one) and *Save* for a finished photo or file. Attachments cannot be edited.
  Notifications, reply quotes and the reply bar say *Photo*, *Video*, *Audio*
  or the file's name, followed by the caption.

## How it works

### Two kinds of traffic

An attachment is sent in two parts (`src/domain/Attachment.*`).

The **message** is an ordinary MLS application message, a tagged
`MessageContent` of type 3, so it takes a row, a shared id and delivery
states exactly as a text does:

    0xFF  CBOR [1, 3, caption, attachment id, descriptor, reply or null]

The descriptor is small: the kind, the size, the SHA-256 of the whole blob,
the number of parts, a file name, MIME type, pixel size, length and waveform
where they apply, and a fresh random 32-byte **key**. It never carries the
bytes themselves or the preview.

The **frames** carry everything else, as `AttachmentControl` envelopes
(kind 3, which every deployed client and relay already accepts). A frame is a
23-byte header and a body sealed with the attachment's key
(`src/security/AttachmentSeal.*`):

    0xAC  1  type  attachment id (16)  index (u32)  |  AES-256-GCM body + tag

The header is the associated data, so a body cannot be moved to another
attachment, index or type. A part's nonce is made of its type and index: a
part is sealed once and sent again byte for byte. Every other frame can be
sealed again with different content under the same key (a receiver asks
again for fewer parts, and each member of a group asks for its own), so its
body starts with a fresh random 12-byte nonce, its top bit set so it can
never be a part's. Types are *part* (224 KiB of the blob), *preview* (a small
JPEG), *request* (a bitmap of the parts a receiver still lacks) and *cancel*.

Frames never touch the MLS ratchet. That is deliberate: a file of 74 parts
would otherwise spend 74 generations of the sender's ratchet, a part delayed
past the 32 the receiver tolerates would be lost for good, and a peer who
removed this device from a group could make every part fail to encrypt,
which stops the whole engine. A frame can instead be sent again byte for
byte, and parts may arrive in any order.

### Who is trusted with what

The descriptor is MLS-authenticated, so its hash is the sender's. A frame
opens only under the key, which only the message's recipients hold, so the
relay cannot forge or alter one. A group member who holds the key could still
send garbage parts in the sender's name only with the relay's help, and then
the assembled bytes would not match the hash: the transfer fails, it never
shows someone else's content. Parts are filed under the device the relay
authenticated as their sender, the conversation and the attachment id.

What arrives is hostile until checked (`attachmentBlobIsValid`): its size and
hash, then per kind a baseline or progressive JPEG with at most 32 scans whose
frame size matches the descriptor (a progressive or multi-scan frame, which a
decoder holds whole however small it is drawn, only up to 2048 px), a video
sequence whose every segment decodes, or a song container within the chat
limits. Previews must be JPEGs of at most 16 KiB and 320 px. The video player
reads every VP9 frame's header before libvpx does and refuses a frame that
codes itself larger than a clip may be. File names are cleaned again on arrival
(`sanitizeAttachmentFileName`: no control, format or bidi characters, no path
separators or Windows device names, a short extension kept) and every peer
string is shown as plain text. A file is never opened by OpenChat; *Save…*
writes it with `QSaveFile` to a local path the user chose, and on Windows and
macOS marks it as downloaded so the system warns before running it.

### Moving the bytes

`AttachmentTransfer` (`src/app/`) runs beside the engine.

- **Sending.** Before the message is encrypted, every part is sealed and
  written to disk (`AttachmentFileStore`, one `.ocab` file per attachment
  under the profile's `attachments/` directory, each sealed part at a fixed
  offset). Plaintext never touches the disk. Once the relay has taken the
  message, a pump hands the engine one frame at a time: the preview, then
  each part. It waits while the link is down, during a call, while the socket
  holds more than 64 KiB, and while any earlier frame is still in the outbox
  on its first attempt (one the relay leaves unanswered retries on its own).
  A device the relay refuses (one that does not exist or was retired by a
  login elsewhere) is sent no more frames in this run unless it asks for
  parts itself; nothing waits on it.
  Frames also sit behind every other kind of traffic in the outbox (its new
  `priority` column), so a text, receipt or call signal never waits for a
  file. Parts go only to the devices the message went to that are still in
  the chat, so someone added later never receives an older file.
- **Receiving.** Each part is opened with the key before it is kept. Frames
  that arrive before their message (possible after a retry) are kept sealed,
  only from someone in the chat, for a day, and within 16 files and 32 MiB per
  sender, counted by how far each file reaches (a part sits at its fixed
  offset); they are checked when the message arrives, and what is kept is
  then counted at the sizes the message gives. When every part is in, a worker thread assembles and checks the
  blob. A receiver that has made no progress for 2 minutes while online asks
  the sender for what is missing, backing off up to 6 hours; the sender
  answers only for parts it has already sent, at most once every 10 minutes
  per attachment and asker. Frames live 24 hours at the relay, so a device
  away longer asks for what it lacks. A transfer nobody is left to finish
  (the chat was left, or its sender has not been in it for a day since the
  last part) fails and its file is freed.
- **Limits.** Nothing is received while less than 512 MiB of disk is free or
  once 8 GiB of received attachments are held. One send may put at most
  256 MiB on the wire (size × recipients), so a 16 MB file can go to a group
  of up to 16 others.

### Sizes

| kind | as sent | cap |
|---|---|---|
| Photo | baseline JPEG, long side ≤ 2048 px | 2 MiB |
| Video | 480 px VP9 + Opus, 5 s segments | 1 minute |
| Audio | one Opus song, 48 kHz | 5 minutes, 4 MiB |
| File | the bytes as they are | 16 MiB |

Photos, videos and audio are converted on the sender's machine, at most one
video or audio file and two others at a time. A WAV whose first five minutes
are more than 192 MiB (32-bit float at 96 kHz, say) is streamed through the
decoder rather than read whole. A photo, video or audio file that cannot be
converted but fits 16 MiB is sent as a file instead.

### Storage

Migration 018 adds `message_attachments` (one row per attachment message,
with its descriptor, key, state and, for sends, the devices it went to and how
far it got), `attachment_transfers` (which parts have arrived, per
conversation, sender and attachment, including frames whose message has not
come yet) and `outbox.priority`. The blobs themselves live sealed in the
`attachments/` directory, which removing the profile or erasing local data
already deletes. Orphaned frames are collected after a day; a failed or
cancelled incoming transfer frees its file.

## Compatibility promises

- **The relay needs no change to carry attachments**: both parts travel as
  envelope kinds it already carries. Redeploying it is strongly recommended
  all the same. The new relay
  - answers an envelope for a device that does not exist or was retired with
    *RecipientUnavailable*, so a sender stops at once instead of retrying for
    minutes;
  - replays a connecting device's inbox a stretch at a time (at most
    256 KiB waiting in its socket), so its pong is never stuck behind a
    backlog;
  - writes large messages in 16 KiB WebSocket frames and counts any frame as
    a sign of life, as the new client does both ways; the client also counts
    its own upload still leaving as one.
- **0.2.8 and 0.2.9** do not show attachments: the message is a tagged type
  they do not know, and a frame is not an MLS message they can open. They
  never acknowledge a frame, so a device on them is handed the same frames
  again on every connect until they expire (24 hours). Through an old relay
  on a slow link that backlog can hold their heartbeat's pong back long
  enough to drop the link, and then nothing sent after it arrives until the
  frames expire; the new relay's paced replay avoids that. There is no
  capability flag yet that would let a sender leave such devices out.
- **0.1.x** shows the message as one short line of unreadable text, as it
  already does for replies, and treats the frames as 0.2.x does.
- **Wire version stays 1.** Profile songs, clips and pictures are unchanged:
  the codecs take their old limits by default.

## Limits and follow-ups

- There is no voice-note recorder yet; audio comes from files.
- Received videos and audio can be played but not saved, since they are
  OpenChat's own containers; send the original as a *File* to share it
  as it is.
- Sent attachments stay on disk so that a lost part can be sent again and a
  failed send retried. Received attachments are kept until the chat's
  history is; there is no eviction beyond the 8 GiB ceiling yet.
- The video importer converts pictures on the GUI thread, which is why videos
  are capped at a minute.
- The Windows package does not ship Qt's WebP and TIFF image plugins, so
  there the Photo row offers JPG, PNG and BMP only.
- A video seeks to the start of the 5-second segment that holds the chosen
  time.

## Tests

- `tst_attachment`: the frame codec and sealing (every tamper case, and that
  requests never share a nonce), the descriptor rules, file-name cleaning,
  the video sequence codec, blob and preview checks (large progressive
  frames refused), and the chat song limits.
- `tst_messagecontent`: the attachment message, its bounds, and that older
  decoders refuse it.
- `tst_syncstore`, `tst_repositories`, `tst_profilepagestore`: migration 018
  (including the upgrade path), the attachment rows, outbox priority.
- `tst_syncengine`: refusals never encrypt and never stop the engine; frames
  bypass the ratchet; replays are not signalled twice.
- `tst_attachmenttransfer`: two real peers — photos and a 16 MB file end to
  end, groups with a busy link, a retired member device, reordering, loss and
  recovery, restarts, calls, cancelling, budgets (early frames by extent) and
  cleanup, stranded transfers.
- `tst_relayservices` (needs the PostgreSQL test service): a retired
  recipient is refused at once, deliveries go in small frames, and a long
  backlog still lets the pong through.
- `tst_chatattachmentimport`: preparing real photos, videos (needs ffmpeg and
  libvpx), audio and files, the fallbacks, the photo types this computer can
  read, and that dropping an audio card never holds the window.
- `tst_profileclip`: the decoder refuses VP9 frames larger than any clip;
  large progressive pictures are not decoded.
- `tst_chatcontroller`, `tst_models`: the tray, sending, retry, summaries,
  pasting, a caption that never goes alone, a text that waits its turn.
- `tst_qmlload`: the composer layout, the menu, the tray, dropping, every
  kind of bubble in light and dark and under skins, right-to-left names, and
  the viewer (a photo of any shape fitted whole). `OPENCHAT_ATTACHMENT_CAPTURES=<dir>` saves its screenshots.
- `tst_e2e` (needs the PostgreSQL test service): a photo end to end over the
  real relay.
- Captures: `OpenChat --attachment-menu --capture <png>` and
  `OpenChat --attachment-demo --capture <png>`.
