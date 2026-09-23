# Daily case opening

The small case icon beside the add-contact control opens a native Qt Quick popup.
Each tile shows a collectible cosmetic with its rarity tier, and the claim draws
one of them with the published odds. Nothing is granted yet: there is no
inventory or equip flow behind the reveal. There is no payment and no monetary
value.

## Ownership and integration

`DailyCaseController` belongs to the main window, independently of the popup's
lifetime. `DailyCaseModal` and its 80 lightweight tiles are loaded on demand and
destroyed when closed. The controller has Available, Opening, and OpenedToday
states; its Opening guard is set before entering the claim service. Closing a
spin stops the animation and audio immediately and keeps the recorded result.
Reopening displays the result without replaying its impact.

`LocalDailyCaseService` is a **temporary local mock authority**. It records a
claim ID, presentation seed, and next UTC midnight in an atomic JSON file under
the application's local data directory, keyed by a hash of the account ID. A
nonblocking per-account process lock serializes status/claim transactions.
Repeated claims return the existing result. Different local accounts are
independent. Preview windows use a separate `preview` key. Corrupt/unwritable
files fail closed with a retryable UI error. The midnight refresh is a single
shot timer; there is no polling while closed.

This mock uses the local clock and local storage, and is **not secure eligibility**.
Deleting its files, changing the clock, or using another device can bypass it.
No actual reward may be granted using this mock.

For the online implementation, use the existing authenticated relay/account
conventions. The relay currently has no daily-case endpoint. Add a server
transaction with a unique `(account_id, server_day)` claim constraint. Verify
eligibility, choose the reward, persist consumption, and return the persisted
claim on all retries, including after a lost response. Return at least
`claimId`, `rewardId`, `seed`, and `nextAvailableAt`. The seed controls only the
visual arrangement; it must never select the actual reward on the client.
`rewardId` is a cosmetic catalogue id; the server draws it with the tier odds
below. `adopt()` is where the predetermined result enters the presentation. Implement the network service asynchronously, routing its
completion into the controller's claim-result handling; never block the GUI
thread with a network wait. Preserve the Opening guard during the request and
reconcile an uncertain response using the server's existing claim.

## Rewards and rarity

`CosmeticCatalog` (`src/cosmetics/`) lists every collectible (bubble skins,
avatar frames, presence beads, name flair, profile scenes) with a stable id and
one of five tiers. The ladder and each item's tier live in one table in
`CosmeticCatalog.cpp`:

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
The belt around the winner is the day's: `DailyCaseController.fillers` is drawn
with the same odds from a hash of the account and the UTC date. It does not
change when the claim lands, so nothing on screen swaps as the spin starts, and
the seed still only chooses which of five slots the winner occupies.

Tiles draw each item through the component that wears it (`CosmeticPreview`),
with the tier's colour as a bottom bar, a glow and a label. The popup always
shows the odds. On reveal, the ring, selector and winner border take the tier
colour, and the ring thickens with the tier; Legendary and Exotic add a flash
behind the belt and a second, later ring. Less motion skips all of it.
`openchat-profile-gallery --page rarity-ladder` and `--page case-reveals`
render the ladder and frozen reveals for review.

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
