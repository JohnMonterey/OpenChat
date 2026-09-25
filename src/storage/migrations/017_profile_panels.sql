-- Custom profile panels (docs/profile-panels.md). Additive: every existing
-- page, blob and record is copied as it is.
--
-- 1. Media kinds 3 (panel picture) and 4 (video segment). SQLite cannot alter
--    a CHECK, so the two tables holding a kind are rebuilt. No table has a
--    foreign key to either (checked against PRAGMA foreign_key_list), so the
--    DROP cascades nowhere.
CREATE TABLE profile_media_017 (
    sha256        BLOB PRIMARY KEY NOT NULL CHECK(length(sha256) = 32),
    kind          INTEGER NOT NULL CHECK(kind BETWEEN 1 AND 15),
    data          BLOB NOT NULL CHECK(length(data) BETWEEN 1 AND 229376),
    created_at_ms INTEGER NOT NULL
);
INSERT INTO profile_media_017(sha256, kind, data, created_at_ms)
    SELECT sha256, kind, data, created_at_ms FROM profile_media;
DROP TABLE profile_media;
ALTER TABLE profile_media_017 RENAME TO profile_media;

CREATE TABLE contact_page_media_017 (
    account_id     BLOB NOT NULL CHECK(length(account_id) = 16),
    sha256         BLOB NOT NULL CHECK(length(sha256) = 32),
    kind           INTEGER NOT NULL CHECK(kind BETWEEN 1 AND 15),
    received_at_ms INTEGER NOT NULL,
    PRIMARY KEY(account_id, sha256)
);
INSERT INTO contact_page_media_017(account_id, sha256, kind, received_at_ms)
    SELECT account_id, sha256, kind, received_at_ms FROM contact_page_media;
DROP TABLE contact_page_media;
ALTER TABLE contact_page_media_017 RENAME TO contact_page_media;

-- 2. The blobs panels name, beside the background and song slot columns.
--    stage 0 is the draft, 1 the published page.
CREATE TABLE local_page_panel_media (
    profile_id BLOB NOT NULL REFERENCES local_profiles(profile_id) ON DELETE CASCADE,
    stage      INTEGER NOT NULL CHECK(stage IN (0, 1)),
    sha256     BLOB NOT NULL CHECK(length(sha256) = 32),
    PRIMARY KEY(profile_id, stage, sha256)
);

-- The blobs a contact's stored core names in its panels, with the kind it
-- names each as. Replaced whenever the core is.
CREATE TABLE contact_page_panel_media (
    account_id BLOB NOT NULL CHECK(length(account_id) = 16),
    sha256     BLOB NOT NULL CHECK(length(sha256) = 32),
    kind       INTEGER NOT NULL CHECK(kind BETWEEN 1 AND 15),
    PRIMARY KEY(account_id, sha256)
);

-- Every blob the local page names, and every blob a contact's stored core
-- names: the one test for "named" that every query uses.
CREATE VIEW local_page_named_media(profile_id, sha256) AS
    SELECT profile_id, draft_background FROM local_profile_page WHERE draft_background IS NOT NULL
    UNION ALL SELECT profile_id, draft_song FROM local_profile_page WHERE draft_song IS NOT NULL
    UNION ALL SELECT profile_id, published_background FROM local_profile_page
        WHERE published_background IS NOT NULL
    UNION ALL SELECT profile_id, published_song FROM local_profile_page WHERE published_song IS NOT NULL
    UNION ALL SELECT profile_id, sha256 FROM local_page_panel_media;

CREATE VIEW contact_page_named_media(account_id, sha256) AS
    SELECT account_id, background_sha256 FROM contact_pages WHERE background_sha256 IS NOT NULL
    UNION ALL SELECT account_id, song_sha256 FROM contact_pages WHERE song_sha256 IS NOT NULL
    UNION ALL SELECT account_id, sha256 FROM contact_page_panel_media;

-- 3. 1: stored by a client before panels, which kept no panel refs and may
--    have refused a core over 24 KiB. The page sync re-reads such cores once
--    and asks their owner once for anything newer (docs/profile-panels.md).
ALTER TABLE contact_pages ADD COLUMN format INTEGER NOT NULL DEFAULT 1;

PRAGMA user_version = 17;
