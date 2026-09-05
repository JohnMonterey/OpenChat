-- Self-published profiles. The local user's chosen presence, status line and
-- JPEG picture live on their profile row; each contact's row keeps the last
-- ProfileUpdate that contact sent. Presence is a models/Contact.h Presence
-- value (0 Available, 1 Away, 2 Offline, 3 Busy). A NULL picture means none.
ALTER TABLE local_profiles ADD COLUMN presence INTEGER NOT NULL DEFAULT 0;
ALTER TABLE local_profiles ADD COLUMN status_text TEXT NOT NULL DEFAULT '';
ALTER TABLE local_profiles ADD COLUMN avatar_jpeg BLOB;

ALTER TABLE contacts ADD COLUMN presence INTEGER NOT NULL DEFAULT 0;
ALTER TABLE contacts ADD COLUMN status_text TEXT NOT NULL DEFAULT '';
ALTER TABLE contacts ADD COLUMN avatar_jpeg BLOB;

PRAGMA user_version = 12;
