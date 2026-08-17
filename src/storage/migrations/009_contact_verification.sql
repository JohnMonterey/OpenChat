-- Phase 9: bind a safety number to each contact and remember whether the user
-- verified it out-of-band. peer_signing_key is the peer's MLS-authenticated
-- Ed25519 identity key (credential bytes 17..49); NULL until known (sender: at
-- add; receiver: at accept). verified is a local user assertion, default off.
ALTER TABLE contacts ADD COLUMN peer_signing_key BLOB
    CHECK(peer_signing_key IS NULL OR length(peer_signing_key) = 32);
ALTER TABLE contacts ADD COLUMN verified INTEGER NOT NULL DEFAULT 0;

PRAGMA user_version = 9;
