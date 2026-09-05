-- Group chats. A group is a conversations row of kind Group (1) whose title is
-- the name the members gave it; its roster lives here, one row per other
-- member, keyed by the member's device (the recipient of every envelope sent
-- into the group). display_name is the name the inviter knew the member by,
-- used only when the member is not a local contact. left_at_ms marks a group
-- the local user has left: it stays hidden rather than deleted, so a message
-- still in flight from a member who has not yet heard can be stored without
-- violating the messages foreign key.

CREATE TABLE group_members (
    conversation_id BLOB NOT NULL REFERENCES conversations(id) ON DELETE CASCADE,
    account_id      BLOB NOT NULL CHECK(length(account_id) = 16),
    device_id       BLOB NOT NULL CHECK(length(device_id) = 16),
    display_name    TEXT NOT NULL DEFAULT '',
    joined_at_ms    INTEGER NOT NULL,
    PRIMARY KEY(conversation_id, device_id)
);

ALTER TABLE conversations ADD COLUMN left_at_ms INTEGER NOT NULL DEFAULT 0;

PRAGMA user_version = 13;
