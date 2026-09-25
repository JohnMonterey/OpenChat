# Profile pages

Every user has a profile page they can design themselves, in the spirit of
MySpace's 2005-2008 "Profile 1.0" pages: a two-column stack of boxes with
coloured header strips over a wallpaper, restyled however its owner likes.
Clicking a person's picture anywhere in OpenChat opens their page.

Owners can also add panels of their own: text, pictures, a video, lists
such as favorite games, and dividers. See [profile-panels.md](profile-panels.md).

## What the user sees

### Opening a profile

A person's picture opens their profile wherever it is shown:

- a contact's row in the sidebar (the picture only; the rest of the row still
  opens the chat), and "View profile" on a right click;
- your own picture in the sidebar (your page; *Change picture* now lives there
  and in the editor);
- the open chat's header (also a Tab stop, and Ctrl+I from anywhere);
- a friend request's row and the Search & Find result (a private stub, below);
- a call tile while that person's camera is off (a live camera still means
  "enlarge");
- a Top Friends tile on someone's page.

Pointing at a picture draws a focus-coloured ring, a halo and a small profile
badge; the face is never darkened (darkening plus a glyph stays the "change
picture" language on your own photo). A group's picture opens nothing.

The page covers the whole window under an OpenChat-owned top bar: Back (named
after where it returns to, with the history on a right click), the person's
name, handle and real presence, and *Plain style* or *Edit profile*. Escape,
Alt+Left and the mouse's Back button go back; profile to profile keeps a back
stack. A call that rings or runs while a page is open shows the usual call
strip under the bar: a profile never hides a call.

### The page

The narrow column holds the name and photo (headline, three short info lines,
mood, "Online Now!" from real presence, the status line), *Contacting <name>*,
the OpenChat handle with Copy, the profile song, Interests and Details. The
wide column holds "<name> is in your contacts.", the Blurbs (*About me*, *Who
I'd like to meet*) and the Friend Space (Top Friends, up to 8, names above
pictures). Empty boxes are hidden. Two columns hold from the 720 px minimum
window up; the content caps at 940 px and centres, the wallpaper filling the
rest. The owner can flip the columns or use one.

*Contacting* shows only actions that really work: Send Message, Voice Call,
Video Call and Safety Number (disabled with a reason, or "Return to call",
during a call). Your own page shows *Your Profile* instead: Edit Profile,
Change Picture, Copy Invite Link. There are no favorites, blocking of
contacts, comments, blogs or photo albums, because OpenChat has none.

The profile song is a small player that **never plays by itself**; only one
song plays at a time, and it pauses when a call rings.

Anyone who is not a contact (a request, a search result, a call member or a
Top Friend you don't know) gets a calm stub card instead: their name,
relay-confirmed handle, why you are seeing them, "This profile is private",
and only the real actions (Accept / Decline / Block @x for a request, *Send
contact request* where a handle is known).

### Customising your page

*Edit profile* opens the editor beside a live, true-scale preview of your
page (*Preview* hides the panel for the two-column view on narrow windows).
Its tabs are Themes, Background, Boxes, Text, Name & FX (Design) and About me,
Top Friends, Song, Layout (Content). Hovering or focusing a preset for a
moment tries it on; Undo/Redo, an *Unsaved changes* pill, a local autosaved
draft and a Save / Discard / Keep editing dialog protect the work. Nothing is
sent until Save.

Ten presets ship, all free: Aero Sky (the default, following light and dark
mode), Classic '06, Scene Queen, Neon Zebra, Midnight Emo, Glitter Girl,
Safety Pin, Headliner, Linen and Chrome Y2K. Every knob is typed data, never
markup: background (solid, gradient, one of 17 wallpaper patterns or an
uploaded picture tiled, filled, fitted or centred), box colour and
see-through, border colour, width and style, corners, neon edge, header strip
style and colours (optionally a second family for the right column), table
style, heading and body fonts, text size and colours, and the name's font,
size, colours, effect (glow, outline, gradient, shadow, animated glitter,
chrome) and flourish (★, xXx, ♥, ~*, ♫, ✿), plus falling stars, hearts, snow
or sparkles behind the boxes.

**Readability is enforced by the renderer.** Body text, labels and links are
held at 4.5:1 against the box they sit on (worst case over the wallpaper),
header titles against their strip and the name at 3:1: the box is made more
opaque first, then the ink moves toward black or white, and the editor says
what it adjusted. The floor cannot be turned off, and nothing blocks Save.

*Plain style* (the top bar's switch and Settings › Appearance) shows
everyone else's page in OpenChat's own look with the same words, pictures,
friends and actions.

## How it works

### On the wire

Pages travel inside the existing `ProfileUpdate` envelope kind (7), so the
relay needed no change. A page message is the byte `0xFF` followed by one
canonical CBOR map (`src/domain/ProfilePageCodec.*`): `PageCore` (theme,
layout, words, Top Friends and references to media by SHA-256), `PageMedia`
(one blob: the background JPEG or the song) or `PageRequest`. `0xFF` is the
CBOR break byte, so a client from before this feature fails to parse it and
ignores the message silently; presence, status and the picture keep using the
unchanged legacy message. Decoding rejects anything malformed or oversized,
ignores unknown keys, and clamps out-of-range values, so newer pages still
render on older clients of this version.

Every page payload stays under 240 KiB, below MLS's 256 KiB plaintext cap
(an encrypt over it would stop the engine for the session): a core is at most
24 KiB, and each media blob at most 224 KiB. A background is re-encoded to a
baseline JPEG of at most 1920 px on its long side; a song is at most 45 s of
Opus (48 kHz, 40 kbit/s stereo) in OpenChat's own container
(`src/domain/SongContainer.*`), loudness-normalised, imported from WAV
everywhere and from other formats where Qt's decoder reads them.

### Delivery

`ProfilePageSync` (`src/app/ProfilePageSync.*`) publishes on Save and pumps
the page to one contact at a time while the link is up and its send backlog
is low, and not during a call. Media goes only to contacts known to run a
page-capable client, and only blobs that contact has not been sent. Viewers
ask for what they miss in the background (at start, after acceptance, when
media is missing), never because a page was opened, so the owner cannot tell
who looks at their page; the one exception is re-fetching media the viewer's
own storage evicted. Answers are bounded (4 a day per contact). The engine's
outbox now deletes settled control envelopes and paces its drain on the
socket's backlog (`SyncEngine`, `SqlCipherSyncStore`).

### Storage

Migration 016 adds the local draft and published page, received pages and
their media (scoped per owner, so one contact's page can never reveal whether
you know another), delivery and request bookkeeping, and the local handle
(`src/storage/SqlCipherProfilePageRepository.*`). Received media has a 96 MiB
soft cap; beyond it the least recently viewed contacts' media is evicted
(their words stay). None of these tables has a foreign key to `contacts`.

### Rendering

The page is QML (`qml/OpenChat/components/Profile*.qml`) over native
QPainter items in `openchat_profile` (`src/profile/`): the backdrop and its
motifs, the picture layer, the styled name, the ambient sprites, preset
miniatures and mood faces, all software-scene-graph safe (no shaders). The
contrast engine is `ProfileReadability`. One shared ticker drives every
animation at up to 30 fps and stops when the page is covered, the window is
hidden or minimised, or Low memory mode is on. Decoded pictures and fonts are
loaded only when a page first needs them.

`ProfileController` (owned by `ChatController`, as `chatController.profiles`)
is the QML surface: opening and the back stack, the viewed person and page,
the owner's draft with undo and imports, and the catalogues. `Main.qml` hosts
the page over everything except the contact dialogs, the screen-share picker
and an enlarged picture; the chat underneath takes no input, stays unread and
stops rendering, and the keyboard returns where it was when the page closes.

## Fonts

Profile content may use eight bundled faces (`assets/fonts/`, with their
licences; see its README): the interface font, Fredoka, Pacifico, Courier
Prime, Press Start 2P (names only), UnifrakturMaguntia, "OpenChat Future" and
"OpenChat Serif". The last two are static weight instances cut from Orbitron
and Playfair Display (`tools/fonts/make_static.py`) and renamed, because those
names are Reserved Font Names under the SIL OFL and a cut is a Modified
Version. App chrome always uses the interface font.

## Testing and tools

- `tst_profilepage` (model, codec, container), `tst_profilepagestore`
  (migration 016, repository), `tst_profilepagesync` (two real peers),
  `tst_outboxretention`, `tst_profilesong`, `tst_profilerender`,
  `tst_profilecontroller`, `tst_profilepageqml`, `tst_profileeditor`, and the
  profile sections of `tst_qmlload` (every picture site, Escape, focus, unread
  state, dialogs, Plain style, the minimum window).
- `OPENCHAT_PROFILE_CAPTURES=<dir>` makes the QML suites save review PNGs.
- `OpenChat --profile <preset-slug> --capture <png>` opens a reference page
  and exits non-zero unless it is really on screen (`capture_profile`).
- `openchat-profile-gallery --page profile-pages` renders every preset, the
  editor and a stub through the real window, light and dark.

## Limits and follow-ups

- Profiles are for 1:1 contacts; group members who are not contacts get
  stubs, since no channel carries a page to them.
- Non-WAV song import depends on the platform's Qt Multimedia backend (FFmpeg
  on Linux, Media Foundation on Windows); it was only exercised on Linux.
- Reduced motion is honoured only where Qt reports it (Windows); Low memory
  mode stills animation everywhere.
- The Layout tab reorders on drop rather than live during a drag.
