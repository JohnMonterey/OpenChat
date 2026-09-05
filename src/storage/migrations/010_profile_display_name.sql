ALTER TABLE local_profiles ADD COLUMN display_name TEXT NOT NULL DEFAULT '';

PRAGMA user_version = 10;
