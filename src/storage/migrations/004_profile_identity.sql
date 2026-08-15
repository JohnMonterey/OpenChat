CREATE TABLE local_profiles (
    profile_id BLOB PRIMARY KEY NOT NULL CHECK(length(profile_id) = 16),
    device_id BLOB UNIQUE NOT NULL CHECK(length(device_id) = 16),
    signing_public_key BLOB NOT NULL CHECK(length(signing_public_key) = 32),
    private_nonce BLOB NOT NULL CHECK(length(private_nonce) = 12),
    private_ciphertext BLOB NOT NULL CHECK(length(private_ciphertext) = 32),
    private_tag BLOB NOT NULL CHECK(length(private_tag) = 16),
    created_at_ms INTEGER NOT NULL CHECK(created_at_ms >= 0)
);

CREATE TABLE local_mls_state (
    profile_id BLOB PRIMARY KEY NOT NULL REFERENCES local_profiles(profile_id) ON DELETE CASCADE,
    state_blob BLOB NOT NULL
);

PRAGMA user_version = 4;
