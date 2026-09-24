# Custom profile panels

Users can add their own panels to their profile page: "Add new panel" makes a
box they fill with text, pictures, video clips, lists (favourite games,
top 5s, …) and dividers. It gets its own title, icon and colours, and they
place it anywhere in the Layout like the built-in boxes.

This builds on [profile-pages.md](profile-pages.md) and uses the same
principles: typed knobs only (no markup, URLs or scripts), everything
normalised on arrival, and the renderer owns readability.

## Compatibility promises

* **Existing profiles are untouched.** A page with no panels encodes to the
  same bytes as before: the new core key is written only when the page has
  panels, and the module list only carries panel entries when there are
  some. Migration 017 only adds tables and widens two kind CHECKs. It copies
  every existing row and rewrites none of them.
* **Old clients (0.2.9) keep working.** Every new field is additive and uses
  a key an old decoder ignores:
  * PageCore key 10 (`panels`) is ignored.
  * Layout entries for `CustomPanelModule` (7) are unknown modules, and old
    `normalized()` drops them. There are at most 10 panels, so a list of
    6 built-in modules plus panels never exceeds the 16 entries an old
    decoder looks at.
  * Panel media travels as new media kinds (3 panel picture, 4 video
    segment). `classifyProfilePayload` on an old client calls these
    `UnknownPage` and ignores them.
  * Page requests ask for at most 6 blobs (≤ 256 bytes, the old request
    cap). An old owner keeps the first two.
  * The one visible limit: an old client refuses cores over 24 KiB. A page
    with a lot of panel text can go past that, and such a page reaches an
    updated client only. After the update, every contact's stored page gets
    one "refresh" request (see below), so a core an old version dropped is
    fetched once.
* **Wire version stays 1.** Nothing here needs a version bump.

## Model (`domain/ProfilePage.h`)

```
Page.panels : QVector<Panel>             ≤ 10
Panel  { id 1…255, title, icon, look, blocks ≤ 12 }
PanelLook { ownColours, headerFill, headerText, boxFill, bodyInk, showTitle }
Block  { id 1…65535 (unique per page), kind, … fields per kind }
  TextBlock     text ≤ 2000, textStyle (paragraph/heading/quote/callout), align
  ImageBlock    images ≤ 6 {ref, caption ≤ 120}, gallery (grid/stack/strip), frame (plain/rounded/polaroid/circle)
  VideoBlock    clip {segments ≤ 6, poster}, caption ≤ 120, loop
  ListBlock     listStyle (bullets/numbers/games/hearts), items ≤ 20 {title ≤ 80, detail ≤ 80, rating 0…5, status, cover}
  DividerBlock  dividerStyle (line/dots/stars/hearts/space)
ModulePlacement.panel : quint8           the panel a CustomPanelModule entry places
```

Page-wide budgets, applied by `normalized()` in document order, so they
are deterministic and idempotent:

* **panelTextBudget** 16 000 UTF-16 units across every panel string. A
  string past the budget is cut at a cluster boundary, and later ones
  become empty.
* **maxPanelMedia** 24 refs across all panels (pictures, covers, posters
  and segments).
* **maxPanelMediaBytes** 4 MiB across those refs. A ref past either budget
  is dropped.
* Total blocks ≤ 40, total list items ≤ 120.

A worst-case page encodes to under `maxPageCoreBytes` (raised to 96 KiB);
`tst_profilepage` checks this.

`normalized()` also repairs ids (zero or duplicate ids get the next free
one, in order), drops placements that name no panel, keeps the first
placement of each panel, and appends unplaced panels to the wide column.

## Media

| kind | what | checks on arrival |
|------|------|-------------------|
| 1 | background JPEG | as before |
| 2 | song (Opus container) | as before |
| 3 | panel picture / cover / poster (JPEG ≤ 1280 px, ≤ 224 KiB) | JPEG walk, 1–32 scans |
| 4 | video segment (`ClipContainer`, VP9 + Opus, ≤ 224 KiB, ≤ 8 s) | container parse |

A video is up to 6 segments, and every segment plays on its own (it starts
on a keyframe and carries its own audio), so a clip survives the 224 KiB
per-message cap without a new transport.

Storage: `profile_media` and `contact_page_media` accept kinds 1–15.
`local_page_panel_media(profile_id, stage, sha256, kind)` lists the refs of
the draft (stage 0) and the published page (stage 1).
`contact_page_panel_media(account_id, sha256, kind)` lists the refs the
stored contact core names. Two views (`local_page_media_named`,
`contact_page_media_named`) combine them with the old slot columns, and
every "is this blob named" query reads a view.

## Sync

Everything that went through the "published blobs" (background, song) now
goes through `Profile::mediaRefs(page)`, which lists every ref with its
kind. Panel media is delivered after the core, like the background and
song. The pending-media bound per contact rises to 26.

Refresh after upgrade: `contact_pages.format` (added by 017, default 1).
The first start that finds format-1 rows schedules one jittered
`Refresh` request per such contact (with `haveRevision`). The owner answers
only if their revision differs. The row becomes format 2 once the request
is queued.

## UI

* The editor has a **Panels** tab: the panel list with **Add new panel**
  and templates (Blank, Text, Photo album, Favourite games, Video). Each
  panel expands into its block editor, where blocks can be added,
  reordered, duplicated and removed, and panels can be renamed, recoloured
  and have their icon changed.
* The page preview in the editor ends the wide column with a dashed
  "+ Add new panel" tile.
* The Layout tab lists panels as rows beside the built-in modules (move
  column, reorder, hide).
* Viewing: `ProfilePanelModule` renders the blocks. Pictures open in the
  zoom overlay, and videos show their poster until played.
