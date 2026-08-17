-- The contact roster is the durable set of peers the local user is connected to
-- or has a pending connection with, keyed by the peer's stable directory
-- AccountId. Its state column drives an explicit lifecycle (pending outgoing,
-- pending incoming, accepted, blocked). conversation_id is a plain nullable blob
-- rather than a foreign key to conversations: a pending contact has no MLS group
-- row yet, and the group linkage is wired in a later phase.

CREATE TABLE contacts (
    account_id BLOB PRIMARY KEY NOT NULL CHECK(length(account_id) = 16),
    handle TEXT NOT NULL DEFAULT '',
    display_name TEXT NOT NULL DEFAULT '',
    state INTEGER NOT NULL,
    conversation_id BLOB CHECK(conversation_id IS NULL OR length(conversation_id) = 16),
    created_at_ms INTEGER NOT NULL,
    updated_at_ms INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS contacts_handle ON contacts(handle);

PRAGMA user_version = 7;
