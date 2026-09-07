# OpenChat Quality-of-Life & Gap-Closure Plan

**Date:** 2026-09-05
**Goal:** Close the gap between the shipped-looking OpenChat interface and a chat client someone can actually live in — fix the bugs that are visible today, then land the missing everyday affordances in dependency order.

**Method used to produce this:** read the full source tree, ran the test suite (`ctest`, 34/35 pass), and captured every reachable surface offscreen (`--capture`, plus a temporary nav-section hook that was reverted) — default chat, add-contact, verify, call, incoming call, onboarding, onboarding-recovery, Call section, Settings section.

---

## Where the client stands

The foundations are strong and mostly done: MLS group crypto, canonical CBOR envelopes, SQLCipher storage with an outbox, a real relay with auth/directory/invites/key-packages, a durable SyncEngine, voice calls with Opus + jitter buffer + SRTP-style media crypto, friend requests, and safety-number verification. The Aero rendering is polished and pixel-tested.

What is missing is almost entirely the **last mile**: the layer between working machinery and a person using it. Several complete C++ subsystems exist with no UI attached, and several UI affordances exist with no C++ attached.

---

## Findings

### A. Broken today (visible defects)

| # | Defect | Evidence |
|---|---|---|
| A1 | `tst_qmlload::bottomNavigationIconsStayAboveLabels` fails — the Call icon's bottom edge lands exactly on the label's top edge. | `tests/tst_qmlload.cpp:145`; icon box `y:10 h:26` vs label `bottomMargin:10` in a 64 px nav (`ContactSidebar.qml`) |
| A2 | Live users permanently see a red **"1"** missed-call badge that means nothing. | `ChatController::callMissedCount()` returns a hardcoded `1` (`src/controllers/ChatController.cpp:376`), and it is a `CONSTANT` property, so it never clears |
| A3 | The composer draft is a single global string — switching chats carries your half-typed message into someone else's conversation. | `m_composerText` in `ChatController`; `selectContact()` (`ChatController.cpp:251`) never swaps it |
| A4 | Three visible controls are inert (no `onClicked` at all): the composer's chevron button, the video-call button, the conversation-header ⌄ menu. | `Composer.qml:96`, `ConversationHeader.qml:99`, `ConversationHeader.qml:157` |
| A5 | The whole connection/security banner is dead code in the live app — `setSessionState()` has no caller outside the controller, so OpenChat never says "Offline" or "Reconnecting". | grep: only declaration at `ChatController.h:116` |
| A6 | Delivery state is computed, stored, and pushed into the model — and then never rendered. `MessageDelegate` doesn't even declare the role. | `MessageListModel::DeliveryStateRole` vs `MessageDelegate.qml` required properties |
| A7 | `SyncEngine::acknowledgeRead()` has no caller anywhere in the client, so the `Read` state can never be reached. | `src/network/SyncEngine.cpp:572` |
| A8 | A `Failed` send is a terminal dead end — no indication, no retry. | `MessageFailureReason` is set in `ChatController::onMessageStateChanged` and dropped on the floor |

### B. Missing everyday affordances

- **Conversation:** no scrollbar, no jump-to-latest, no unread divider, no "load older" (history is a hard 200-row page, `historyPageSize`), no text selection or copy, no message context menu, no clickable links, no in-conversation search, no typing indicator, no emoji picker (`MessageKind::Emoji` renders large but this client can never *send* one — `enqueueText` is always `ContentKind::Text`).
- **Roster:** no per-contact unread badge (`ContactListModel` has no unread role), no last-message preview, no recency sorting, no timestamp, no hover state, no right-click actions (verify / block / remove) even though `ContactRequestService::blockContact` and `ContactRepository::block` exist.
- **Presence:** hardwired. `contactRowFor()` sets `Presence::Offline` with the comment *"no presence exchange exists yet"* (`ChatController.cpp:538`); the row subtitle falls back to the `@handle`.
- **Notifications:** none. No tray icon, no desktop notification, no unread count in the window title, no message sound — `assets/sounds/` is call-only (`CallSounds.cpp`).
- **Settings:** 7 categories × 4 rows = 28 rows, all inert stubs with a muted chevron (`ChatController.cpp:62-89`, `Main.qml` settings pane). Nothing persists at all — there is no `QSettings` in the tree, so window geometry, selected chat and every preference reset on launch.
- **Identity:** avatars are a fixed set of built-in resource keys (`AvatarArtwork.cpp`); every live contact and the local user get `userpfp_none`. Display name is seeded from `$USER` "until profile editing is available" (`main.cpp:250`) and can never be changed in-app.
- **Recovery:** the recovery code is generated, shown once, and then unusable — there is no redeem/restore path anywhere in the client.
- **Profiles/devices:** `findExistingProfile()` returns the *first* profile directory it finds. No profile switching, no sign-out, no second-device linking UI despite `DeviceLink` existing.
- **Calls:** Call section is a blank pane and the sidebar says "No calls yet." unconditionally — no call history model, no per-call records. Video is a button and nothing else.
- **Attachments:** the relay has an `attachments` table and the protocol has an `AttachmentControl` envelope type — but the relay exposes no attachment routes and the client has no attachment code beyond the ID type.
- **Groups:** 1:1 only; `ConversationId` is derived per contact.
- **Keyboard/accessibility:** the only key handler in the entire QML tree is Enter-to-send. No shortcuts, no focus ring, no tab order, no `Accessible` properties.

---

## Plan

Ordered so each phase stands on its own and the earliest phases remove the things a first-time user notices first.

### Phase 0 — Stop the bleeding (bugs only, no new surface)

- [ ] **0.1** Fix the bottom-nav overlap: give `NavigationIcon` a `y: 8` / 24 px box or raise the label's `bottomMargin` to 12, whichever keeps the capture diff smallest. Re-run `tst_qmlload` and the capture smoke tests.
- [ ] **0.2** Make `callMissedCount` real: `NOTIFY`-backed, `0` in live mode until Phase 6 lands call history; keep the mock value only when `!isLive()` so the reference capture is byte-identical.
- [ ] **0.3** Per-conversation drafts: move `m_composerText` into `LiveChat` (and the mock map), save on `selectContact()` and restore on open. Test in `tst_chatcontroller`.
- [ ] **0.4** Neutralise the three inert controls — either wire them (Phase 1.4 emoji, Phase 6.3 video, 0.5 menu) or visibly disable them. Nothing clickable should do nothing.
- [ ] **0.5** Conversation-header ⌄ menu: a real menu with *Verify safety number* (already implemented — `ContactController::openSafetyNumber`), *Block*, *Remove contact*.
- [ ] **0.6** Wire `setSessionState()` to reality: connect `RelayClient::transportError` / connect-disconnect and the `DeviceLink` auth state into `Offline` / `Reconnecting`, and vault lock into `Locked`. The banner already exists and is already tested — it just needs an input. Add a **Retry now** action in the banner.

### Phase 1 — The conversation itself

- [ ] **1.1** Delivery ticks: add `deliveryState` / `failureReason` to `MessageDelegate`'s required properties and render Queued/Sending/Sent/Delivered/Read/Failed as a small glyph beside the timestamp on outgoing bubbles only.
- [ ] **1.2** Failed-send recovery: a **Retry** affordance on failed bubbles calling a new `ChatController::retry(stableId)` → outbox re-enqueue.
- [ ] **1.3** Read receipts: call `SyncEngine::acknowledgeRead()` when a conversation is open *and* the window is active (`Window.active`), gated by the Privacy → Read receipts setting from Phase 4.
- [ ] **1.4** Emoji: a picker behind the composer's chevron, plus `ContentKind::Emoji` on send when the body is emoji-only, so the existing large-emoji rendering path becomes reachable.
- [ ] **1.5** History navigation: `ScrollBar`, a jump-to-latest pill when scrolled up, an unread divider on open, and paging older messages via the `before` cursor `ChatRepository::messages()` already takes.
- [ ] **1.6** Message interaction: selectable text, a context menu (Copy / Copy timestamp / Delete for me / Retry), and link detection with confirm-before-open.
- [ ] **1.7** Typing indicator: a new lightweight envelope kind, TTL-bounded, off by default and gated by the Privacy setting.

### Phase 2 — The roster

- [ ] **2.1** Add `unread`, `lastMessagePreview`, `lastActivityMs` roles to `ContactListModel`; render a per-row badge, a one-line preview and a relative timestamp in `ContactRow`.
- [ ] **2.2** Sort chats by last activity, with a stable tiebreak.
- [ ] **2.3** Row hover state and a right-click menu (Verify / Block / Remove), reusing the Phase 0.5 actions.
- [ ] **2.4** An unblock surface (Settings → Privacy → Blocked contacts) so blocking is not one-way.

### Phase 3 — Presence & being told about things

- [ ] **3.1** Presence exchange: publish Available/Away/Offline over the existing envelope path (Away driven by idle time), apply to `ContactRow`, `ConversationHeader` and the local user bead. Removes the hardcoded `Presence::Offline`.
- [ ] **3.2** Desktop notifications for messages, friend requests and incoming calls when the window is not active; click focuses the conversation.
- [ ] **3.3** Tray icon with unread count, minimise-to-tray, and a real "On close, keep running".
- [ ] **3.4** Unread count in the window title.
- [ ] **3.5** A message sound (render one alongside the existing call sounds with `tools/render_call_sounds.cpp`), respecting Do Not Disturb.

### Phase 4 — Settings that do something

- [ ] **4.1** A `Preferences` service over `QSettings` (or a settings table in the profile DB for anything privacy-sensitive), plus window geometry and last-open-chat restore.
- [ ] **4.2** Replace the stub row with real controls — toggle, combo, disclosure — driven by a typed settings model rather than a `QStringList`.
- [ ] **4.3** Implement, category by category: General (launch on startup, close behaviour, taskbar), Account & Profile (display name, presence, picture, sign out), Privacy (read receipts, typing, blocked list, who can contact me), Notifications (per-type, sounds, DND), Audio & Video (device pickers — `QtAudioIo` already enumerates), Appearance (theme, chat font size, compact rows), About (version, licenses).
- [ ] **4.4** Dark theme: `Theme.qml` is already the single source of colour truth, so this is a palette swap plus an Appearance toggle.

### Phase 5 — Identity

- [ ] **5.1** Editable display name and handle-aware profile screen; drop the `$USER` seed hack.
- [ ] **5.2** Real avatars: pick an image, store it in the profile DB, exchange it as an encrypted profile attribute, and extend `AvatarArtwork` to render a stored image with the existing rounded mask and neutral fallback.
- [ ] **5.3** Recovery-code restore: an "I already have an account" path from onboarding that redeems the code, re-derives the vault key and re-registers the device. This is the single biggest data-loss risk today.
- [ ] **5.4** Multiple profiles: a profile picker when more than one exists, plus sign-out and delete-profile.

### Phase 6 — Calls

- [ ] **6.1** A durable `CallRecord` store and `CallListModel`; fill the empty Call section with history (direction, peer, duration, outcome) and drive the missed badge from it.
- [ ] **6.2** Call back / call again from a history row.
- [ ] **6.3** Video calls, or an honestly disabled video button with a tooltip until then.
- [ ] **6.4** Incoming-call handling while the user is in the Call or Settings section, and while the window is minimised (tray + notification + ringtone).
- [ ] **6.5** Device selection during a call, and a "no microphone" explanation instead of a dimmed button.

### Phase 7 — Attachments

- [ ] **7.1** Relay: `POST /v1/attachments` (metadata + upload), `GET /v1/attachments/:id`, retention sweep. The schema in `003_inboxes_attachments.sql` already anticipates this.
- [ ] **7.2** Client: chunked encrypt-then-upload, `AttachmentControl` envelope, download with hash verification.
- [ ] **7.3** UI: drag-and-drop and a file button on the composer, an image/file bubble with progress, save-as, and a size cap with a clear error.

### Phase 8 — Keyboard & accessibility

- [ ] **8.1** Shortcuts: `Ctrl+F` search, `Ctrl+,` settings, `Esc` closes overlays, `Ctrl+1/2/3` sections, `Alt+↑/↓` previous/next chat.
- [ ] **8.2** Visible focus rings and a sane tab order across sidebar → search → history → composer.
- [ ] **8.3** `Accessible.role` / `Accessible.name` on every interactive element; screen-reader pass.
- [ ] **8.4** Honour the system font-scale factor and verify the layout at 125% / 150%.

---

## Suggested order if the goal is "usable daily, soonest"

Phase 0 → 1.1–1.3 → 2.1–2.2 → 3.2–3.4 → 4.1 → 5.3. That sequence removes every visible defect, makes messages legible as *delivered*, makes the roster informative, tells you when something arrives, remembers your preferences, and stops a lost machine from meaning a lost account. Everything after that is enrichment.

## Test strategy

Every phase extends the existing harnesses rather than adding new ones: `tst_chatcontroller` for controller behaviour, `tst_qmlload` for structure and layout invariants, the `capture_*` smoke tests for the approved rendering, `tst_syncengine` / `tst_relayclient` for anything crossing the wire, and `tst_e2e` for the two-client paths (presence, receipts, attachments). The reference chat capture must stay byte-identical through Phase 0–2 — every new affordance is added behind a live-mode check or in a state the capture does not enter.
