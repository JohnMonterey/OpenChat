# OpenChat Secure Networking and Storage Design

## Objective

Replace OpenChat's in-memory mock conversations with durable accounts, real-time messaging, offline synchronization, end-to-end encryption, and encrypted local persistence while preserving the approved Qt/QML interface.

The security objective is concrete: a copied client database, a relay database, an intercepted network connection, or a stolen powered-off computer must not reveal message bodies, attachments, MLS state, device private keys, or recovery material without an unlocked authorized endpoint. The application must fail closed if its key vault, encrypted database, cryptographic engine, TLS validation, or protocol validation is unavailable.

## Security Claim and Limits

OpenChat will provide:

- end-to-end confidentiality and authentication for direct and group messages;
- forward secrecy and post-compromise security through Messaging Layer Security (MLS);
- encrypted local database pages, journals, and WAL files;
- operating-system-backed protection for the local database key;
- authenticated TLS transport in addition to message-level encryption;
- replay, duplicate, tamper, rollback, and malformed-envelope rejection;
- device verification, revocation, and visible security state;
- ciphertext-only server storage for messages and attachments;
- encrypted, integrity-protected backups that require a recovery secret.

OpenChat cannot protect plaintext from:

- malware, debuggers, memory inspection, or screen capture while the client is unlocked;
- a conversation participant who copies, photographs, exports, or forwards content;
- traffic analysis based on timing, account relationships, IP addresses, and ciphertext sizes;
- an already-compromised device until the device is removed and the MLS group advances;
- deletion recovery guarantees on SSDs, cloud snapshots, recipient devices, or server backups;
- denial of service, account-name squatting, or a malicious relay withholding messages.

The UI and documentation must never claim that data is literally unreadable under every circumstance. The supported claim is "encrypted at rest and end to end; readable only by unlocked authorized endpoints."

## Threat Model

### Protected assets

- account and device private keys;
- MLS credentials, KeyPackages, group epochs, ratchet state, and pending commits;
- local message bodies, drafts, attachment keys, filenames, thumbnails, and search indexes;
- recovery and backup keys;
- authentication refresh tokens;
- plaintext notifications and diagnostic output.

### Adversaries

- passive and active network attackers;
- a compromised or curious relay operator;
- an attacker with a copy of client or server disks and backups;
- a malicious contact sending malformed, replayed, oversized, or reordered envelopes;
- a revoked device attempting to resume a session;
- an attacker replacing a contact's advertised device keys;
- local users without access to the OS account's credential vault.

### Trust boundaries

1. QML is presentation only and must never persist secrets.
2. `ChatController` orchestrates application state but does not implement cryptography or SQL.
3. C++ domain services exchange typed plaintext only inside the unlocked process.
4. The Rust MLS bridge owns MLS state transitions and secret deletion.
5. SQLCipher is the only on-disk client database implementation.
6. The OS credential vault stores only the random database-unlock key and device-bootstrap wrapping key.
7. The relay authenticates devices and routes opaque envelopes; it never receives message or attachment plaintext.
8. PostgreSQL and object storage are treated as hostile ciphertext stores.

## Architecture Decision

### Cryptographic protocol

Use RFC 9420 Messaging Layer Security for both two-party and group conversations. A one-to-one chat is an MLS group containing the active devices of the two accounts. This avoids maintaining separate direct-message and group-message protocols and provides asynchronous group key establishment, forward secrecy, and post-compromise security.

Use OpenMLS `0.8.1` behind a project-owned Rust `staticlib` and a minimal C ABI. The bridge exposes domain operations rather than OpenMLS internals. OpenMLS debug features that can print content or key material are forbidden. The initial mandatory ciphersuite is `MLS_128_DHKEMX25519_CHACHA20POLY1305_SHA256_Ed25519`. Dependency updates require changelog and advisory review, storage-format migration testing, and protocol interoperability fixtures.

Do not use the archived `libsignal-protocol-c`, raw unsupported libsignal bridge symbols, custom Diffie-Hellman handshakes, reusable symmetric conversation keys, or home-grown ratchets.

### Local database

Use SQLCipher `4.17.0`, compiled from a pinned source archive whose SHA-256 is verified by CMake. Link it as a private SQLite implementation for OpenChat rather than loading the system `QSQLITE` driver. Configure:

- a random 256-bit raw database key, never a human password;
- `SQLITE_TEMP_STORE=2` and in-memory temporary tables;
- encrypted WAL mode and encrypted rollback journals;
- `PRAGMA cipher_memory_security = ON`;
- restrictive owner-only filesystem permissions;
- no plaintext FTS index, preview cache, or thumbnail cache;
- migration transactions with a schema version and integrity check;
- immediate failure when the key is absent or incorrect.

The database stores decrypted message bodies only because the complete file and journals are encrypted. Particularly sensitive long-lived secret blobs (MLS state, refresh tokens, attachment keys, backup state) receive an additional versioned AEAD envelope before insertion so accidental SQLCipher misconfiguration is not the only boundary.

### Key vault

Use qtkeychain `0.16.0` as a narrow cross-platform adapter over Windows Credential Manager, macOS Keychain, and Linux Secret Service/KWallet. Store one random database key and one device-bootstrap wrapping key per profile. Never fall back to a plaintext file, environment variable, command-line argument, QSettings, or compiled key.

If the vault is unavailable or locked, OpenChat shows a locked state and does not open or create a database. Logout removes refresh tokens; "Remove local profile" destroys vault entries and the encrypted local database after explicit confirmation.

### Transport

The client uses Qt Network and Qt WebSockets:

- HTTPS for account/device provisioning, KeyPackages, attachment upload tickets, and bounded catch-up;
- WSS for live envelope delivery, acknowledgements, presence, and wake-up hints;
- TLS 1.3 where supported, system trust roots, hostname verification, and no `ignoreSslErrors()` path;
- bounded frames, bounded decompression, connection/read timeouts, and exponential backoff with jitter;
- protocol subprotocol `openchat.ciphertext.v1`;
- short-lived access tokens in the WebSocket authorization handshake;
- optional development CA configured explicitly, never by disabling verification.

Message-level MLS encryption remains mandatory even inside TLS.

### Relay

Build a separate C++ executable, `openchat-relay`, using Qt Core/Network/WebSockets/HttpServer and PostgreSQL. In production it runs behind a TLS-terminating reverse proxy with request limits. The relay owns:

- account handles and opaque account IDs;
- device public credentials and revocation state;
- one-time KeyPackage queues;
- per-device ciphertext inboxes and acknowledgement watermarks;
- idempotency keys, expiry timestamps, and rate-limit counters;
- opaque attachment object metadata and ciphertext hashes;
- access/refresh token records stored only as hashes.

The relay never logs envelope bodies, tokens, KeyPackages, IP-plus-account combinations, or attachment URLs. Database administrators can observe routing metadata but not content.

## Identity, Accounts, and Devices

### Account creation

The first release uses device-key authentication rather than passwords:

1. The client generates an Ed25519 device-signing key and MLS credential locally.
2. The user registers a normalized handle and uploads only public device material.
3. The server issues a random challenge; the device signs the challenge and bound context.
4. The server returns a short-lived access token and a rotating refresh token.
5. The client generates a high-entropy recovery secret and displays an offline recovery code once.

Account creation endpoints are rate limited and designed to accept abuse controls without changing the cryptographic identity model.

### Additional devices

An existing verified device authorizes a new device by scanning a QR code or transferring a one-time link over an authenticated channel. The authorization binds account ID, new device public credential, nonce, expiry, and protocol version. Every affected conversation adds the new device through an MLS commit. The relay cannot silently add a device because existing clients reject credentials without a valid account-device authorization.

### Verification and revocation

- Each contact exposes a safety-number/QR verification view derived from stable account and device credentials.
- Device-list changes create a visible system event.
- Removing a device revokes its relay token, removes its MLS leaves, advances every affected group epoch, and rotates backup authorization.
- Key transparency is a required production phase: an append-only signed device directory with consistency proofs prevents targeted key substitution by the relay.

## Wire Protocol

Use canonical CBOR with strict schemas and a fixed maximum nesting depth. All integers are range checked; unknown critical fields reject the envelope; unknown noncritical fields may be retained for forward compatibility.

`CiphertextEnvelopeV1` contains:

- `version`: `1`;
- `envelope_id`: 16 random bytes;
- `sender_account_id`: 16 bytes;
- `sender_device_id`: 16 bytes;
- `recipient_device_id`: 16 bytes;
- `conversation_id`: 16 random bytes;
- `message_kind`: MLS private message, MLS handshake, receipt, or attachment control;
- `created_at_ms`: signed 64-bit UTC milliseconds used only for display hints;
- `expires_at_ms`: signed 64-bit UTC milliseconds;
- `idempotency_key`: 16 random bytes;
- `ciphertext`: bounded byte string;
- `ciphertext_sha256`: 32 bytes;
- `sender_signature`: Ed25519 signature over the canonical envelope fields.

The relay adds `server_sequence` per recipient inbox and `accepted_at_ms`; clients do not trust either for cryptographic ordering. Duplicate `envelope_id` or `idempotency_key` is accepted idempotently. MLS validates sender membership and epoch. Envelopes older than the local replay window or already committed are discarded after acknowledgement.

## Message Lifecycle and Synchronization

### Send transaction

1. Validate and normalize the composer input without modifying its Unicode content.
2. Create a local message ID and persist a `draft-to-send` record in one SQLCipher transaction.
3. Ask the MLS bridge to encrypt an application message for the current group epoch.
4. Persist the opaque outgoing envelope and advance MLS state in the same logical transaction boundary.
5. Update the UI optimistically to `Queued` only after durable persistence succeeds.
6. Send when authenticated; retry with capped exponential backoff and jitter.
7. Mark `Sent` after relay acceptance, `Delivered` after every active recipient device acknowledges, and `Read` only from an encrypted receipt.

Crashes between any steps recover from the outbox without duplicating visible messages or reusing cryptographic state.

### Receive transaction

1. Enforce frame size, schema, signature, expiry, recipient, and duplicate checks.
2. Persist the encrypted envelope before acknowledgement.
3. Process MLS handshakes in epoch order and buffer bounded future-epoch messages.
4. Decrypt and authenticate application messages.
5. Atomically persist the plaintext message, updated MLS state, replay record, and inbox watermark.
6. Notify the UI only after the transaction commits.
7. Send an acknowledgement containing no plaintext.

### Offline behavior

- Sending while disconnected always writes to the durable outbox.
- Reconnect performs token refresh, bounded catch-up from the last watermark, then resumes WSS.
- Out-of-order handshakes pause affected conversations without blocking other conversations.
- Clock values never decide cryptographic order.
- Server retention defaults to 30 days for unacknowledged ciphertext and is configurable by deployment policy.

## Local Schema

The initial SQLCipher schema contains:

- `profiles(profile_id, account_id, handle, local_device_id, created_at_ms)`;
- `devices(device_id, account_id, display_name, credential, verified_state, revoked_at_ms)`;
- `conversations(conversation_id, mls_group_id, title, kind, epoch_hint, created_at_ms)`;
- `conversation_members(conversation_id, account_id, device_id, active)`;
- `messages(message_id, conversation_id, sender_device_id, direction, kind, body, sent_at_ms, state, server_sequence, reply_to_id)`;
- `message_ciphertexts(message_id, envelope_id, envelope, envelope_hash)`;
- `outbox(envelope_id, conversation_id, attempt_count, next_attempt_ms, state)`;
- `inbox_replay(envelope_id, sender_device_id, received_at_ms)`;
- `mls_state(conversation_id, state_version, encrypted_state_blob)`;
- `attachments(attachment_id, message_id, encrypted_metadata, ciphertext_hash, byte_count, transfer_state)`;
- `drafts(conversation_id, body, updated_at_ms)`;
- `sync_state(device_id, server_watermark, updated_at_ms)`;
- `schema_migrations(version, applied_at_ms, checksum)`.

Foreign keys are enabled, destructive cascades are explicit, and every query is parameterized. Search is initially an in-memory scan of decrypted messages in the open conversation; persistent plaintext FTS is forbidden.

## Attachments

- Generate a random 256-bit attachment key per file.
- Encrypt in bounded chunks with libsodium secretstream XChaCha20-Poly1305 or an equivalently reviewed streaming AEAD.
- Encrypt filename, MIME type, dimensions, and attachment key inside the MLS application message.
- Upload only ciphertext through a short-lived, size-bounded ticket.
- Verify the complete ciphertext hash before decryption.
- Write decrypted data only to an explicitly chosen destination; previews stay memory-only unless separately encrypted.
- Strip image metadata only with explicit product policy; never mutate arbitrary files silently.

## Backups and Recovery

- Backups are explicit encrypted exports, never raw database copies synchronized by default.
- A random backup key encrypts the export; the recovery code derives a wrapping key using Argon2id with recorded parameters and random salt.
- The backup manifest is authenticated and versioned and includes account/device credentials, MLS state, conversations, and attachments selected by the user.
- Restore creates a new device identity, authenticates account recovery, imports history, and rejoins active conversations. It never clones a live device identity.
- Recovery attempts are rate limited server-side. Losing every authorized device and recovery code is intentionally unrecoverable.

## Privacy and Metadata

- Notifications show no message text by default while locked.
- Typing and presence are optional, ephemeral, and never stored as message history.
- The relay learns sender device, recipient device, timing, size, and delivery state. This limitation is disclosed.
- Padding buckets for small MLS application messages are added before public release.
- Telemetry is opt-in, aggregate, and contains no identifiers, message fields, crypto errors with payloads, or network tokens.
- Logs use structured event codes and redact all content. Crash dumps are disabled for secret-owning production processes unless an OS facility guarantees encrypted restricted storage.

## Error Handling

- Vault locked/unavailable: remain on locked screen; never create a replacement key.
- Database authentication/integrity failure: stop opening the profile and offer restore/export diagnostics without mutation.
- MLS state error: quarantine only the affected conversation and request a signed resynchronization path; never display unauthenticated plaintext.
- TLS validation error: disconnect with a durable diagnostic code; never offer an ignore button.
- Expired/revoked token: refresh once, otherwise lock networking without deleting the outbox.
- Malformed/oversized envelope: reject, count toward a bounded abuse score, and do not parse recursively.
- Server unavailable: stay usable offline and retry durable outbox items.
- Disk full: abort the transaction and keep the composer content in memory; never acknowledge an unpersisted incoming message.

## Deployment and Operations

- Development uses `docker compose` for relay, PostgreSQL, and an object-store emulator; TLS uses a checked-in development CA only.
- Production requires a managed PostgreSQL cluster, encrypted backups, object retention policies, TLS certificates, rate limiting, audit logging without content, and secret injection from a deployment vault.
- Database migrations run as a dedicated deployment step with rollback rehearsal.
- Build outputs include an SBOM and pinned dependency lockfiles.
- Release artifacts are signed for Windows, macOS, and Linux; update manifests are signed separately.
- Security updates for OpenMLS, SQLCipher, OpenSSL, Qt, Rust crates, and qtkeychain block release until assessed.

## Verification Strategy

Security-critical work cannot be validated visually. The networking phase requires automated checks even though earlier UI-only iterations used visual verification:

- unit tests for codecs, bounds, state transitions, repositories, and failure closure;
- golden canonical-CBOR and MLS interoperability fixtures;
- integration tests with two clients, multiple devices, offline delivery, reconnect, deduplication, and revocation;
- database-at-rest inspection proving no SQLite header or plaintext corpus appears in DB/WAL/journal files;
- wrong-key, modified-page, modified-envelope, replay, rollback, and truncated-ciphertext tests;
- fuzzing for CBOR parsing, relay frames, and Rust FFI inputs;
- sanitizer builds for C++ and Rust;
- dependency advisory scanning and license/SBOM generation;
- packet capture proving no message body appears outside encrypted endpoints;
- an independent cryptographic design and implementation audit before production claims.

## Rollout

1. Introduce domain IDs, delivery states, repository boundaries, and deterministic mock adapters without changing the UI.
2. Add OS vault and SQLCipher profile opening; migrate mock messages only in development fixtures.
3. Add versioned envelope codec, durable outbox, and loopback transport.
4. Add the relay and authenticate two real local clients over TLS.
5. Integrate OpenMLS and reject plaintext transport completely.
6. Add multi-device membership, verification, revocation, and encrypted receipts.
7. Add encrypted attachments and recovery exports.
8. Complete key transparency, abuse controls, operations hardening, external audit, and production release.

## Acceptance Criteria

- Restarting OpenChat preserves conversations, drafts, delivery states, and sync watermarks.
- Removing the vault key makes the local database unusable and plaintext strings are absent from DB, WAL, journal, and backup files.
- Two independently provisioned clients exchange messages over WSS, including after either client was offline.
- The relay and PostgreSQL contain only ciphertext envelopes and routing metadata.
- Modified, replayed, duplicated, expired, oversized, wrong-recipient, and wrong-epoch envelopes are rejected safely.
- Revoking a device prevents further relay authentication and removes it from future MLS epochs.
- Direct and group conversations use the same reviewed MLS engine with forward secrecy and post-compromise security.
- Attachments are encrypted before upload and verified before decryption.
- No TLS-error bypass, plaintext key fallback, plaintext database mode, or payload logging exists in release builds.
- The approved Aero interface remains visually intact while mock data is replaced by repository-backed state.
- A third-party security audit is complete before OpenChat is described as production secure.
