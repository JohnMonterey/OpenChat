# Key package supply

Friend requests and group invitations each consume a one-time MLS KeyPackage
from the recipient device. Calls in an established conversation do not. Previously
registration published eight packages and never replaced them: eight claims,
including failed attempts, could make a device unreachable for new conversations.

Registration now publishes 16 packages. DeviceLink maintains the supply for existing
profiles too: after authentication, on live reconnect, when another device claims
a package, and hourly (to detect expiry). Below eight available packages it fills
to 16 in the background, one publish at a time. Private MLS state is committed to
SQLCipher **before** each upload, including during registration. A failed supply
check or publish retries after 30 seconds; uncertain publish outcomes recheck the
relay count. An empty account recovers on its next login without re-registering.
Offline devices cannot generate replacements until they reconnect.

The relay counts only unclaimed packages whose expiry is strictly in the future
(the default TTL is 90 days). `availableKeyPackages` is an optional integer in the
auth-complete and publish responses. Authenticated `GET /v1/key-packages` returns
the same count for the caller's device. Clients opting in with
`keyPackageSupply=1` on the live URL receive `[10, available]` control frames on
connect and claim. These are relay control frames, not MLS envelopes. Older
clients do not receive the new frame; missing counts are never interpreted as zero.

Deploy the updated relay before using replenishment against
`https://chat.rigidstudios.de/v1`. An older relay can still serve the client but
cannot supply counts/notifications. No MLS wire format or key derivation changed.
Deployment requires separate approval and is not part of this change.

## Failed requests

AddContactService now performs contact lookup, the blocked gate, outgoing-contact
and conversation storage, empty-group creation, and MLS persistence before claiming.
That preparation runs once per add; trying a peer's next device only re-points the
row. A claim that then fails rolls the preparation back — the row, the conversation
and the MLS group (`MlsClient::deleteGroup`) all go, a row that existed before the
attempt is put back as it was, and a peer blocked mid-flight keeps the row that says
so — so a failed add never leaves "request sent" behind for a request that never
left, and the handle can be asked again at once. The same cleanup
(`discardOutgoingRequest`) backs **withdrawing** a request: the sent mark on a
Search & Find row is a button, and clicking it removes the pending request so it
can be sent again. A Welcome that did reach the peer cannot be recalled; an
acceptance arriving after a withdrawal names a conversation this side no longer
knows and is ignored. GroupService already
checks accepted contacts and membership before claiming and now rechecks eligibility
and session state between asynchronous claims. Package-dependent MLS work and the
atomic Welcome/outbox commit must still follow the claim; a failure there, a block
while the claim is in flight, or a later member being unavailable can still consume
packages. Replenishment covers that loss. There is deliberately no unclaim endpoint:
an already disclosed one-time package must not be returned for reuse.

## Windows diagnostics

The desktop app writes Qt messages to `openchat.log` under
`QStandardPaths::AppDataLocation` (normally
`%APPDATA%\OpenChat\OpenChat\openchat.log` on Windows). Rotation keeps the current
file and `.1` / `.2`, each at most 1 MiB. Warnings, critical errors, and fatal errors
are logged with UTC timestamps and categories. Verbose relay, contacts, and MLS
logging is disabled by default. Enable it before launching the application with:

```powershell
$env:QT_LOGGING_RULES = 'openchat.relay.debug=true;openchat.contacts.debug=true;openchat.mls.debug=true'
```

These categories log stages, result codes and counts, never tokens, key-package
bytes, private MLS state, message content, or contact handles. Send the log and
rotated files with the exact displayed error. `NoKeyPackage`/a claim HTTP 404 points
to an unavailable package or missing device. The tester's *outgoing* requests consume
the target's packages, so his own exhausted pool does not explain that symptom.
The old “Couldn't reach the server” claim error can identify a build predating
`ded5fff` (claim-endpoint wiring); record the build and error before investigating.
