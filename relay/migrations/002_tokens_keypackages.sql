-- OpenChat relay schema, migration 002: authentication tokens and one-time
-- KeyPackages.
--
-- Access and refresh tokens are stored ONLY as SHA-256 hashes; the plaintext
-- token bytes are returned to the client once and never persisted. Refresh
-- tokens form a family: rotating a refresh token issues a new one in the same
-- family, and presenting an already-used (or a revoked-family) refresh token is
-- treated as theft and revokes the entire family.

CREATE TABLE token_families (
    family_id     BYTEA PRIMARY KEY CHECK (octet_length(family_id) = 16),
    device_id     BYTEA NOT NULL REFERENCES devices(device_id) ON DELETE CASCADE,
    account_id    BYTEA NOT NULL CHECK (octet_length(account_id) = 16),
    revoked       BOOLEAN NOT NULL DEFAULT FALSE,
    created_at_ms BIGINT NOT NULL
);

CREATE INDEX token_families_device_idx ON token_families (device_id);

CREATE TABLE refresh_tokens (
    token_hash    BYTEA PRIMARY KEY CHECK (octet_length(token_hash) = 32),
    family_id     BYTEA NOT NULL REFERENCES token_families(family_id) ON DELETE CASCADE,
    device_id     BYTEA NOT NULL CHECK (octet_length(device_id) = 16),
    created_at_ms BIGINT NOT NULL,
    expires_at_ms BIGINT NOT NULL,
    used_at_ms    BIGINT
);

CREATE INDEX refresh_tokens_family_idx ON refresh_tokens (family_id);

CREATE TABLE access_tokens (
    token_hash    BYTEA PRIMARY KEY CHECK (octet_length(token_hash) = 32),
    family_id     BYTEA NOT NULL REFERENCES token_families(family_id) ON DELETE CASCADE,
    device_id     BYTEA NOT NULL CHECK (octet_length(device_id) = 16),
    account_id    BYTEA NOT NULL CHECK (octet_length(account_id) = 16),
    created_at_ms BIGINT NOT NULL,
    expires_at_ms BIGINT NOT NULL
);

CREATE INDEX access_tokens_device_idx ON access_tokens (device_id);
CREATE INDEX access_tokens_expiry_idx ON access_tokens (expires_at_ms);

-- One-time KeyPackages published by a device for asynchronous MLS additions.
-- Each is claimed at most once inside a serializable transaction. package_ref
-- is an opaque hash used only for de-duplication of uploads.
CREATE TABLE key_packages (
    key_package_id BIGSERIAL PRIMARY KEY,
    device_id      BYTEA NOT NULL REFERENCES devices(device_id) ON DELETE CASCADE,
    account_id     BYTEA NOT NULL CHECK (octet_length(account_id) = 16),
    key_package    BYTEA NOT NULL CHECK (octet_length(key_package) BETWEEN 1 AND 262144),
    package_ref    BYTEA NOT NULL CHECK (octet_length(package_ref) = 32),
    created_at_ms  BIGINT NOT NULL,
    expires_at_ms  BIGINT NOT NULL,
    claimed_at_ms  BIGINT,
    claimed_by     BYTEA,
    UNIQUE (device_id, package_ref)
);

CREATE INDEX key_packages_claimable_idx ON key_packages (device_id, key_package_id)
    WHERE claimed_at_ms IS NULL;
