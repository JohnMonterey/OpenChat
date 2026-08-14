# ClearChat Interface Design

## Objective

Build a cross-platform C++ desktop chat prototype that faithfully reproduces the supplied ClearChat reference. The application uses Windows 7 Aero visual language and a compact two-pane contacts/conversation layout. It is a local interactive prototype: it does not implement accounts, networking, persistence, voice, or video.

The supplied PNG is the visual source of truth. The earlier NIMBI brand and Discord-like multi-rail layout are out of scope and must not influence the implementation.

## Product Scope

The prototype supports:

- a frameless Aero-style application window with minimize, maximize/restore, close, drag, and resize behavior;
- the ClearChat title and compact application icon;
- current-user presence and status;
- Favorites and Contacts categories;
- contact selection and local search filtering;
- a conversation header with selected-contact identity and call controls;
- a local seeded conversation;
- a message composer whose Send button follows input state;
- appending a sent message to the active local conversation;
- resizing down to a usable minimum without clipping the core panes.

The prototype does not support:

- real authentication, servers, sockets, delivery, synchronization, or message persistence;
- real voice or video calls;
- editable profiles, contact creation, or settings screens;
- platform-native Windows 7 theme APIs. The Aero appearance is painted by the app so it remains consistent on Windows, macOS, and Linux.

## Visual Contract

### Window and Geometry

- The reference logical window size is 860 × 680 pixels, with a 720 × 560 minimum.
- The outer frame uses a translucent blue Aero border, a 45-pixel title bar, a 10-pixel content inset, fine white inner highlights, and a cool blue drop shadow.
- The content uses a two-pane split. The contact pane is 270 logical pixels at the reference size; the conversation pane consumes the remaining width.
- The title bar uses Segoe UI where available. Segoe UI is installed in the development environment; the runtime falls back to Noto Sans on systems without it.
- All dimensions, colors, borders, and gradients derive from centralized QML tokens. Components must not invent local variants.

### Contact Pane

- Current-user identity, contact search, grouped contact list, and bottom actions form four vertical regions.
- Favorites and Contacts are the only section dividers.
- Contact rows have no spacer lines or borders between individual contacts.
- The selected contact uses the reference's pale horizontal blue wash.
- Presence uses small green, amber, and gray glossy status beads.
- Contact thumbnails are self-contained assets and may be replaced later without layout changes.

### Conversation Pane

- The selected-contact header uses a 68-pixel avatar, name, status, and right-aligned video, phone, and dropdown controls.
- Video and phone controls are proper vector silhouettes, not font glyphs. They remain replaceable SVG resources because the user may provide final SVGs.
- The conversation canvas is almost white with a subtle cool top wash and a centered date rule.
- Incoming messages align left; outgoing messages align right.
- Message bubbles use a single painted outline and fill path that combines the rounded rectangle and tail. The tail is never a separate QML triangle, preventing seams at different sizes or display scales.
- Bubble width follows content until it reaches a cap: `min(360 px, 68% of the available history width)`. Beyond the cap, text wraps by words. Bubbles retain minimum horizontal padding and may grow vertically without a fixed height.
- Incoming and outgoing tails mirror exactly and connect near the lower edge, matching the reference.

### Composer

- The composer is a bordered white field over a pale blue footer.
- A narrow dropdown segment occupies the field's right edge.
- Send is visually disabled while the trimmed input is empty and enables as soon as text exists.

### Resizing

- At narrow widths, both panes remain visible and keep approximately the reference split ratio.
- Contact thumbnails and secondary labels reduce slightly before any content is hidden.
- The member list does not become a drawer because the reference contains no such mode.
- At short heights, contact rows and message gaps compress consistently; section boundaries remain visible.

## Architecture

The app uses Qt 6 Quick/QML for presentation and C++ for state and behavior.

### C++ Layer

- `ContactListModel` is a `QAbstractListModel` exposing identity, avatar, presence, favorite status, and selection state.
- `MessageListModel` is a `QAbstractListModel` exposing direction, body, timestamp, and message kind.
- `ChatController` owns the models, current contact, search query, composer state, and send action.
- `main.cpp` registers types, creates the controller, loads the QML module, and fails clearly if the root object cannot load.

All seed data is deterministic and lives in a focused prototype-data unit, not inline in QML.

### QML Layer

- `Main.qml` owns the transparent top-level window and the high-level split.
- `AeroWindowFrame.qml` owns title chrome and window actions.
- `ContactSidebar.qml`, `ContactCategory.qml`, and `ContactRow.qml` own the left pane.
- `ConversationHeader.qml`, `MessageHistory.qml`, `MessageDelegate.qml`, and `Composer.qml` own the right pane.
- Reusable visual primitives include `PresenceBead.qml`, `AeroButton.qml`, and `Avatar.qml`.
- `BubbleBackground` is a small `QQuickPaintedItem` implemented in C++ with `QPainterPath`; it receives direction, radius, fill, and stroke values from QML.
- Call controls load replaceable SVG resources.
- `Theme.qml` is a singleton containing palette, typography, radii, borders, and spacing.

QML does not own application data, fabricate list entries, or duplicate controller logic.

## Data Flow

1. `ChatController` constructs deterministic contacts and message models.
2. QML binds list views to the exposed models.
3. Selecting a contact calls the controller, updates model selection, changes the header, and swaps the active message list.
4. Typing updates the controller's composer text and Send-enabled state.
5. Sending trims the text, appends an outgoing message, clears the composer, and scrolls the history to the end.
6. Searching updates a `QSortFilterProxyModel`; category headers remain visible only when that category has matching contacts.

## Error and Empty States

- A QML load failure returns a nonzero process exit code and logs the failing URL and component errors.
- An empty search shows `No contacts found` inside the contact pane without moving the pane geometry.
- A contact without messages shows the date header followed by `No messages yet.` and a usable composer.
- Attempting to send whitespace has no effect.
- Missing avatar assets fall back to a neutral silhouette without disturbing row dimensions.
- Missing final call SVGs fall back to built-in vector paths with the same bounding box.

## Verification

### Automated

- QtTest coverage for contact roles, category filtering, selection, composer enablement, whitespace rejection, and outgoing message insertion.
- A QML smoke test loads the full window and verifies required object names and minimum-size behavior.
- A bubble-geometry test renders incoming and outgoing backgrounds across minimum, typical, maximum, and wrapped sizes and checks that their paths remain closed and within bounds.
- CMake build and CTest run in a clean build directory.

### Visual

- Launch the actual binary on the user's computer after each coherent visual pass.
- Capture the running Qt window at the reference size.
- Compare the capture beside the supplied PNG for: outer frame geometry, split ratio, category-only separators, row density, header alignment, bubble tail joins, wrapping, composer geometry, and button silhouettes.
- Repeat until no material visual mismatch remains. Do not use the HTML mockup as the comparison target.

## Acceptance Criteria

- The running application is recognizably the supplied ClearChat reference at first glance and under close inspection.
- There are no lines between individual contact rows; only category boundaries divide the list.
- Bubble tails are part of the bubble path, remain connected at every tested size, and mirror correctly by direction.
- Long messages wrap within the defined cap and expand the bubble vertically.
- Video and phone buttons use correct vector silhouettes or the user's final SVG replacements.
- Search, contact selection, typing, and sending work with local data.
- The application builds and launches on the current Linux environment using Qt 6, with no architecture that prevents Windows or macOS builds.
- Automated tests pass and the final running-window screenshot has been visually checked against the supplied reference.

