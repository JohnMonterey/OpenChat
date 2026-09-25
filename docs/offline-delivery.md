# Messages to someone who is offline

A message reaches its recipient even when neither side is online at the same
time. Alice can message Bob while Bob's app is closed, close her own app, and
Bob gets the message when he next opens his.

## What the user sees

- **Sending to someone who is offline** looks like any other send: the
  message is accepted by the relay and marked *Sent*. There is no "Not sent"
  prompt because the recipient is away.
- **Sending while your own connection is down** keeps the message *Queued*.
  It leaves on its own once the app reconnects, and still does after the app
  is closed and opened again, because the outbox is on disk. Nothing needs
  to be retried by hand.
- **Opening the app** delivers everything that arrived while it was closed,
  in the order it was sent, before newer live traffic.

## How it works

The relay keeps a per-device inbox (`inbox_messages`) and replays it to a
device whenever that device opens its live socket. Every envelope kind except
call media (which uses the unstored datagram path) is stored there whether or
not the recipient is connected at the time. A device's acknowledgement prunes
what it has received.

Earlier relays refused a chat message with *RecipientUnavailable* when the
recipient had no live socket, and the client marked a message as failed when
its own link was down. Both refusals are gone:

- `EnvelopeService::submit` no longer takes or checks whether the recipient
  is connected, and the relay no longer sends the type-9 *RecipientUnavailable*
  frame for an offline device. It sends it only for a recipient device that
  does not exist or was retired (a login elsewhere retires an account's other
  devices), which will never take the envelope; the sender then gives up on it
  at once instead of retrying for minutes. The client handles that frame, so a
  message sent through a relay that has not been redeployed fails visibly
  instead of hanging.
- `SyncEngine` commits every outgoing text as *Queued* with a pending outbox
  row. The outbox is drained when the link comes up, on the one-second retry
  timer, and when the engine starts after a restart.

## How long a message waits

An envelope waits in the relay until its own expiry, so the lifetime the
sender stamps on it decides how long the recipient can be away:

| Kind | Lifetime |
| --- | --- |
| Messages, receipts, contact and group handshakes, profile and group updates | 30 days (`maxEnvelopeLifetimeMs`, the longest the wire format accepts) |
| Call signalling and call media | 24 hours |

Call signalling stays short so that an offer never rings a device that only
reappears days after the call. A send queued offline for longer than its
lifetime fails at once when the link returns, rather than being retried
against a relay that would refuse it every time.

## Retention on the relay

A device that never comes back never acknowledges, so its inbox would never be
pruned. The relay therefore runs `EnvelopeService::pruneExpired()` at startup
and every hour. It deletes every expired envelope, the whole inbox of any
revoked device (for example one retired by a password login elsewhere, which
can never fetch it), and expired acceptance records.

## Deploying

This is a relay change. Until the relay is rebuilt from this code, a message to
an offline recipient is still refused and shown as not sent. See the deploy
recipe for the hosted relay (`git pull --ff-only`, then
`docker compose --env-file deploy/.env.tunnel -f deploy/compose.tunnel.yaml up -d --build --no-deps relay`).
