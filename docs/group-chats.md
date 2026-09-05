# Group chats and group calls

The "+" next to the name at the top of any chat starts a group. In a
one-to-one chat it lists every other accepted contact; picking one makes a
group of the three of you and opens it. In a group it adds the picked contact.
A group's name is edited the way the status line is: click it, type, press
Enter (Escape reverts). **Leave group** tells the others and removes the chat
here. A group is named after its members until someone names it.

## How a group works

A group is one multi-party MLS group whose id is the conversation id. The
creator claims one KeyPackage per invited device from the relay, adds them all
in a single commit, and ships the sealed Welcome to each device as
`EnvelopeMessageKind::GroupWelcome`, followed by a `GroupControl` *Info*
message (title plus the full roster of accounts, devices and names) encrypted
under the new epoch. Adding a member later ships the MLS Commit to the existing
members and the Welcome to the newcomer in one atomic send, then the roster to
everyone. The outbox hands envelopes out in the order they were queued, so a
Welcome always precedes the roster that depends on it.

A device joins a `GroupWelcome` only when the sender is an already-accepted
contact and the Welcome's own membership names the sender's device; anything
else is consumed and dropped. Members who are not contacts of each other can
still talk: a group message is encrypted once and fanned out as one envelope
per member device, all carrying the same message id. The row reaches *Sent* on
the first relay acceptance and *Failed* only when no member could be reached.

Leaving sends a `GroupControl` *Leave*; every remaining member drops the leaver
from the roster and the one with the lowest device id commits their removal
(an `MlsCommit` to the rest), so the group re-keys without them. Application
messages sealed under a recent past epoch still decrypt (three epochs), so a
message in flight during an add or removal is not lost. A group this device has
left keeps its row, hidden, so a late envelope from a member who has not yet
heard can still be stored. Two members committing in the same epoch is not
reconciled; the loser's commit is dropped by the others.

Storage: `group_members` (migration 013) keyed by conversation and device, and
`conversations.left_at_ms`. Code: `src/app/GroupService.*` (create, add,
rename, leave, inbound updates), `src/domain/GroupUpdate.*` (the control codec),
`SyncEngine::enqueueGroupText` / `sendGroupControl` / `sendGroupChange`.

## Group calls

The phone or video button in a group rings every member at once: the caller
sends the same offer (one call id, one secret) to each device. Each pair of
members keys its own media path from the call secret and the ordered pair of
device ids (`deriveGroupPairSecret`), so audio never passes through a third
device and no two paths share a key. Answers are broadcast; a member already in
the call replies to a newcomer so both key their side of the pair. Playback is
the sum of every member's jitter buffer. The call screen shows everyone with
what they are doing (ringing, declined, busy, no answer, left); the talker is
ringed green. A member who hangs up is dropped from the others' mesh and the
call carries on; it ends for a device only when nobody else is left.

## Validation

- `tst_groupupdate`: the control codec.
- `tst_mlsbridge`: three-party groups, member listing, removal, past epochs.
- `tst_syncstore`, `tst_syncengine`: fan-out commits, per-envelope failure,
  trusted-sender Welcome joins, queue-ordered draining.
- `tst_groupcall`: a mesh of scripted endpoints: ring, join, mix, leave,
  decline, timeout, busy, glare, cameras.
- `tst_chatcontroller`, `tst_qmlload`: the surface, live and mock.
- `tst_e2e::groupChatAndGroupCallOverRealTls`: three clients over the real
  TLS relay; needs the PostgreSQL test service.
- `OpenChat --call-group --capture <path.png>`: the group call preview.
