# Case drops

The small case icon beside the add-contact control opens a native Qt Quick popup.
Each tile shows a collectible cosmetic with its rarity tier, and opening a case
draws one of them with the published odds. Cases drop while OpenChat is open and
connected: one for every half hour of connected time (`caseDropIntervalMs`), so
two an hour, and none while it is closed. Unopened cases wait and stack; each
open takes one. A new account starts with one waiting. The popup counts the
waiting cases and says when the next drops if OpenChat stays open; the icon's
dot shows while any wait, and its tooltip gives the count or the time. The
drawn item joins the account's collection, and only collected items can be worn
(Settings → Cosmetics). What an account wears is seen by everyone it chats with.
There is no payment and no monetary value.

The case started out daily, which is why its classes, files and settings keys
still carry `DailyCase` names.

## The relay is the authority

Signed in, the relay holds everything (relay migration 007,
`relay/src/CosmeticsService`): each account's waiting cases and connected time
toward the next, every case opened, what it owns and what it wears. It is the
one deliberate exception to the relay storing only ciphertext and routing
material -- none of it is message content, just catalogue ids, counters and
times -- and the migration says so.

- **Connected time.** `RelayServer` counts the time an account has a `/v1/live`
  socket open, on a monotonic clock, and credits it when a case is due (one
  precise single-shot timer per online account), when the last socket closes
  and when the relay shuts down. Nothing a client reports counts. Each new case
  is pushed to the account's opted-in sockets (`?cosmetics=1`) as frame
  `[14, state]`; clients that do not opt in never see the frame.
- **Claims.** `POST /v1/cosmetics/claim { request_id }` consumes one waiting
  case, draws the reward with the shared odds (`domain/CosmeticRules`, so the
  relay and every client name the same items) and grants it, in one
  transaction holding the account's case row. The request id (the client keeps
  it until an answer arrives) makes a retry after a lost response return the
  same case rather than open a second. With nothing waiting a new claim is 409.
- **Wearing.** `POST /v1/cosmetics/equip { slot, item_id }` wears an owned item
  in its own slot (an empty id clears the slot); anything else is refused.
- **Seeing others.** `POST /v1/cosmetics/loadouts { accounts }` (at most 256)
  returns what those accounts wear, to any signed-in caller, like the directory:
  the relay has no notion of contacts, so it cannot limit this to them.
- **State.** `GET /v1/cosmetics` returns `{ drops, progress_ms, interval_ms,
  next_case_key, owned, loadout, imported, last? }`; every other route answers
  with it too.
- **Import.** `POST /v1/cosmetics/import { owned, drops }` hands over a
  collection a device kept before the relay held it: the items this build
  knows, and up to ten waiting cases (never fewer than the account has). Only
  the first import counts.
- **Operators.** `openchat-relay grant-cases <n> --all` (or `<handle>`) adds
  waiting cases against the same database; clients see them the next time they
  ask (opening the popup, reconnecting). In the Docker deployment:
  `docker compose ... exec relay openchat-relay grant-cases 5 --all`.

On the client, `RelayCaseService` (`src/case`) is the authority once signed in;
`AppRuntime` installs it as `DailyCaseController`'s service factory. It asks
the relay, hears frame 14, and on the first state for an account the relay has
not imported, hands over what `LocalDailyCaseService` kept on this device. If
the relay answers 404 (not redeployed yet) it carries on with the local
stand-in as before, so clients can ship before the relay. The authority
interface is asynchronous (`DailyCaseService`, answering through a callback),
and `DailyCaseController` drops answers for an account it no longer shows. A
popup closed while a claim is on its way lands on the result without a spin.

## Running time for the local stand-in

Previews, tests and a relay without the cosmetics routes use
`LocalDailyCaseService`, which is told the running time instead of counting it.
`DailyCaseController` counts running time for its account on a monotonic clock
(`QElapsedTimer`) and reports it with `accrue(ms)`: when the next drop is due
(one single-shot timer, nothing polls), when the account changes, when the
application is about to quit, and when the controller is destroyed. Time toward
the next drop is kept across restarts; time with the app closed is never
reported, so it earns nothing. One report credits at most what the next drop
still needs, so a machine left asleep with OpenChat open earns the one drop
that was due, not a stack. A report that fails is retried a minute later with
its time. A crash loses the running time since the last report (under half an
hour).

## Ownership and integration

`DailyCaseController` belongs to the main window, independently of the popup's
lifetime. `DailyCaseModal` and its 80 lightweight tiles are loaded on demand and
destroyed when closed. The controller has Available (a case waits), Opening,
and Opened (the last case opened is on show) states; its Opening guard is set
before entering the claim service. Closing a spin stops the animation and
audio immediately and keeps the recorded result. A fresh reveal stays on show
until the popup closes or "Open next case" starts the next waiting one; with
none waiting, reopening displays the last result without replaying its impact.

`LocalDailyCaseService` is the **local stand-in authority**. It records the
waiting drops, the running time toward the next, and the last claim's ID,
reward, presentation seed and case key in an atomic JSON file under the
application's local data directory, keyed by a hash of the account ID. A
nonblocking per-account process lock serializes status/claim/accrue
transactions. A claim with no case waiting returns the last result. Different
local accounts are independent. Preview windows use a separate `preview` key.
Corrupt/unwritable files fail closed with a retryable UI error. A claim saved
by a build with a cooldown (daily or hourly) leaves one case waiting if it was
already due, and none if not.

Every reward the mock hands out goes into `LocalCosmeticInventory`: one atomic
`<hash>.owned.json` per account beside the claim, listing catalogue ids in the
order they arrived. The claim is saved first and the grant second; each
transaction hands a stored claim's reward over if the collection lacks it, so a
failed grant is retried rather than lost, and claims saved before collections
existed still count. An unreadable collection fails closed like a corrupt claim.
Every reply carries the collection (`CaseReply::owned`), exposed as
`DailyCaseController.owned` once known.

The stand-in uses local storage and believes whatever running time the client
reports, so it is **not secure eligibility** or a secure inventory: deleting or
editing its files, reporting time that never ran, or running two copies can
bypass it. That is why it is used only where nobody else sees the result, and
why the relay imports from it once, capped, and never again.

On either authority the seed controls only the visual arrangement; it never
selects the reward on the client. `adopt()` is where the predetermined result
enters the presentation.

## Rewards and rarity

`CosmeticCatalog` (`src/cosmetics/`) lists every collectible (bubble skins,
avatar frames, presence beads, name flair, profile scenes) with a stable id and
one of five tiers. The ladder, each item's slot and tier, and the draw live in
one table in `src/domain/CosmeticRules.cpp`, which the relay compiles too; the
client's `CosmeticCatalog` adds names, descriptions and colours:

| Tier | Colour | Odds | Items |
|---|---|---|---|
| Common | blue `#4b69ff` | 60% | 8 |
| Rare | purple `#8847ff` | 25% | 7 |
| Epic | pink `#d32ce6` | 10% | 7 |
| Legendary | red `#eb4b4b` | 4% | 5 |
| Exotic | gold `#e4ae39` | 1% | 2 |

A draw rolls a tier by weight (parts per thousand), then picks one of its items
evenly, so an item's chance is its tier's share over the tier's size.
`CosmeticCatalog::draw()` is deterministic in its two rolls; the caller supplies
the randomness. Small or plain items are Common; the larger, more detailed or
animated an item, the higher it sits, and animated items are never below
Legendary. `tst_cosmetics` pins the tier sizes so a new item gets its tier on
purpose.

The local mock draws at claim time and stores the id beside the claim; a claim
saved before rewards existed replays as `placeholder` and shows the "?" tile.
The belt around the winner is the case's: `DailyCaseController.fillers` is
drawn with the same odds from a hash of the account and the reply's case key.
The service names the case on offer after the last claim ("first" before any)
and stores that key with the claim, so the belt does not change when the claim
lands, shows again around a replayed result, stays the same in every window and
across restarts, and changes when the next waiting case is shown. The seed
still only chooses which of five slots the winner occupies.

Tiles draw each item through the component that wears it (`CosmeticPreview`),
with the tier's colour as a bottom bar, a glow and a label. The popup always
shows the odds. On reveal, the ring, selector and winner border take the tier
colour, and the ring thickens with the tier; Legendary and Exotic add a flash
behind the belt and a second, later ring. Less motion skips all of it.
`openchat-profile-gallery --page rarity-ladder` and `--page case-reveals`
render the ladder and frozen reveals for review.

## Equipping

Settings → Cosmetics has one picker per kind (avatar frame, name flair,
presence bead, profile scene, chat bubble). Each is a grid of tiles: None
with the stock look first, then every item of the kind from Common to Exotic,
drawn by `CosmeticPreview` over the same tier bar and glow as the case tiles.
A click, Space or Enter equips the tile at once through `AppearanceSettings`,
which remembers it; the sidebar header wears it while Settings is still open.
Animated items carry a play mark and move while pointed at or focused. Arrow
keys walk the grid and Tab stops once per kind, on its equipped tile.

Only collected items can be worn. `Main.qml` hands the case controller's
`owned` list to `AppearanceSettings.ownedCosmetics` once it is known, and
`AppearanceSettings` enforces it: nothing is worn until ownership is known,
an equipped id the account does not own is taken off and forgotten, and
equipping one it does not own is refused (whoever asks). An unknown collection
(a failed reply for a new account) hands over nothing rather than an empty
list. In the picker, items not yet unboxed stay in the grid, dimmed under a
padlock, with a tooltip pointing at the case drops; each section counts
"N of M unboxed", and a newly unboxed item unlocks on the open page.

What is worn follows the account: signed in, `DailyCaseController.loadout`
carries the relay's loadout, which `Main.qml` hands to
`AppearanceSettings.loadout` (adopted as it is, without asking the relay for
anything), and whatever is equipped here leaves through
`AppearanceSettings.equipRequested` for `DailyCaseController.equip()` to send.
If the relay refuses, the loadout it holds is what is worn. The local stand-in
keeps the loadout on the device instead.

## Seeing what others wear

`ChatController` asks the relay for the loadouts of every contact and group
member when it connects, when the roster changes (coalesced) and every five
minutes, and hands them to `PeerCosmetics`: one process-wide object, shared by
every QML engine as the `PeerCosmetics` singleton, that only ever hands out ids
this build knows in the slot they belong to. Surfaces bind
`PeerCosmetics.item(accountHex, slot)` with a dependency on
`PeerCosmetics.revision`:

- a contact's row wears their frame and bead;
- the conversation header wears their frame, bead, flair (inking the name,
  which keeps its own width and elision) and scene (with frosted patches behind
  the name and the call buttons);
- their incoming bubbles wear their skin (messages carry the sender's account,
  `senderAccount`, for one-to-one chats and groups alike);
- a one-to-one call's remote tile wears their frame (the call's chat id is
  their account).

A group itself, and a group call's tiles, wear nothing of anyone's yet.

## Motion and sound

`CaseMotion.h` centralizes duration, travel, sequence bounds, easing, crossing
selection, and tick limiting. The motion integrates a short acceleration ramp
and quadratic velocity falloff. It reaches peak speed after 252 ms, then slows
over the remainder of 7.2 seconds. The last 20% moves roughly 0.4 tiles. Its
endpoint is the exact integer winner index (56–60), never a rounded frame or a
random physics stop. Qt's [animation framework](https://doc.qt.io/qt-6/qvariantanimation.html)
supplies elapsed progress; easing is independent of frame frequency.

The belt origin is `viewportWidth/2 - tileWidth/2 - position*stride`. Thus the
winning tile center is always `viewportWidth/2` at rest, including after a
resize or card-size breakpoint. Eight leading and at least nineteen trailing
tiles cover the popup's maximum width. Only the belt position changes during
the spin; no geometry is queried in the animation callback.

A crossing occurs when `floor(position + 0.5)` changes: a new tile enters the
selector. The same event triggers the bundled Counter-Strike crate scroll
sample. Stalled frames coalesce into one current tick; sound is bounded to one
voice and a 28 ms minimum interval. A retrigger fades the outgoing sample
across a 3 ms tail, mixed with saturation instead of a hard cut, because
chopping the busy scroll recording mid-rattle clicks. The crate open sample
and the 650 ms expanding ring fire once at normal completion. No effect fires
again on reopening; the popup's display sample, however, plays every time the
case is shown. Audio releases after the open sample finishes, or immediately
on dismissal or spin start. A mutex protects the single sample stream if the
audio backend pulls on another thread.

The three crate samples (`crate-display`, `crate-item-scroll`, `crate-open`
under `assets/sounds`, from CS2's crate UI) are bundled through the QML
module's resources, decoded with the defensive `WavFile` reader, and linearly
resampled and channel-mapped to the output device's preferred S16 format. They
play at half scale so a UI flourish cannot shout over an ongoing call, and the
display sample obeys the same mute preference as the rest. A missing sample or
no audio device degrades to silence.

Sound and Less motion preferences persist. Less motion skips the conveyor and
animated impact and reveals the recorded tile immediately. Qt does not expose
a uniform cross-platform reduced-motion preference here, so the control is
explicit. The popup uses Qt Controls' modal overlay, Escape dismissal, keyboard
navigation, and focus restoration. The app's existing desktop minimum remains
720 px; the popup itself also supports narrower embedding.

## Verification

Build `OpenChat`, `tst_dailycase`, and `tst_qmlload`. Run:

```sh
ctest --test-dir Build/daily-case -R 'tst_dailycase|tst_qmlload' --output-on-failure
```

To capture the actual interaction in the QML test, set
`OPENCHAT_CASE_CAPTURES` to an existing directory and run
`Build/daily-case/tst_qmlload dailyCaseInteraction`. It saves available, opened,
and narrow-window PNGs in light and dark themes. Tests cover persistence, account isolation, lock/error
behavior, duplicate requests, reduced motion, stopping on dismissal, the audio
stream, curve behavior at 30/60/144 Hz, full-duration opening, selector and belt
bounds each frame, resizing, exactly one reveal, focus restoration, reopening,
and QML warnings. Existing QML tests exercise the rest of the chat UI.

On this checkout with Qt 6.11.2, the existing
`bottomNavigationIconsStayAboveLabels` test fails for the Call icon's spacing.
The same failure is reproducible with the original main-worktree test binary;
the daily-case change does not modify that navigation layout.

Offscreen tests establish behavior and geometry, not hardware GPU frame rate
or perceived loudness. Audition the default sound level on the target desktop
and profile the native renderer before promising a minimum FPS on all devices.
