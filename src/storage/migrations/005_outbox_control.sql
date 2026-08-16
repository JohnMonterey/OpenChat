-- Control envelopes (e.g. encrypted read receipts) are queued through the same
-- durable outbox but have no visible chat message, so their outbox row cannot
-- reference messages(id). Rebuild outbox to drop that foreign key while keeping
-- message_id a required, decodable 16-byte identifier. saveOutgoing/commitSend
-- still insert a real message first, so their behavior is unchanged.

CREATE TABLE outbox_rebuilt (
    envelope_id BLOB PRIMARY KEY NOT NULL CHECK(length(envelope_id) = 16),
    message_id BLOB NOT NULL CHECK(length(message_id) = 16),
    ciphertext BLOB NOT NULL,
    attempt_count INTEGER NOT NULL DEFAULT 0,
    next_attempt_at_ms INTEGER NOT NULL,
    state INTEGER NOT NULL,
    conversation_id BLOB,
    lease_until_ms INTEGER NOT NULL DEFAULT 0
);

INSERT INTO outbox_rebuilt (
    envelope_id, message_id, ciphertext, attempt_count,
    next_attempt_at_ms, state, conversation_id, lease_until_ms)
SELECT envelope_id, message_id, ciphertext, attempt_count,
    next_attempt_at_ms, state, conversation_id, lease_until_ms
FROM outbox;

DROP TABLE outbox;

ALTER TABLE outbox_rebuilt RENAME TO outbox;

CREATE INDEX IF NOT EXISTS outbox_due
    ON outbox(state, next_attempt_at_ms);

PRAGMA user_version = 5;
