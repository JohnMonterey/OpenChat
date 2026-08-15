# OpenChat Secure Networking Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace OpenChat's mock in-memory chats with durable, offline-capable, end-to-end encrypted conversations delivered through a ciphertext-only relay.

**Architecture:** The Qt/C++ client retains the approved QML interface and gains domain services, an OS-backed key vault, SQLCipher repositories, a durable outbox, and authenticated Qt WebSocket transport. A narrow Rust static library wraps OpenMLS 0.9.0-rc.2 for both direct and group E2EE; a separate C++ relay authenticates devices and stores only opaque envelopes in PostgreSQL.

**Tech Stack:** C++20, CMake 3.21+, Qt 6.5+ Core/Network/WebSockets/HttpServer/Sql/Concurrent/DBus, Rust 1.91+ with Cargo, OpenMLS 0.9.0-rc.2, SQLCipher 4.17.0, qtkeychain 0.16.0, OpenSSL 3.x, PostgreSQL 16+, canonical CBOR, Docker Compose

**Spec:** `docs/superpowers/specs/2026-08-14-secure-networking-design.md`

## Global Constraints

- The visible product name remains exactly `OpenChat`; preserve the approved native-window Aero interface.
- QML owns presentation only; repositories, encryption, networking, and state machines live outside QML.
- Never implement a custom ratchet or key agreement; direct and group chats use RFC 9420 MLS through pinned OpenMLS 0.9.0-rc.2, with release blocked until its pre-release dependency status is cleared.
- Never send or store message/attachment plaintext on the relay.
- Never open a plaintext client database or fall back to a file/environment/argument key.
- Never call `ignoreSslErrors()` or provide a TLS bypass.
- The client remains useful offline; every send is durable before network transmission.
- Every external byte sequence is size bounded and validated before allocation or parsing.
- C++ exceptions and Rust panics must not cross the FFI boundary.
- Sensitive buffers are explicitly wiped where supported; logs never contain message bodies, tokens, keys, envelopes, or attachment URLs.
- Dependency sources and release artifacts are pinned and hash verified.
- The earlier visual-only test preference does not apply to this security phase: security claims require automated unit, integration, tamper, fuzz, and at-rest inspection evidence.

---

## File Map

### Client domain and orchestration

- `src/core/Result.h` — C++20 success/error result type used across trust boundaries.
- `src/domain/Identifiers.{h,cpp}` — strongly typed random 128-bit account, device, conversation, message, envelope, and attachment IDs.
- `src/domain/ChatTypes.h` — conversation kind, message kind, delivery state, verification state, and durable value records.
- `src/domain/Clock.h` — injectable UTC clock boundary.
- `src/repositories/ChatRepository.h` — transaction-oriented conversation/message/draft interface.
- `src/repositories/OutboxRepository.h` — durable outgoing-envelope interface.
- `src/repositories/SyncRepository.h` — inbox replay and watermark interface.
- `src/security/KeyVault.h` — asynchronous OS credential-vault contract.
- `src/security/QtKeychainVault.{h,cpp}` — fail-closed qtkeychain adapter.
- `src/security/SecureBuffer.{h,cpp}` — move-only zeroizing secret buffer.
- `src/storage/SqlCipherDatabase.{h,cpp}` — keyed database lifecycle and migrations.
- `src/storage/SqlCipherChatRepository.{h,cpp}` — SQLCipher-backed chat repository.
- `src/storage/SqlCipherOutboxRepository.{h,cpp}` — SQLCipher-backed outbox.
- `src/storage/SqlCipherSyncRepository.{h,cpp}` — replay/watermark persistence.
- `src/protocol/CiphertextEnvelope.{h,cpp}` — bounded versioned envelope value type.
- `src/protocol/CanonicalCborCodec.{h,cpp}` — strict canonical CBOR encoding/decoding.
- `src/crypto/MlsClient.{h,cpp}` — C++ RAII facade over the Rust ABI.
- `src/network/RelayClient.{h,cpp}` — HTTPS/WSS session, TLS policy, reconnect, and frame limits.
- `src/network/SyncEngine.{h,cpp}` — outbox/inbox state machine and acknowledgements.
- `src/controllers/ChatController.{h,cpp}` — repository-backed UI orchestration only.
- `src/app/ProfileSession.{h,cpp}` — vault unlock, DB open, service wiring, and lock/logout.

### Rust MLS bridge

- `rust/openchat-mls/Cargo.toml` — pinned staticlib crate and lockfile.
- `rust/openchat-mls/src/lib.rs` — panic-contained C ABI.
- `rust/openchat-mls/src/client.rs` — device credential, KeyPackage, group, encrypt/decrypt operations.
- `rust/openchat-mls/src/storage.rs` — opaque storage callbacks into C++.
- `rust/openchat-mls/include/openchat_mls.h` — generated and checked C header.

### Relay

- `relay/CMakeLists.txt` — relay target.
- `relay/src/main.cpp` — configuration and lifecycle.
- `relay/src/RelayServer.{h,cpp}` — HTTP/WSS routing and bounds.
- `relay/src/AuthService.{h,cpp}` — device challenge authentication and token rotation.
- `relay/src/EnvelopeService.{h,cpp}` — idempotent ciphertext fan-out and acknowledgement.
- `relay/src/KeyPackageService.{h,cpp}` — one-time KeyPackage publication and claim.
- `relay/src/PostgresStore.{h,cpp}` — parameterized PostgreSQL persistence.
- `relay/migrations/*.sql` — accounts, devices, KeyPackages, inboxes, tokens, attachments, and rate limits.
- `deploy/compose.yaml` — local relay/PostgreSQL/object-store environment.
- `deploy/Caddyfile.dev` — trusted development TLS endpoint.

### Verification

- `tests/tst_domain.cpp` — ID and state invariants.
- `tests/tst_envelopecodec.cpp` — canonical encoding, rejection, and golden fixtures.
- `tests/tst_sqlcipherstorage.cpp` — persistence, wrong-key, tamper, migration, and plaintext inspection.
- `tests/tst_keyvault.cpp` — fail-closed vault behavior with a fake backend.
- `tests/tst_syncengine.cpp` — offline queue, retry, dedupe, ordering, and acknowledgements.
- `tests/tst_chatcontroller.cpp` — repository-backed UI behavior.
- `rust/openchat-mls/tests/interop.rs` — two-device and multi-device MLS flows.
- `tests/integration/tst_two_clients.cpp` — two real clients through the relay.
- `fuzz/cbor_decoder.cpp` and `fuzz/mls_bridge.rs` — untrusted-input fuzz targets.

---

### Task 1: Stable domain model and repository boundaries

**Files:**
- Create: `src/core/Result.h`
- Create: `src/domain/Identifiers.h`
- Create: `src/domain/Identifiers.cpp`
- Create: `src/domain/ChatTypes.h`
- Create: `src/domain/Clock.h`
- Create: `src/repositories/ChatRepository.h`
- Create: `src/repositories/OutboxRepository.h`
- Create: `src/repositories/SyncRepository.h`
- Create: `tests/tst_domain.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `template<class Tag> class StrongId` with `generate()`, `fromBytes(QByteArrayView)`, `bytes()`, `toHex()`, equality, ordering, and `qHash`.
- Produces: C++20 `Result<T, E>` and `Result<void, E>` with `hasValue()`, `value()`, and `error()`; no exceptions cross repository boundaries.
- Produces: `ProfileId`, `AccountId`, `DeviceId`, `ConversationId`, `MessageId`, `EnvelopeId`, and `AttachmentId` aliases.
- Produces: `DeliveryState { Draft, Queued, Sending, Sent, Delivered, Read, Failed }` with monotonic transition validation.
- Produces: repository records `ConversationRecord`, `MessageRecord`, `OutboxRecord`, `SyncCursor`.
- Produces: transaction-oriented pure virtual repository APIs returning `Result<T, RepositoryError>`.

- [ ] **Step 1: Add invariant tests**

```cpp
void DomainTest::idsRequireExactlySixteenBytes()
{
    QVERIFY(!MessageId::fromBytes(QByteArray(15, '\0')).has_value());
    QVERIFY(MessageId::fromBytes(QByteArray(16, '\1')).has_value());
    QCOMPARE(MessageId::generate().bytes().size(), 16);
}

void DomainTest::deliveryStateCannotRegress()
{
    QVERIFY(canTransition(DeliveryState::Queued, DeliveryState::Sent));
    QVERIFY(!canTransition(DeliveryState::Delivered, DeliveryState::Sending));
}
```

- [ ] **Step 2: Verify the new tests fail to compile**

Run: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j2 --target tst_domain`

Expected: compilation fails because `Identifiers.h` and `ChatTypes.h` do not exist.

- [ ] **Step 3: Implement random strong IDs and state invariants**

Use `QRandomGenerator::system()->generate()` only for non-cryptographic IDs. Reject null and wrong-length IDs at every decode boundary. Store timestamps as UTC milliseconds and expose formatted local time only from the view model.

- [ ] **Step 4: Define repository contracts without a storage implementation**

```cpp
class ChatRepository {
public:
    virtual ~ChatRepository() = default;
    virtual Result<QVector<ConversationRecord>, RepositoryError> conversations() = 0;
    virtual Result<QVector<MessageRecord>, RepositoryError>
        messages(const ConversationId &, int limit, const std::optional<MessageId> &before) = 0;
    virtual Result<void, RepositoryError>
        saveOutgoing(const MessageRecord &, const OutboxRecord &) = 0;
    virtual Result<void, RepositoryError>
        applyIncoming(const MessageRecord &, const EnvelopeId &, quint64 watermark) = 0;
};
```

- [ ] **Step 5: Build and run the focused test**

Run: `cmake --build build -j2 --target tst_domain && ./build/tst_domain`

Expected: all domain invariants pass.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt src/core src/domain src/repositories tests/tst_domain.cpp
git commit -m "feat: define secure chat domain boundaries"
```

### Task 2: Zeroizing secrets and fail-closed key-vault contract

**Files:**
- Create: `src/security/SecureBuffer.h`
- Create: `src/security/SecureBuffer.cpp`
- Create: `src/security/KeyVault.h`
- Create: `src/security/QtKeychainVault.h`
- Create: `src/security/QtKeychainVault.cpp`
- Create: `tests/tst_keyvault.cpp`
- Modify: `cmake/dependencies/QtKeychain.cmake`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `ProfileId` from Task 1.
- Produces: move-only `SecureBuffer` with locked-memory best effort and guaranteed zeroization on destruction.
- Produces: `KeyVault::readProfileKey`, `createProfileKey`, `deleteProfileKey`, and `availability`.
- Produces: `QtKeychainVault` with service name `org.openchat.OpenChat` and no fallback backend.

- [ ] **Step 1: Add fake-vault failure tests**

```cpp
void KeyVaultTest::unavailableVaultNeverReturnsAReplacementKey()
{
    FakeKeyVault vault(KeyVaultAvailability::Unavailable);
    auto result = vault.readProfileKey(ProfileId::generate());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error(), KeyVaultError::Unavailable);
}
```

- [ ] **Step 2: Verify failure**

Run: `cmake --build build -j2 --target tst_keyvault`

Expected: compilation fails because the security interfaces do not exist.

- [ ] **Step 3: Implement `SecureBuffer`**

Allocate 32-byte profile keys, disable copy operations, permit moves, expose only scoped `QByteArrayView`, wipe with `OPENSSL_cleanse`, and attempt `mlock`/`VirtualLock` without weakening failure behavior if locking is unavailable.

- [ ] **Step 4: Pin and integrate qtkeychain**

Fetch qtkeychain `0.16.0` from its release archive with an exact SHA-256. Build without insecure fallback. Map native cancellation, locked vault, missing entry, and backend unavailable to distinct errors.

- [ ] **Step 5: Run tests and manually verify native vault entry lifecycle**

Run: `cmake --build build -j2 --target tst_keyvault && ./build/tst_keyvault`

Expected: fake-vault tests pass; an opt-in local diagnostic creates, reads, and removes a test entry without printing its value.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt cmake/dependencies/QtKeychain.cmake src/security tests/tst_keyvault.cpp
git commit -m "feat: add fail-closed profile key vault"
```

### Task 3: SQLCipher build and keyed database lifecycle

**Files:**
- Create: `cmake/dependencies/SqlCipher.cmake`
- Create: `src/storage/SqlCipherDatabase.h`
- Create: `src/storage/SqlCipherDatabase.cpp`
- Create: `src/storage/migrations/001_initial.sql`
- Create: `src/storage/migrations/002_indexes.sql`
- Create: `tests/tst_sqlcipherstorage.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `SecureBuffer` profile key.
- Produces: `SqlCipherDatabase::open(path, key)`, `transaction(fn)`, `close()`, and migration status.
- Produces: a private `openchat_sqlcipher` target pinned to SQLCipher `4.17.0`.

- [ ] **Step 1: Add at-rest and wrong-key tests**

```cpp
void SqlCipherStorageTest::plaintextNeverAppearsOnDisk()
{
    const QByteArray marker("OPENCHAT_SECRET_CORPUS_7fd1");
    writeEncryptedFixture(marker);
    QVERIFY(!readAll(databasePath()).contains(marker));
    QVERIFY(!readAll(databasePath() + "-wal").contains(marker));
}

void SqlCipherStorageTest::wrongKeyFailsClosed()
{
    createDatabase(keyA());
    auto opened = SqlCipherDatabase::open(databasePath(), keyB());
    QVERIFY(!opened.has_value());
    QVERIFY(QFile::exists(databasePath()));
}
```

- [ ] **Step 2: Verify tests fail**

Run: `cmake --build build -j2 --target tst_sqlcipherstorage`

Expected: compilation fails because the SQLCipher database wrapper does not exist.

- [ ] **Step 3: Build pinned SQLCipher privately**

Configure SQLCipher with OpenSSL 3, `SQLITE_THREADSAFE=2`, `SQLITE_TEMP_STORE=2`, `SQLITE_HAS_CODEC`, `SQLCIPHER_CRYPTO_OPENSSL`, `SQLITE_EXTRA_INIT=sqlcipher_extra_init`, and `SQLITE_EXTRA_SHUTDOWN=sqlcipher_extra_shutdown`. Verify the archive hash before extraction.

- [ ] **Step 4: Implement keyed open and migration transaction**

Open through the SQLCipher C API, apply the raw 32-byte key with `sqlite3_key` before any query (avoiding a hexadecimal secret copy), enable foreign keys/WAL/memory security, run `cipher_integrity_check`, and reject an empty or incorrect key without creating a replacement database.

- [ ] **Step 5: Run storage verification**

Run: `cmake --build build -j2 --target tst_sqlcipherstorage && ./build/tst_sqlcipherstorage`

Expected: reopen persistence passes; wrong key, modified page, truncated WAL, migration rollback, and plaintext scans pass.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt cmake/dependencies/SqlCipher.cmake src/storage tests/tst_sqlcipherstorage.cpp
git commit -m "feat: add encrypted SQLCipher profile database"
```

### Task 4: SQLCipher repositories and durable outbox

**Files:**
- Create: `src/storage/SqlCipherChatRepository.h`
- Create: `src/storage/SqlCipherChatRepository.cpp`
- Create: `src/storage/SqlCipherOutboxRepository.h`
- Create: `src/storage/SqlCipherOutboxRepository.cpp`
- Create: `src/storage/SqlCipherSyncRepository.h`
- Create: `src/storage/SqlCipherSyncRepository.cpp`
- Create: `tests/tst_repositories.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Task 1 repository contracts and Task 3 database transactions.
- Produces: atomic `saveOutgoing`, `applyIncoming`, `advanceDeliveryState`, `claimDueOutbox`, `recordReplay`, and `advanceWatermark` operations.

- [x] **Step 1: Add crash-consistency and deduplication tests**

```cpp
void RepositoryTest::messageAndOutboxCommitAtomically()
{
    QVERIFY(repository.saveOutgoing(message(), outbox()).has_value());
    QCOMPARE(repository.messages(conversation(), 50, {}).value().size(), 1);
    QCOMPARE(outboxRepository.claimDue(now(), 10).value().size(), 1);
}

void RepositoryTest::incomingEnvelopeIsIdempotent()
{
    QVERIFY(repository.applyIncoming(message(), envelopeId(), 41).has_value());
    QVERIFY(repository.applyIncoming(message(), envelopeId(), 41).has_value());
    QCOMPARE(repository.messages(conversation(), 50, {}).value().size(), 1);
}
```

- [x] **Step 2: Verify failure, then implement parameterized SQL only**

Run: `cmake --build build -j2 --target tst_repositories`

Expected before implementation: compile failure. Use prepared statements for every value and explicit transactions for multi-table state changes.

- [x] **Step 3: Enforce delivery-state monotonicity and retry leases**

`claimDueOutbox` marks rows with a bounded lease so a crash makes them retryable. Attempts use `min(300s, 1s * 2^attempt) + random(0..1000ms)` and never delete an envelope until relay acceptance is durable.

- [x] **Step 4: Run focused storage tests**

Run: `cmake --build build -j2 --target tst_repositories && ./build/tst_repositories`

Expected: transaction rollback, dedupe, pagination, state monotonicity, and lease recovery pass.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt src/storage tests/tst_repositories.cpp
git commit -m "feat: persist chats and durable outbox"
```

### Task 5: Canonical bounded ciphertext envelope

**Files:**
- Create: `src/protocol/CiphertextEnvelope.h`
- Create: `src/protocol/CiphertextEnvelope.cpp`
- Create: `src/protocol/CanonicalCborCodec.h`
- Create: `src/protocol/CanonicalCborCodec.cpp`
- Create: `tests/fixtures/envelope-v1.cbor.hex`
- Create: `tests/tst_envelopecodec.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `CiphertextEnvelopeV1` exactly matching the spec.
- Produces: `encodeCanonical(const CiphertextEnvelopeV1&)` and `decodeEnvelope(QByteArrayView, DecodeLimits)`.
- Produces: constants `maxEnvelopeBytes=1_MiB`, `maxCiphertextBytes=960_KiB`, `maxCborDepth=8`.

- [x] **Step 1: Add golden and hostile-input tests**

```cpp
void EnvelopeCodecTest::canonicalEncodingMatchesGoldenFixture()
{
    QCOMPARE(encodeCanonical(fixedEnvelope()).toHex(), readFixtureHex("envelope-v1"));
}

void EnvelopeCodecTest::rejectsOversizeBeforeParsing()
{
    const QByteArray hostile(1024 * 1024 + 1, '\xff');
    QCOMPARE(decodeEnvelope(hostile).error(), DecodeError::FrameTooLarge);
}
```

- [x] **Step 2: Verify failure and implement strict decoder**

Reject duplicate map keys, indefinite lengths, noncanonical integer encodings, wrong field sizes, unknown critical fields, invalid expiry ranges, mismatched hashes, and trailing bytes.

- [x] **Step 3: Run codec tests**

Run: `cmake --build build -j2 --target tst_envelopecodec && ./build/tst_envelopecodec`

Expected: golden round-trip and every rejection table case passes.

- [x] **Step 4: Commit**

```bash
git add CMakeLists.txt src/protocol tests/fixtures tests/tst_envelopecodec.cpp
git commit -m "feat: add bounded ciphertext envelope protocol"
```

### Task 6: OpenMLS Rust bridge

**Files:**
- Create: `rust/openchat-mls/Cargo.toml`
- Create: `rust/openchat-mls/Cargo.lock`
- Create: `rust/openchat-mls/src/lib.rs`
- Create: `rust/openchat-mls/src/client.rs`
- Create: `rust/openchat-mls/src/storage.rs`
- Create: `rust/openchat-mls/include/openchat_mls.h`
- Create: `rust/openchat-mls/tests/interop.rs`
- Create: `cmake/OpenChatRust.cmake`
- Create: `src/crypto/MlsClient.h`
- Create: `src/crypto/MlsClient.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: opaque SQLCipher storage callbacks and canonical application payload bytes.
- Produces C ABI: `oc_mls_client_create/free`, `oc_mls_generate_key_package`, `oc_mls_create_group`, `oc_mls_join_group`, `oc_mls_add_members`, `oc_mls_remove_members`, `oc_mls_encrypt`, `oc_mls_process`, and `oc_mls_free_buffer`.
- Produces C++ RAII `MlsClient` with `Result<T, MlsError>` results and no raw ownership.

- [x] **Step 1: Pin OpenMLS and disable secret-debug features**

```toml
[dependencies]
openmls = { version = "=0.9.0-rc.2", default-features = false }
openmls_rust_crypto = { version = "=0.6.0-rc.2", default-features = false }
tls_codec = "=0.5.0"
zeroize = { version = "=1.9.0", features = ["derive"] }
```

Generate and commit `Cargo.lock`. CI rejects `content-debug`, `crypto-debug`, git dependencies, and yanked/advisory-affected crates.

- [x] **Step 2: Add Rust two-client and epoch tests**

Create Alice/Bob credentials, publish/consume KeyPackages, create a two-member group, exchange application messages, reject a tampered ciphertext, remove Bob, and prove Bob cannot decrypt the next epoch.

- [x] **Step 3: Implement panic-contained C ABI**

Wrap every exported function in `catch_unwind`, validate pointer/length pairs before slice creation, cap inputs, return stable numeric error codes, and zeroize all returned secret buffers on free.

- [x] **Step 4: Add the C++ RAII facade**

```cpp
Result<MlsCiphertext, MlsError>
MlsClient::encrypt(const ConversationId &conversation, QByteArrayView plaintext);

Result<MlsProcessResult, MlsError>
MlsClient::process(const ConversationId &conversation, QByteArrayView mlsMessage);
```

- [x] **Step 5: Run Rust and C++ bridge verification**

Run: `cargo test --manifest-path rust/openchat-mls/Cargo.toml --locked && cmake --build build -j2 --target tst_mlsbridge && ./build/tst_mlsbridge`

Expected: direct/group flows, tamper rejection, removal, reordered application messages, and panic containment pass.

- [x] **Step 6: Commit**

```bash
git add CMakeLists.txt cmake/OpenChatRust.cmake rust/openchat-mls src/crypto tests/tst_mlsbridge.cpp
git commit -m "integrate OpenMLS encryption"
```

### Task 7: Profile session, device identity, and account bootstrap

**Files:**
- Create: `src/app/ProfileSession.h`
- Create: `src/app/ProfileSession.cpp`
- Create: `src/security/DeviceIdentity.h`
- Create: `src/security/DeviceIdentity.cpp`
- Create: `src/security/RecoveryCode.h`
- Create: `src/security/RecoveryCode.cpp`
- Create: `tests/tst_profilesession.cpp`
- Modify: `src/main.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `KeyVault`, `SqlCipherDatabase`, repositories, and `MlsClient`.
- Produces: `ProfileSession::{create, unlock, lock, removeLocalProfile}`.
- Produces: `DeviceIdentity::{generate, signChallenge, publicCredential}`.
- Produces: one-time `RecoveryCode` with explicit reveal/confirm lifecycle.

- [ ] **Step 1: Add fail-closed lifecycle tests**

```cpp
void ProfileSessionTest::missingVaultKeyDoesNotCreateASecondIdentity()
{
    auto result = ProfileSession::unlock(profileId(), unavailableVault(), paths());
    QVERIFY(!result.has_value());
    QVERIFY(!QFile::exists(paths().database));
}
```

- [ ] **Step 2: Implement create/unlock sequencing**

Create the vault key before the database, roll back the vault entry if DB initialization fails, store the device private identity only inside SQLCipher's additionally wrapped secret table, and wire services only after integrity and migration checks pass.

- [ ] **Step 3: Implement explicit lock and removal**

Lock stops networking, cancels queued decryptions, closes repositories, destroys MLS handles, wipes `SecureBuffer`s, clears QML models, then closes SQLCipher. Removal requires a profile-ID confirmation token and deletes only the resolved profile path.

- [ ] **Step 4: Run lifecycle tests**

Run: `cmake --build build -j2 --target tst_profilesession && ./build/tst_profilesession`

Expected: rollback, wrong key, lock ordering, and removal target tests pass.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt src/app src/security src/main.cpp tests/tst_profilesession.cpp
git commit -m "feat: add secure profile and device lifecycle"
```

### Task 8: Authenticated HTTPS/WSS relay client

**Files:**
- Create: `src/network/RelayClient.h`
- Create: `src/network/RelayClient.cpp`
- Create: `src/network/BackoffPolicy.h`
- Create: `src/network/BackoffPolicy.cpp`
- Create: `tests/tst_relayclient.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: access/refresh token callbacks, `CiphertextEnvelopeV1`, and explicit endpoint configuration.
- Produces: `RelayClient::{authenticateDevice, connectLive, sendEnvelope, acknowledge, fetchSince, disconnect}`.
- Produces signals with typed results: `connected`, `disconnected`, `envelopeReceived`, `relayAccepted`, and `authExpired`.

- [ ] **Step 1: Add TLS and frame-limit tests using a local fake server**

Verify hostname mismatch, untrusted CA, expired certificate, oversized frame, invalid subprotocol, and invalid CBOR all disconnect without invoking envelope callbacks.

- [ ] **Step 2: Implement strict TLS configuration**

Set peer verification to `VerifyPeer`, require `SecureProtocols`, set read buffer and HTTP response limits, bind the WebSocket subprotocol to `openchat.ciphertext.v1`, and make the SSL-error signal terminal. No production code references `ignoreSslErrors`.

- [ ] **Step 3: Implement token refresh and reconnect state machine**

Allow one serialized refresh attempt, queue no plaintext in the network object, resume with the durable watermark, and apply full-jitter backoff capped at five minutes.

- [ ] **Step 4: Run network tests**

Run: `cmake --build build -j2 --target tst_relayclient && ./build/tst_relayclient`

Expected: valid TLS path succeeds; every invalid certificate/frame/auth path fails closed.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt src/network tests/tst_relayclient.cpp
git commit -m "feat: add authenticated ciphertext relay client"
```

### Task 9: Relay schema, device authentication, and ciphertext inbox

**Files:**
- Create: `relay/CMakeLists.txt`
- Create: `relay/src/main.cpp`
- Create: `relay/src/RelayServer.h`
- Create: `relay/src/RelayServer.cpp`
- Create: `relay/src/AuthService.h`
- Create: `relay/src/AuthService.cpp`
- Create: `relay/src/EnvelopeService.h`
- Create: `relay/src/EnvelopeService.cpp`
- Create: `relay/src/KeyPackageService.h`
- Create: `relay/src/KeyPackageService.cpp`
- Create: `relay/src/PostgresStore.h`
- Create: `relay/src/PostgresStore.cpp`
- Create: `relay/migrations/001_accounts_devices.sql`
- Create: `relay/migrations/002_tokens_keypackages.sql`
- Create: `relay/migrations/003_inboxes_attachments.sql`
- Create: `relay/tests/tst_relayservices.cpp`
- Modify: top-level `CMakeLists.txt`

**Interfaces:**
- Produces HTTP endpoints: `POST /v1/accounts`, `/v1/auth/challenge`, `/v1/auth/complete`, `/v1/auth/refresh`, `/v1/key-packages`, `/v1/key-packages/claim`, `/v1/sync`.
- Produces WSS endpoint: `/v1/live` with send, acknowledgement, ping, and server-draining frames.
- Produces PostgreSQL transactions for idempotent fan-out and monotonic per-device sequences.

- [ ] **Step 1: Create schema with ciphertext-only constraints**

Use `BYTEA` for credentials, hashes, KeyPackages, and envelopes; prohibit any `body`, `plaintext`, `message_text`, or decrypted attachment column through a schema-policy test.

- [ ] **Step 2: Implement signed device challenge authentication**

Challenges are 32 random bytes, single-use, expire in 120 seconds, and bind account/device IDs plus protocol version. Store refresh tokens as SHA-256 hashes with family IDs, rotate on every use, and revoke the entire family on reuse.

- [ ] **Step 3: Implement idempotent envelope fan-out**

Validate schema/size/hash/signature without decrypting MLS content. Insert one inbox row per active recipient device and return the existing acceptance result for duplicate idempotency keys.

- [ ] **Step 4: Implement one-time KeyPackage claim**

Claim and delete KeyPackages in one serializable transaction. Reject expired, reused, oversized, or device-revoked packages.

- [ ] **Step 5: Run service tests against disposable PostgreSQL**

Run: `ctest --test-dir build -R relay --output-on-failure`

Expected: challenge replay, refresh reuse, duplicate send, concurrent KeyPackage claim, watermark, and revoked-device tests pass.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt relay
git commit -m "feat: add ciphertext-only OpenChat relay"
```

### Task 10: Durable sync engine and MLS transaction coordination

**Files:**
- Create: `src/network/SyncEngine.h`
- Create: `src/network/SyncEngine.cpp`
- Create: `src/network/MlsTransactionCoordinator.h`
- Create: `src/network/MlsTransactionCoordinator.cpp`
- Create: `tests/tst_syncengine.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: repositories, `MlsClient`, `RelayClient`, and injectable `Clock`.
- Produces: `SyncEngine::{start, stop, enqueueText, acknowledgeRead, handleEnvelope}`.
- Produces conversation-scoped MLS serialization so two operations never mutate one MLS group concurrently.

- [ ] **Step 1: Add offline and crash-recovery tests**

```cpp
void SyncEngineTest::offlineSendSurvivesEngineRestart()
{
    firstEngine.enqueueText(conversation(), "durable");
    firstEngine.stop();
    secondEngine.start();
    QCOMPARE(fakeRelay.sentEnvelopeCount(), 1);
}
```

Cover crash after MLS encryption but before send, relay acceptance before local state update, duplicate incoming envelope, future epoch buffering, stale epoch rejection, and outbox retry exhaustion.

- [ ] **Step 2: Implement send transaction**

Persist the local message, MLS output, updated MLS state, and outbox row atomically. Emit UI state only after commit. Never ask MLS to encrypt the same logical message twice after recovery.

- [ ] **Step 3: Implement receive transaction**

Persist raw envelope, validate signature/replay, process MLS, then atomically commit plaintext message, MLS state, replay ID, and watermark before acknowledging.

- [ ] **Step 4: Implement encrypted receipts**

Delivery and read receipts are MLS application messages containing bounded message IDs and states; the relay only sees ordinary ciphertext envelopes.

- [ ] **Step 5: Run sync tests**

Run: `cmake --build build -j2 --target tst_syncengine && ./build/tst_syncengine`

Expected: offline, recovery, dedupe, order, retry, receipt, and conversation-isolation scenarios pass.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt src/network tests/tst_syncengine.cpp
git commit -m "feat: synchronize durable encrypted messages"
```

### Task 11: Replace mock chats beneath the approved UI

**Files:**
- Modify: `src/controllers/ChatController.h`
- Modify: `src/controllers/ChatController.cpp`
- Modify: `src/models/Message.h`
- Modify: `src/models/MessageListModel.h`
- Modify: `src/models/MessageListModel.cpp`
- Modify: `src/models/ContactListModel.{h,cpp}`
- Modify: `src/main.cpp`
- Modify: `qml/OpenChat/components/MessageDelegate.qml`
- Modify: `qml/OpenChat/components/Composer.qml`
- Modify: `tests/tst_chatcontroller.cpp`
- Modify: `tests/tst_qmlload.cpp`

**Interfaces:**
- Consumes: `ProfileSession`, `ChatRepository`, and `SyncEngine`.
- Produces model roles: stable ID, delivery state, failure reason code, sender device, and security-system event.
- Preserves existing body/timestamp/kind/date roles and approved geometry.

- [ ] **Step 1: Replace constructor seed data with injected services**

`ChatController(ProfileSession&, QObject*)` loads conversations and current messages from repositories. Keep deterministic mock adapters only in tests and capture mode; release mode cannot instantiate seed chats.

- [ ] **Step 2: Route sending through `SyncEngine`**

Keep composer text until durable enqueue succeeds. Display `Queued`, `Sent`, `Delivered`, `Read`, and retry states without changing bubble dimensions; expose details through accessible labels and a context action.

- [ ] **Step 3: Add locked/offline/security states**

The interface must distinguish vault locked, offline, reconnecting, conversation quarantined, device changed, and storage full. No state displays unverified plaintext.

- [ ] **Step 4: Build and run controller/QML tests plus visual capture**

Run: `cmake --build build -j2 && ctest --test-dir build -R 'tst_chatcontroller|tst_qmlload' --output-on-failure && ./tools/capture_openchat.sh build/networked-ui.png 860 680`

Expected: repository-backed messages render with unchanged approved geometry and durable send states.

- [ ] **Step 5: Commit**

```bash
git add src/controllers src/models src/main.cpp qml tests/tst_chatcontroller.cpp tests/tst_qmlload.cpp
git commit -m "feat: connect OpenChat UI to durable sync"
```

### Task 12: Encrypted attachments

**Files:**
- Create: `src/attachments/AttachmentCrypto.h`
- Create: `src/attachments/AttachmentCrypto.cpp`
- Create: `src/attachments/AttachmentTransfer.h`
- Create: `src/attachments/AttachmentTransfer.cpp`
- Create: `tests/tst_attachments.cpp`
- Modify: `src/network/RelayClient.{h,cpp}`
- Modify: relay attachment endpoints and schema

**Interfaces:**
- Produces: streaming `encryptFile`, `decryptFile`, `resumeUpload`, and `cancel` operations.
- Produces: encrypted metadata embedded only in the MLS application payload.
- Enforces: 2 GiB absolute file limit, 1 MiB chunks, ciphertext SHA-256, and destination-safe filenames.

- [ ] **Step 1: Add round-trip, tamper, truncation, and path tests**

Verify empty, small, multi-chunk, resumed, corrupted, oversized, and malicious filename cases. Confirm plaintext filename and corpus never appear in relay storage.

- [ ] **Step 2: Implement libsodium secretstream through a pinned provider**

Generate one random key per attachment, authenticate each chunk and final tag, wipe keys, and never create a partially decrypted final file. Move the verified temporary file atomically to the chosen destination.

- [ ] **Step 3: Add short-lived upload tickets and resumable ciphertext transfer**

Tickets bind attachment ID, uploader device, ciphertext size/hash, expiry, and maximum chunk count. Object-store URLs never enter logs.

- [ ] **Step 4: Run attachment tests and packet/disk inspection**

Run: `cmake --build build -j2 --target tst_attachments && ./build/tst_attachments`

Expected: all integrity failures reject and delete temporary plaintext; relay/object storage contains only ciphertext.

- [ ] **Step 5: Commit**

```bash
git add src/attachments src/network relay tests/tst_attachments.cpp
git commit -m "feat: add encrypted attachment transfer"
```

### Task 13: Multi-device authorization, verification, and revocation

**Files:**
- Create: `src/security/DeviceAuthorization.h`
- Create: `src/security/DeviceAuthorization.cpp`
- Create: `src/models/DeviceListModel.h`
- Create: `src/models/DeviceListModel.cpp`
- Create: `qml/OpenChat/components/DeviceVerificationDialog.qml`
- Create: `tests/tst_devices.cpp`
- Modify: `SyncEngine`, `MlsClient`, relay auth/device services, and `CMakeLists.txt`

**Interfaces:**
- Produces: bounded signed `DeviceAuthorizationV1` QR payload with account ID, new credential hash, nonce, and expiry.
- Produces: verification fingerprints, device-list system events, and `revokeDevice` workflow.

- [ ] **Step 1: Add authorization/revocation tests**

Reject expired, wrong-account, replayed, modified, already-used, and self-revocation payloads. Verify a revoked device cannot refresh, fetch, send, or decrypt the next MLS epoch.

- [ ] **Step 2: Implement QR authorization and device enrollment**

The existing device signs canonical authorization bytes. The relay stores the used nonce and public authorization; affected clients verify it before accepting the new MLS credential.

- [ ] **Step 3: Implement revocation fan-out**

Revoke tokens first, publish a signed device-list change, serialize MLS remove commits for every affected group, and expose partial-progress state until all commits are durable.

- [ ] **Step 4: Add key verification UI without changing the main chat layout**

Show safety number, QR, verified/unverified state, and device-change history in a modal reachable from the conversation header menu.

- [ ] **Step 5: Run device tests and commit**

Run: `cmake --build build -j2 --target tst_devices && ./build/tst_devices`

```bash
git add src/security src/models qml/OpenChat/components relay tests/tst_devices.cpp CMakeLists.txt
git commit -m "feat: add verified multi-device security"
```

### Task 14: Encrypted recovery exports

**Files:**
- Create: `src/backup/BackupManifest.h`
- Create: `src/backup/BackupService.h`
- Create: `src/backup/BackupService.cpp`
- Create: `src/security/RecoveryKdf.h`
- Create: `src/security/RecoveryKdf.cpp`
- Create: `tests/tst_backup.cpp`
- Modify: `ProfileSession` and `CMakeLists.txt`

**Interfaces:**
- Produces: versioned authenticated backup manifest and chunked encrypted export.
- Produces: Argon2id recovery wrapping with stored salt, memory, iteration, and parallelism parameters.
- Produces: restore-as-new-device flow; never clones a live device identity.

- [ ] **Step 1: Add wrong-code, corruption, downgrade, and partial-file tests**

Include known corpus checks proving plaintext and database headers never appear in the export. Verify interrupted exports do not replace an existing valid backup.

- [ ] **Step 2: Implement streaming export and atomic completion**

Create a random backup key, encrypt each bounded record/chunk, authenticate the manifest, wrap the backup key with the Argon2id-derived key, fsync, then atomically rename.

- [ ] **Step 3: Implement restore into a fresh profile**

Validate the full manifest before mutation, create a new device identity, import history in a transaction, then use recovery authorization to rejoin active accounts/conversations.

- [ ] **Step 4: Run backup tests and commit**

Run: `cmake --build build -j2 --target tst_backup && ./build/tst_backup`

```bash
git add src/backup src/security src/app tests/tst_backup.cpp CMakeLists.txt
git commit -m "feat: add encrypted recovery exports"
```

### Task 15: Key transparency and directory consistency

**Files:**
- Create: `src/security/TransparencyClient.h`
- Create: `src/security/TransparencyClient.cpp`
- Create: `relay/src/TransparencyService.h`
- Create: `relay/src/TransparencyService.cpp`
- Create: `relay/migrations/004_transparency_log.sql`
- Create: `tests/tst_transparency.cpp`

**Interfaces:**
- Produces: append-only signed device-directory leaves, inclusion proofs, consistency proofs, and client gossip checkpoints.
- Blocks: silent relay substitution or targeted omission of device credentials.

- [ ] **Step 1: Add proof and equivocation tests**

Verify valid inclusion/consistency, modified leaf, missing device, stale tree head, split-view gossip, and signing-key rotation with cross-signature.

- [ ] **Step 2: Implement append-only log and signed tree heads**

Every device add/revoke appends a canonical leaf. Relay responses include inclusion proof and the latest tree head. Clients persist the last accepted head in SQLCipher and reject nonconsistent updates.

- [ ] **Step 3: Add checkpoint gossip through encrypted conversations**

Piggyback recent tree heads inside MLS application control messages. Quarantine device changes when two valid signatures describe inconsistent trees at the same size.

- [ ] **Step 4: Run transparency tests and commit**

Run: `cmake --build build -j2 --target tst_transparency && ./build/tst_transparency`

```bash
git add src/security relay tests/tst_transparency.cpp
git commit -m "feat: verify device directory transparency"
```

### Task 16: End-to-end two-client integration and adversarial verification

**Files:**
- Create: `tests/integration/IntegrationHarness.h`
- Create: `tests/integration/IntegrationHarness.cpp`
- Create: `tests/integration/tst_two_clients.cpp`
- Create: `tests/integration/tst_multidevice.cpp`
- Create: `tests/integration/tst_adversarial.cpp`
- Create: `tests/integration/inspect_at_rest.cmake`
- Create: `deploy/compose.yaml`
- Create: `deploy/Caddyfile.dev`
- Create: `deploy/dev-ca/README.md`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: hermetic local PostgreSQL/relay/TLS test environment and independent Alice/Bob/secondary-device profiles.

- [ ] **Step 1: Build the disposable environment**

Use a generated per-run development CA and certificates, random database credentials, isolated ports, and disposable volumes. Never commit private CA keys.

- [ ] **Step 2: Verify core user journeys**

Create accounts, verify devices, send online, send offline, restart clients, catch up, deliver/read receipts, create a three-member group, add/remove a device, and transfer a multi-chunk attachment.

- [ ] **Step 3: Verify adversarial cases**

Inject modified ciphertext/signature/hash, replay, duplicate idempotency key, stale/future epoch, wrong recipient, expired token, refresh reuse, hostile CBOR, oversized frame, disk full, wrong DB key, and relay restart mid-transaction.

- [ ] **Step 4: Inspect all storage and packet captures**

Search client DB/WAL/journal, PostgreSQL dumps, object storage, relay logs, and packet captures for a unique plaintext corpus. The test fails on any match.

- [ ] **Step 5: Run and commit**

Run: `ctest --test-dir build -L integration --output-on-failure`

```bash
git add CMakeLists.txt tests/integration deploy
git commit -m "test: verify encrypted messaging end to end"
```

### Task 17: Fuzzing, sanitizers, dependency policy, and observability

**Files:**
- Create: `fuzz/cbor_decoder.cpp`
- Create: `rust/openchat-mls/fuzz/Cargo.toml`
- Create: `rust/openchat-mls/fuzz/fuzz_targets/process_message.rs`
- Create: `cmake/Sanitizers.cmake`
- Create: `cmake/DependencyPolicy.cmake`
- Create: `src/diagnostics/SecurityLog.h`
- Create: `src/diagnostics/SecurityLog.cpp`
- Create: `.github/workflows/security.yml`
- Create: `SECURITY.md`
- Modify: all logging call sites.

**Interfaces:**
- Produces: structured redacted event IDs, ASan/UBSan builds, C++/Rust fuzz targets, SBOM, advisory scan, and secret-pattern log scanner.

- [ ] **Step 1: Add log-redaction tests**

Pass canary bodies, tokens, keys, envelope bytes, URLs, and usernames through every error class; assert logs contain only event code, bounded public category, and random correlation ID.

- [ ] **Step 2: Add fuzz targets with seed corpora**

Seed canonical valid envelopes, each invalid-field class, MLS handshake/application messages, and truncated inputs. Every target enforces a memory/time cap and treats panic/crash/OOM as failure.

- [ ] **Step 3: Add release dependency gates**

Run `cargo audit`, CMake pinned-hash validation, GitHub dependency review, SBOM generation, license allowlist, and a scan that rejects forbidden OpenMLS debug features and `ignoreSslErrors`.

- [ ] **Step 4: Run sanitizer and fuzz smoke jobs**

Run: `cmake -S . -B build-asan -DOPENCHAT_SANITIZERS=ON && cmake --build build-asan -j2 && ctest --test-dir build-asan --output-on-failure`

Run each fuzz target for at least 60 seconds in pull requests and a longer scheduled budget nightly.

- [ ] **Step 5: Commit**

```bash
git add fuzz rust/openchat-mls/fuzz cmake src/diagnostics .github SECURITY.md
git commit -m "chore: harden OpenChat security pipeline"
```

### Task 18: Production packaging, operations, and audit gate

**Files:**
- Create: `deploy/production/README.md`
- Create: `deploy/production/relay.service`
- Create: `deploy/production/backup-restore-runbook.md`
- Create: `deploy/production/key-rotation-runbook.md`
- Create: `deploy/production/incident-response.md`
- Create: `docs/security/cryptographic-inventory.md`
- Create: `docs/security/privacy-metadata.md`
- Create: `docs/security/audit-scope.md`
- Modify: packaging/signing configuration for Windows, macOS, and Linux.

**Interfaces:**
- Produces: signed cross-platform client packages, signed update manifests, relay deployment runbooks, and a frozen audit candidate.

- [ ] **Step 1: Document exact production configuration**

Include TLS termination, PostgreSQL roles, encrypted backups, object retention, rate limits, token-signing-key rotation, transparency-log signing, metrics allowlist, log retention, and restore rehearsal.

- [ ] **Step 2: Produce signed reproducible artifacts and SBOMs**

Build from a clean tag with locked dependencies. Compare reproducible hashes where platform toolchains permit and record unavoidable nondeterminism.

- [ ] **Step 3: Execute disaster and incident rehearsals**

Restore relay metadata from backup, rotate auth/transparency keys, revoke a compromised device, drain a relay, recover from PostgreSQL failover, and verify message ciphertext remains opaque throughout.

- [ ] **Step 4: Freeze and commission an independent audit**

Audit scope includes protocol composition, OpenMLS FFI, SQLCipher/key-vault lifecycle, relay auth, sync crash consistency, attachment streaming, recovery, key transparency, build pipeline, and client UI security claims. Any high/critical finding blocks release.

- [ ] **Step 5: Complete acceptance evidence and tag**

Archive test reports, fuzz corpus coverage, dependency/SBOM results, packet/disk plaintext scans, audit remediation, and signed artifacts. Tag only after every acceptance criterion in the spec has objective evidence.

```bash
git add deploy/production docs/security packaging
git commit -m "docs: finalize secure OpenChat operations"
git tag -s openchat-secure-v1.0.0-rc1
```

---

## Plan Self-Review

- **Spec coverage:** Tasks 1-18 cover domain state, OS vault, SQLCipher, repositories, envelope protocol, OpenMLS, profiles, TLS client, relay, sync, UI replacement, attachments, multi-device security, recovery, transparency, integration/adversarial verification, hardening, deployment, and independent audit.
- **Trust-boundary coverage:** Every transition between QML, C++, Rust, disk, network, relay, PostgreSQL, object storage, backup, and operations has an owner and a validation step.
- **Failure closure:** Missing vault, wrong key, DB corruption, TLS errors, malformed frames, MLS errors, token reuse, disk full, relay downtime, and migration failure are explicitly exercised.
- **Type consistency:** IDs originate in Task 1; repositories consume them in Tasks 3-4; envelopes use them in Task 5; MLS/network/sync consume the same types in Tasks 6-10; UI and later features consume those services without duplicating state.
- **No insecure transition state:** The UI stays on mock adapters until vault, encrypted DB, envelope, MLS, relay, and sync are coherent. Release mode never stores new real messages in plaintext.
- **No production-security shortcut:** Production claims remain blocked on automated evidence and independent audit.
