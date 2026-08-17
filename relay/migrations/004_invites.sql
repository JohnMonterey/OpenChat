-- OpenChat relay schema, migration 004: one-time invite links.
--
-- An invite lets a registered account hand a single-use token to someone who
-- then redeems it to discover the inviter's account and active devices. As with
-- auth and refresh tokens, only the SHA-256 hash of the token is stored; the
-- plaintext token is returned to the creator exactly once and never persisted.
-- Redemption is single-use and expiry-guarded via a RETURNING update, so a
-- token is redeemable at most once and never after it expires.

CREATE TABLE invites (
    token_hash     BYTEA PRIMARY KEY CHECK (octet_length(token_hash) = 32),
    account_id     BYTEA NOT NULL REFERENCES accounts(account_id) ON DELETE CASCADE,
    created_at_ms  BIGINT NOT NULL,
    expires_at_ms  BIGINT NOT NULL,
    consumed_at_ms BIGINT
);

CREATE INDEX invites_account_idx ON invites (account_id);
CREATE INDEX invites_expiry_idx ON invites (expires_at_ms);
