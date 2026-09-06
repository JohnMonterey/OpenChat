-- Existing history predates local read tracking and remains read.
ALTER TABLE messages ADD COLUMN locally_read INTEGER NOT NULL DEFAULT 1 CHECK(locally_read IN (0, 1));
CREATE INDEX messages_unread ON messages(conversation_id) WHERE locally_read = 0;
PRAGMA user_version = 14;
