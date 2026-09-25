-- Chat attachments (docs/chat-attachments.md). A photo, video, audio clip or
-- file is one visible message (content_kind 2) whose descriptor lives here, in
-- both directions; its bytes travel as sealed AttachmentControl frames and are
-- kept, still sealed, in files under the profile directory, never in this
-- database.
--
-- Nothing here has a foreign key. Frames may arrive before the message that
-- describes them (attachment_transfers holds what came first), and rows are
-- removed explicitly, as 016 explains for the page tables. Blob columns come
-- last: every lookup reads the small columns without walking the overflow
-- pages of a preview.

-- One row per attachment message. key is the frames' AES-256-GCM key; the
-- whole database is SQLCipher. state: 0 transferring, 1 complete, 2 failed,
-- 3 cancelled. reason: 0 none, 1 invalid, 2 no space, 3 sender cancelled,
-- 4 could not send. parts_sent and preview_sent count frames handed to the
-- engine (outgoing rows only). recipients: the 16-byte device ids the message
-- went to, back to back (outgoing only), so later members never get the bytes.
CREATE TABLE IF NOT EXISTS message_attachments (
    message_id       BLOB PRIMARY KEY NOT NULL CHECK(length(message_id) = 16),
    conversation_id  BLOB NOT NULL CHECK(length(conversation_id) = 16),
    sender_device_id BLOB NOT NULL CHECK(length(sender_device_id) = 16),
    attachment_id    BLOB NOT NULL CHECK(length(attachment_id) = 16),
    kind             INTEGER NOT NULL CHECK(kind BETWEEN 1 AND 15),
    byte_count       INTEGER NOT NULL CHECK(byte_count >= 1),
    part_count       INTEGER NOT NULL CHECK(part_count >= 1),
    width            INTEGER NOT NULL DEFAULT 0,
    height           INTEGER NOT NULL DEFAULT 0,
    duration_ms      INTEGER NOT NULL DEFAULT 0,
    has_preview      INTEGER NOT NULL DEFAULT 0,
    state            INTEGER NOT NULL DEFAULT 0,
    reason           INTEGER NOT NULL DEFAULT 0,
    parts_sent       INTEGER NOT NULL DEFAULT 0,
    preview_sent     INTEGER NOT NULL DEFAULT 0,
    created_at_ms    INTEGER NOT NULL,
    mime_type        TEXT NOT NULL DEFAULT '',
    file_name        TEXT NOT NULL DEFAULT '',
    sha256           BLOB NOT NULL CHECK(length(sha256) = 32),
    attachment_key   BLOB NOT NULL CHECK(length(attachment_key) = 32),
    recipients       BLOB,
    peaks            BLOB,
    preview          BLOB,
    UNIQUE(conversation_id, sender_device_id, attachment_id)
);
CREATE INDEX IF NOT EXISTS message_attachments_active ON message_attachments(state, sender_device_id);

-- What has arrived per (conversation, sender, attachment), with or without a
-- descriptor yet. present is a bitmap of the parts held (bit i of byte i/8);
-- sealed_preview is a preview frame's body that came before its descriptor.
CREATE TABLE IF NOT EXISTS attachment_transfers (
    conversation_id  BLOB NOT NULL CHECK(length(conversation_id) = 16),
    sender_device_id BLOB NOT NULL CHECK(length(sender_device_id) = 16),
    attachment_id    BLOB NOT NULL CHECK(length(attachment_id) = 16),
    have_count       INTEGER NOT NULL DEFAULT 0,
    have_bytes       INTEGER NOT NULL DEFAULT 0,
    first_seen_ms    INTEGER NOT NULL,
    updated_at_ms    INTEGER NOT NULL,
    last_request_ms  INTEGER NOT NULL DEFAULT 0,
    requests_sent    INTEGER NOT NULL DEFAULT 0,
    present          BLOB NOT NULL,
    sealed_preview   BLOB,
    PRIMARY KEY(conversation_id, sender_device_id, attachment_id)
);
CREATE INDEX IF NOT EXISTS attachment_transfers_seen ON attachment_transfers(first_seen_ms);

-- Attachment frames wait behind everything else: claimDue orders by priority
-- first, so texts, receipts and call signals always leave before them.
ALTER TABLE outbox ADD COLUMN priority INTEGER NOT NULL DEFAULT 0;
PRAGMA user_version = 18;
