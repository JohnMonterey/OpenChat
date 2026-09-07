# Leaving and rejoining calls

Leaving a call does not end it. Whoever is still in the call stays in it, and
whoever left can come back, for one-to-one calls and group calls alike.

## What the user sees

- **Hanging up** takes you to the ended-call surface as before. If the call is
  still going without you, a green **Rejoin** button sits beside **Back to
  chat** for as long as that is true.
- **The last person left in a call** stays in it for a five-minute grace
  period. The status line counts it down ("Waiting for Jessica to come back ·
  4:59"), the peer's picture is captioned *Left* and faded, and the microphone
  is closed because there is nobody to send to. If nobody comes back in time
  the call ends as *Nobody rejoined*. Hanging up during the wait ends it at
  once.
- **A conversation with a call going on without you** — one you left,
  declined, missed, or were busy for — shows a banner under its subtitle
  ("Jessica and Michael are in a call") with a **Join** button. The phone
  button joins that call too instead of ringing anyone, and the sidebar row
  carries a small *In call* mark.
- **Calling a group that is already on a call** joins the running call. The
  members in it answer with the running call's id instead of "busy", the
  redundant call is dropped before it ever reaches the ended surface, and no
  one else is rung for it.
- **Rejoining a one-to-one call whose peer has since left** rings them as an
  ordinary call; if they never answer, it ends as *No answer*. Joining a group
  call and finding nobody there ends the same way rather than leaving you
  waiting in an empty room.

## The call surface belongs to its conversation

The in-call surface replaces the header of the conversation the call is on,
and only that one. Opening any other conversation brings that conversation's
own header back and shows a one-line strip under it instead
(`CallStrip.qml`): "In call with Jessica · 3:12" with **Return** and **End
call**, or, for a call still ringing, "Incoming call from Jessica" with
**Answer**, **Decline** and **Open**. Full-screen ends when the call's
conversation is left. The sidebar row of the call's own chat carries the *In
call* mark as well, so the call can always be found again.
`CallController::callChatId` / `callInCurrentChat` drive this; a caller with
no chat (an unknown caller) has no conversation to be sent to, so their call
shows wherever the user is.

## How it works

Everything lives in `CallEngine` (`src/call/CallEngine.*`); the UI is
`CallController` plus `CallHeader.qml`, `ConversationHeader.qml` and
`ContactRow.qml`.

**Grace period.** `CallEngine::Config::rejoinGraceMs` (default five minutes)
runs whenever the engine is in a call (`Connecting`/`Active`) with no other
member pending — nobody joined, ringing, or being confirmed. When it fires the
call ends as `CallEndReason::Abandoned`, which goes on the wire as an ordinary
`LocalHangup` so older builds never see a reason they cannot decode. While
alone the media stall timer is stopped (no path, no media to expect) and the
capture, sessions and screen share are released; the call id and the secret
that still-ringing members will key from are kept.

**Ongoing-call records.** `CallEngine::ongoingCalls()` lists calls this device
is not in while somebody is. A record is made whenever the engine ends a call
that others are still in (hang-up, decline, missed ring, media failure) and
when a group offer is refused as busy. It is kept current because every
group-call `Answer` and `Hangup` is now broadcast to **every** member, in the
call or not, and updated in `noteOngoingSignal`. A record is dropped the
moment its last joined member leaves; there is nothing to join in an empty
call. Records are in memory only; a restarted client falls back to the
"calling a group already on a call" path above.

**Rejoining and re-keying.** `joinCall(conversation)` sends an `Offer` under
the *same call id* with a *fresh secret*. A media session must never be keyed
twice with frame numbers restarting from zero, so a rejoin always re-keys:

- One-to-one: the rejoiner is the offering end of the new session whichever
  way the call was first placed (`m_sessionDirection`); the peer, still in the
  call, re-keys and answers without ringing.
- Group: each pair keys from the *lower device id's* latest offered secret
  (`pairBaseSecret`). Every member learns the others' current secrets from
  their offers and, new in this change, from their **answers** — an accepted
  `Answer` now carries the answerer's current secret in the previously empty
  secret field (older builds send it empty and never changed theirs). That
  makes the choice come out the same on both ends however offers and answers
  crossed, including two members rejoining in the same instant.

Members the rejoiner last saw in the call are shown as *Reconnecting…*
(`CallParticipantState::Connecting`) until they answer; the ring timeout turns
those who did not into *Left*, and a rejoin that confirms nobody ends as
`NoAnswer`.

**Protocol.** No new envelope kind and no relay change: the same
`CallSignal` messages are used, with the answer's secret field now populated.

## Validation

- `tst_callengine`: leaving and waiting, rejoin with fresh keys, grace timeout,
  calling again while the peer waits, a rejoin that finds the peer gone, a
  rejoin nobody answers.
- `tst_groupcall`: rejoin after leaving, joining after declining or missing,
  the last member's grace period, joining once no longer busy, calling a group
  already on a call, and two members rejoining at once.
- `tst_qmlload`: the waiting status and peer caption, the Rejoin button, the
  header banner with Join, and the sidebar mark.
- `tst_e2e`: leave, rejoin and re-key over the real relay, one-to-one and
  group.
