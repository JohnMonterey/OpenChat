-- Keep content-free acceptance records until expiry, even after inbox pruning.
CREATE TABLE envelope_acceptances (
    recipient_device_id BYTEA NOT NULL,
    sender_device_id BYTEA NOT NULL,
    idempotency_key BYTEA NOT NULL,
    envelope_id BYTEA NOT NULL,
    ciphertext_sha256 BYTEA NOT NULL,
    server_sequence BIGINT NOT NULL,
    accepted_at_ms BIGINT NOT NULL,
    expires_at_ms BIGINT NOT NULL,
    PRIMARY KEY (recipient_device_id, sender_device_id, idempotency_key)
);
CREATE INDEX envelope_acceptances_expiry ON envelope_acceptances(expires_at_ms);
INSERT INTO envelope_acceptances
    SELECT recipient_device_id, sender_device_id, idempotency_key, envelope_id,
           ciphertext_sha256, server_sequence, accepted_at_ms, expires_at_ms
    FROM inbox_messages;
