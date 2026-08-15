-- OpenChat relay schema, migration 003: per-device ciphertext inboxes,
-- acknowledgement watermarks, opaque attachment metadata, and rate limits.
--
-- Envelopes are stored verbatim as opaque bytes. server_sequence is a global
-- monotonic sequence; each recipient inbox reads a strictly increasing
-- subsequence of it, which is all catch-up and watermarks require. Clients
-- never trust server_sequence for cryptographic ordering.

CREATE TABLE inbox_messages (
    server_sequence     BIGSERIAL PRIMARY KEY,
    recipient_device_id BYTEA NOT NULL CHECK (octet_length(recipient_device_id) = 16),
    envelope_id         BYTEA NOT NULL CHECK (octet_length(envelope_id) = 16),
    idempotency_key     BYTEA NOT NULL CHECK (octet_length(idempotency_key) = 16),
    sender_device_id    BYTEA NOT NULL CHECK (octet_length(sender_device_id) = 16),
    envelope            BYTEA NOT NULL CHECK (octet_length(envelope) BETWEEN 1 AND 1048576),
    ciphertext_sha256   BYTEA NOT NULL CHECK (octet_length(ciphertext_sha256) = 32),
    created_at_ms       BIGINT NOT NULL,
    accepted_at_ms      BIGINT NOT NULL,
    expires_at_ms       BIGINT NOT NULL,
    -- Idempotency is scoped per sender so two different senders cannot collide
    -- on a shared idempotency key and have one silently dropped.
    UNIQUE (recipient_device_id, sender_device_id, idempotency_key),
    UNIQUE (recipient_device_id, envelope_id)
);

CREATE INDEX inbox_messages_recipient_seq_idx
    ON inbox_messages (recipient_device_id, server_sequence);

CREATE TABLE device_watermarks (
    device_id      BYTEA PRIMARY KEY CHECK (octet_length(device_id) = 16),
    acked_sequence BIGINT NOT NULL DEFAULT 0,
    updated_at_ms  BIGINT NOT NULL
);

-- Attachment object metadata. The real filename/MIME/dimensions live encrypted
-- inside the MLS application message; the relay sees only an opaque metadata
-- blob, the ciphertext hash, a size, and an opaque object-store key.
CREATE TABLE attachments (
    attachment_id      BYTEA PRIMARY KEY CHECK (octet_length(attachment_id) = 16),
    uploader_device_id BYTEA NOT NULL CHECK (octet_length(uploader_device_id) = 16),
    encrypted_metadata BYTEA NOT NULL,
    ciphertext_sha256  BYTEA NOT NULL CHECK (octet_length(ciphertext_sha256) = 32),
    byte_count         BIGINT NOT NULL CHECK (byte_count >= 0),
    object_key         TEXT NOT NULL,
    created_at_ms      BIGINT NOT NULL,
    expires_at_ms      BIGINT NOT NULL,
    transfer_state     SMALLINT NOT NULL DEFAULT 0
);

CREATE TABLE rate_limits (
    subject         BYTEA NOT NULL,
    bucket          TEXT NOT NULL,
    window_start_ms BIGINT NOT NULL,
    counter         INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (subject, bucket, window_start_ms)
);
