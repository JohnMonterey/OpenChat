-- OpenChat relay schema, migration 006: username + password accounts.
--
-- The relay never sees a user's password. The client stretches it locally and
-- sends only a 32-byte password key; the relay stores a second, salted Argon2id
-- hash of that key. So a leaked table yields neither the password nor anything
-- that can be replayed as a login, and cracking one row costs both stretches.
--
-- Every column is nullable because accounts created before this migration have
-- no password. Such an account keeps authenticating its existing device by
-- signed challenge, but it can never be logged in to by password and its handle
-- stays reserved.
--
--   password_hash    Argon2id(password key, password_salt), 32 bytes
--   password_salt    16 random bytes, unique per account
--   password_params  the relay-side Argon2id cost the hash was made with, so the
--                    cost can be raised later without invalidating old rows
--   password_kdf     version of the CLIENT-side stretch that produced the key

ALTER TABLE accounts ADD COLUMN password_hash BYTEA
    CHECK (password_hash IS NULL OR octet_length(password_hash) = 32);
ALTER TABLE accounts ADD COLUMN password_salt BYTEA
    CHECK (password_salt IS NULL OR octet_length(password_salt) = 16);
ALTER TABLE accounts ADD COLUMN password_params TEXT
    CHECK (password_params IS NULL OR char_length(password_params) BETWEEN 1 AND 64);
ALTER TABLE accounts ADD COLUMN password_kdf INTEGER;

-- All four are set together or not at all.
ALTER TABLE accounts ADD CONSTRAINT accounts_password_all_or_none CHECK (
    (password_hash IS NULL AND password_salt IS NULL
        AND password_params IS NULL AND password_kdf IS NULL)
    OR (password_hash IS NOT NULL AND password_salt IS NOT NULL
        AND password_params IS NOT NULL AND password_kdf IS NOT NULL)
);
