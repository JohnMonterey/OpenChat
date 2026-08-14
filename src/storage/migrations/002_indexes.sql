CREATE INDEX IF NOT EXISTS messages_conversation_sequence
    ON messages(conversation_id, server_sequence);
CREATE INDEX IF NOT EXISTS outbox_due
    ON outbox(state, next_attempt_at_ms);
CREATE INDEX IF NOT EXISTS replay_cache_received
    ON replay_cache(received_at_ms);

PRAGMA user_version = 2;
