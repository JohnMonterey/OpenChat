-- Profile pages. The local user's page (the editor's draft and the page last
-- published to contacts) and the page each contact published. Media blobs are
-- stored once by SHA-256 in profile_media, but WHICH contact sent WHICH blob is
-- recorded per contact (contact_page_media): presence, "missing" and requests
-- are always computed per owner, so one contact can never learn whether we
-- hold another contact's picture, and a blob is only ever used in the slot and
-- kind its own sender declared.
--
-- A page travels as a PageCore plus one PageMedia per blob inside
-- ProfileUpdate envelopes (see domain/ProfilePageCodec.h). Cores are stored as
-- the exact encoded message, so what was sent can be sent again byte for byte.
--
-- No table here has a foreign key to contacts. migrate() runs with
-- foreign_keys=ON inside one transaction, so if a later migration rebuilt
-- contacts the way 005 rebuilt outbox (CREATE, copy, DROP, RENAME), the DROP
-- would cascade and silently wipe every page row. Rows of accounts that are no
-- longer Accepted contacts are removed explicitly instead
-- (ProfilePageRepository::dropPagesOfNonContacts, run by every collection).

-- The account's @handle, stamped at sign-up/login. Empty for profiles made
-- before this migration until the relay's reverse lookup fills it in.
ALTER TABLE local_profiles ADD COLUMN handle TEXT NOT NULL DEFAULT '';

CREATE TABLE profile_media (
    sha256        BLOB PRIMARY KEY NOT NULL CHECK(length(sha256) = 32),
    kind          INTEGER NOT NULL CHECK(kind IN (1, 2)),
    data          BLOB NOT NULL CHECK(length(data) BETWEEN 1 AND 229376),
    created_at_ms INTEGER NOT NULL
);

CREATE TABLE local_profile_page (
    profile_id           BLOB PRIMARY KEY NOT NULL
                         REFERENCES local_profiles(profile_id) ON DELETE CASCADE,
    draft_core           BLOB,
    draft_background     BLOB CHECK(draft_background IS NULL OR length(draft_background) = 32),
    draft_song           BLOB CHECK(draft_song IS NULL OR length(draft_song) = 32),
    -- Editor-only: the song's source file path and chosen window, as JSON.
    -- Local convenience for re-trimming; never encoded into a core or sent.
    draft_song_source    TEXT,
    draft_updated_at_ms  INTEGER NOT NULL DEFAULT 0,
    published_core       BLOB,
    published_revision   INTEGER NOT NULL DEFAULT 0 CHECK(published_revision >= 0),
    published_background BLOB CHECK(published_background IS NULL OR length(published_background) = 32),
    published_song       BLOB CHECK(published_song IS NULL OR length(published_song) = 32),
    published_at_ms      INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE contact_pages (
    account_id        BLOB PRIMARY KEY NOT NULL CHECK(length(account_id) = 16),
    revision          INTEGER NOT NULL CHECK(revision >= 0),
    core              BLOB NOT NULL,
    background_sha256 BLOB CHECK(background_sha256 IS NULL OR length(background_sha256) = 32),
    song_sha256       BLOB CHECK(song_sha256 IS NULL OR length(song_sha256) = 32),
    received_at_ms    INTEGER NOT NULL,
    viewed_at_ms      INTEGER NOT NULL DEFAULT 0
);

-- Each blob a contact sent us, with the kind it arrived as. A row the
-- contact's stored core does not name is "pending" (media can overtake its
-- core in transit): at most two per contact, dropped after a day unless a
-- core adopts it.
CREATE TABLE contact_page_media (
    account_id     BLOB NOT NULL CHECK(length(account_id) = 16),
    sha256         BLOB NOT NULL CHECK(length(sha256) = 32),
    kind           INTEGER NOT NULL CHECK(kind IN (1, 2)),
    received_at_ms INTEGER NOT NULL,
    PRIMARY KEY(account_id, sha256)
);

-- Owner side: what each contact has been sent. device_id is the peer device
-- the rows were recorded against; when the contact's device changes the
-- rows are void and the page is delivered again. page_capable: the contact
-- has sent us any page message, so pushing media to them is not wasted.
CREATE TABLE page_deliveries (
    account_id             BLOB PRIMARY KEY NOT NULL CHECK(length(account_id) = 16),
    device_id              BLOB CHECK(device_id IS NULL OR length(device_id) = 16),
    sent_revision          INTEGER NOT NULL DEFAULT -1,
    sent_at_ms             INTEGER NOT NULL DEFAULT 0,
    page_capable           INTEGER NOT NULL DEFAULT 0 CHECK(page_capable IN (0, 1)),
    last_answer_at_ms      INTEGER NOT NULL DEFAULT 0,
    answer_window_start_ms INTEGER NOT NULL DEFAULT 0,
    answers_in_window      INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE page_delivery_media (
    account_id BLOB NOT NULL CHECK(length(account_id) = 16),
    sha256     BLOB NOT NULL CHECK(length(sha256) = 32),
    sent_at_ms INTEGER NOT NULL,
    PRIMARY KEY(account_id, sha256)
);

-- Viewer side: when we last asked a contact for their page, how many times in
-- a row nothing came back (an older client never answers), and the core
-- revision whose missing media we already asked for.
CREATE TABLE page_requests (
    account_id               BLOB PRIMARY KEY NOT NULL CHECK(length(account_id) = 16),
    last_request_at_ms       INTEGER NOT NULL DEFAULT 0,
    unanswered               INTEGER NOT NULL DEFAULT 0,
    media_requested_revision INTEGER NOT NULL DEFAULT -1
);

-- One-time cleanup. Settled control envelopes (receipts, profile updates, call
-- signals: Accepted 2 or Failed 3, with no messages row) are never read again;
-- until 0.2.9 they were kept forever. The engine now deletes them as they
-- settle (SqlCipherSyncStore::markAccepted/failSend/failEnvelope).
DELETE FROM outbox WHERE state IN (2, 3)
    AND NOT EXISTS (SELECT 1 FROM messages WHERE messages.id = outbox.message_id);

PRAGMA user_version = 16;
