CREATE TABLE IF NOT EXISTS verification_markers (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    marker BLOB NOT NULL
);

CREATE TABLE IF NOT EXISTS conversations (
    id BLOB PRIMARY KEY NOT NULL CHECK(length(id) = 16),
    kind INTEGER NOT NULL,
    title TEXT NOT NULL,
    created_at_ms INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS messages (
    id BLOB PRIMARY KEY NOT NULL CHECK(length(id) = 16),
    conversation_id BLOB NOT NULL REFERENCES conversations(id) ON DELETE CASCADE,
    sender_device_id BLOB NOT NULL CHECK(length(sender_device_id) = 16),
    content_kind INTEGER NOT NULL,
    content BLOB NOT NULL,
    client_created_at_ms INTEGER NOT NULL,
    server_sequence INTEGER,
    delivery_state INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS outbox (
    envelope_id BLOB PRIMARY KEY NOT NULL CHECK(length(envelope_id) = 16),
    message_id BLOB NOT NULL REFERENCES messages(id) ON DELETE CASCADE,
    ciphertext BLOB NOT NULL,
    attempt_count INTEGER NOT NULL DEFAULT 0,
    next_attempt_at_ms INTEGER NOT NULL,
    state INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS sync_cursors (
    conversation_id BLOB PRIMARY KEY NOT NULL REFERENCES conversations(id) ON DELETE CASCADE,
    server_sequence INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS replay_cache (
    envelope_id BLOB PRIMARY KEY NOT NULL CHECK(length(envelope_id) = 16),
    received_at_ms INTEGER NOT NULL
);

PRAGMA user_version = 1;
