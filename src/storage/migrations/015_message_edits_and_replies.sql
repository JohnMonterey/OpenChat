-- Replies and edits name a message by the id both ends derive from its
-- ciphertext. Existing history carries ids the peer never saw, so nothing can
-- refer to it: shared_id stays 0 there.
ALTER TABLE messages ADD COLUMN shared_id INTEGER NOT NULL DEFAULT 0 CHECK(shared_id IN (0, 1));
-- When the sender last changed the text; 0 for a message never edited.
ALTER TABLE messages ADD COLUMN edited_at_ms INTEGER NOT NULL DEFAULT 0;
-- A reply's quote as it travelled: who wrote the answered message, and an
-- excerpt of it (reply_to_id names the message itself).
ALTER TABLE messages ADD COLUMN quoted_sender_device_id BLOB;
ALTER TABLE messages ADD COLUMN quoted_body TEXT;
PRAGMA user_version = 15;
