ALTER TABLE conversations ADD COLUMN mls_group_id BLOB NOT NULL DEFAULT X'';

ALTER TABLE messages ADD COLUMN flow INTEGER NOT NULL DEFAULT 0;
ALTER TABLE messages ADD COLUMN body TEXT NOT NULL DEFAULT '';
ALTER TABLE messages ADD COLUMN sent_at_ms INTEGER NOT NULL DEFAULT 0;
ALTER TABLE messages ADD COLUMN reply_to_id BLOB;

ALTER TABLE outbox ADD COLUMN conversation_id BLOB;
ALTER TABLE outbox ADD COLUMN lease_until_ms INTEGER NOT NULL DEFAULT 0;

ALTER TABLE replay_cache ADD COLUMN sender_device_id BLOB;

CREATE TABLE IF NOT EXISTS device_sync_cursors (
    device_id BLOB PRIMARY KEY NOT NULL CHECK(length(device_id) = 16),
    server_watermark INTEGER NOT NULL,
    updated_at_ms INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS messages_conversation_sent
    ON messages(conversation_id, sent_at_ms DESC, id DESC);

PRAGMA user_version = 3;
