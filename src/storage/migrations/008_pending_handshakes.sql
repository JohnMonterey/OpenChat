-- Inbound MLS Welcomes for groups we have not joined are stashed here rather
-- than in the roster: crypto material is kept OUT of the contacts table until
-- the local user accepts. Keyed by the sender-chosen group id, each row records
-- who sent the Welcome, the envelope that carried it, the raw Welcome bytes, and
-- when it arrived. A later phase surfaces these as pending incoming requests and,
-- on accept, joins the group and deletes the stash.

CREATE TABLE pending_handshakes (
    conversation_id   BLOB PRIMARY KEY NOT NULL CHECK(length(conversation_id) = 16),
    sender_account_id BLOB NOT NULL CHECK(length(sender_account_id) = 16),
    sender_device_id  BLOB NOT NULL CHECK(length(sender_device_id) = 16),
    envelope_id       BLOB NOT NULL CHECK(length(envelope_id) = 16),
    welcome           BLOB NOT NULL,
    received_at_ms    INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS pending_handshakes_account ON pending_handshakes(sender_account_id);

PRAGMA user_version = 8;
