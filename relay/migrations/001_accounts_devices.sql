-- OpenChat relay schema, migration 001: accounts, devices, auth challenges.
--
-- The relay is a hostile ciphertext store: it holds opaque public credentials
-- and routing metadata only. It never stores message bodies, attachment
-- contents, MLS secrets, private keys, or recovery material. Every payload-
-- bearing column is BYTEA and opaque to the relay.

CREATE TABLE schema_migrations (
    version      INTEGER PRIMARY KEY,
    applied_at_ms BIGINT NOT NULL
);

CREATE TABLE accounts (
    account_id    BYTEA PRIMARY KEY CHECK (octet_length(account_id) = 16),
    handle        TEXT NOT NULL UNIQUE CHECK (char_length(handle) BETWEEN 1 AND 64),
    created_at_ms BIGINT NOT NULL
);

-- A device holds only public material: its Ed25519 signing public key (used to
-- verify signed challenges and envelope sender signatures) and an opaque MLS
-- credential blob. No device display name or other user metadata is stored.
CREATE TABLE devices (
    device_id     BYTEA PRIMARY KEY CHECK (octet_length(device_id) = 16),
    account_id    BYTEA NOT NULL REFERENCES accounts(account_id) ON DELETE CASCADE,
    signing_key   BYTEA NOT NULL CHECK (octet_length(signing_key) = 32),
    credential    BYTEA NOT NULL CHECK (octet_length(credential) BETWEEN 1 AND 65536),
    created_at_ms BIGINT NOT NULL,
    revoked_at_ms BIGINT
);

CREATE INDEX devices_account_idx ON devices (account_id);

-- Single-use device authentication challenges. A challenge is 32 random bytes,
-- bound to the account/device and protocol version, expires quickly, and is
-- marked consumed the first time it is completed.
CREATE TABLE auth_challenges (
    challenge        BYTEA PRIMARY KEY CHECK (octet_length(challenge) = 32),
    account_id       BYTEA NOT NULL CHECK (octet_length(account_id) = 16),
    device_id        BYTEA NOT NULL CHECK (octet_length(device_id) = 16),
    protocol_version INTEGER NOT NULL,
    created_at_ms    BIGINT NOT NULL,
    expires_at_ms    BIGINT NOT NULL,
    consumed_at_ms   BIGINT
);

CREATE INDEX auth_challenges_expiry_idx ON auth_challenges (expires_at_ms);
