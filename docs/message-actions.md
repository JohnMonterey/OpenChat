# Copying, editing and replying to messages

Every message's text can be selected, and three small actions sit under a
message while the pointer is on it: copy, edit (one's own messages only) and
reply.

## What the user sees

- **Selecting text** works in any bubble, like in a document: drag across it
  and copy with Ctrl+C.
- **The actions** are bare glyphs in the gap every message already leaves
  below it, lined up with the text's edge, and the hovered one is named beside
  them. Showing them moves nothing. Each glyph is 11 px, but its whole
  22 x 20 px slot takes the click.
- **Copy** puts the whole message on the clipboard and says *Copied*.
- **Edit** puts the message's text in the composer, with a bar above it
  reading *Editing message*. On this device the bubble shows *editing...*
  until the edit is sent or cancelled; everyone else keeps seeing the message
  as it was. Enter sends the new text. Esc or the bar's cross cancels. Either
  way, whatever was being typed before the edit comes back. Once edited, the
  message says *edited* below it, above the actions, for everyone.
- **Reply** adds a bar reading *Replying to Name* and the start of the
  message. The reply shows that quote above its own text; clicking the quote
  scrolls to the original.

Edit is offered only once the relay has taken the message (*Sent* or
later). Otherwise an edit could reach the peer before the message it
changes. Messages from before this feature cannot be edited (see below), but
they can still be replied to.

## How it works

### Shared message ids

A reply or an edit has to name a message in a way both ends understand. Each
side used to give a message its own random id. Now the id is the first 16
bytes of the SHA-256 of the message's MLS ciphertext
(`messageIdForCiphertext`). The sender and every recipient see the same
ciphertext (a group message is encrypted once for all members), so all of
them arrive at the same id without it being sent. Rows stored with such an id
have `shared_id = 1`; older history keeps `0`, and nothing can refer to it.

### The payload

A plain message still travels as the bare UTF-8 of its text, exactly as
before, so older clients read it unchanged. Only a reply or an edit is tagged
(`src/domain/MessageContent.*`):

    0xFF  CBOR [1, 1, body, target id, quoted sender device, quoted excerpt]   reply
    0xFF  CBOR [1, 2, body, target id]                                         edit

`0xFF` never occurs in UTF-8, so a plain message can never be mistaken for
either. Both still travel as `MlsPrivateMessage` envelopes, so the relay sees
nothing new and did not need redeploying. A later version may append fields.
A tagged payload this version cannot read (malformed, of a later version, of
an unknown type) is consumed without showing anything. A client from before
this feature shows a reply or an edit as unreadable text.

A reply carries its quote (who wrote the answered message and up to 200
characters of it), so the quote shows even where the answered message is not
held, e.g. history from before shared ids.

### Who may edit what

The store's `WHERE` clause is the authorisation (`SqlCipherSyncStore`,
`applyEdit`):

- an inbound edit changes only a text row of the same conversation, sent by
  the same device (the MLS-authenticated sender), under a shared id, and only
  when it is newer than the last edit applied there. Anything else is consumed
  and changes nothing, so a member cannot rewrite someone else's words, and a
  late, older edit cannot undo a newer one;
- an edit sent from here changes only one of this device's own text rows that
  the relay has taken. `SyncEngine` asks `canEditSent` before encrypting, so an
  edit the store would refuse never moves the ratchet.

The edit's envelopes get a fresh id in the outbox, so their delivery never
touches the edited message's state. The text changes on screen only when the
engine reports the durable change (`messageEdited`).

### Storage

Migration 015 adds `shared_id`, `edited_at_ms`, `quoted_sender_device_id` and
`quoted_body` to `messages`; `reply_to_id` already existed.
