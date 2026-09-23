-- OpenChat relay schema, migration 007: collectible cosmetics.
--
-- The one deliberate exception to "the relay stores only ciphertext and
-- routing material". What an account has unboxed and what it wears are public
-- profile decoration, not message content, and the relay is their authority:
-- it counts the connected time cases drop from, draws every reward, and only
-- lets an account wear what it owns, so no client can show others something it
-- never unboxed. Nothing here is secret: catalogue ids (domain/CosmeticRules),
-- counters and times.
--
--   cosmetic_claims    every case opened, keyed by the client's request id so a
--                      retried claim returns the same case instead of a second
--   cosmetic_cases     per account: cases waiting to be opened, connected time
--                      counted toward the next, the last case opened, and
--                      whether a collection kept on a device was imported
--   cosmetic_items     what each account owns
--   cosmetic_loadouts  what each account wears, one item per slot; any signed-in
--                      account may read these, like the directory

CREATE TABLE cosmetic_claims (
    claim_id      BYTEA PRIMARY KEY CHECK (octet_length(claim_id) = 16),
    account_id    BYTEA NOT NULL REFERENCES accounts(account_id) ON DELETE CASCADE,
    request_id    BYTEA NOT NULL CHECK (octet_length(request_id) BETWEEN 8 AND 32),
    reward_id     TEXT NOT NULL CHECK (char_length(reward_id) BETWEEN 1 AND 64),
    seed          BIGINT NOT NULL CHECK (seed >= 0),
    case_key      TEXT NOT NULL CHECK (char_length(case_key) BETWEEN 1 AND 80),
    claimed_at_ms BIGINT NOT NULL,
    UNIQUE (account_id, request_id)
);

CREATE TABLE cosmetic_cases (
    account_id    BYTEA PRIMARY KEY REFERENCES accounts(account_id) ON DELETE CASCADE,
    drops         INTEGER NOT NULL CHECK (drops >= 0),
    progress_ms   BIGINT NOT NULL CHECK (progress_ms >= 0),
    last_claim_id BYTEA REFERENCES cosmetic_claims(claim_id) ON DELETE SET NULL,
    imported      BOOLEAN NOT NULL DEFAULT FALSE,
    updated_at_ms BIGINT NOT NULL
);

CREATE TABLE cosmetic_items (
    account_id     BYTEA NOT NULL REFERENCES accounts(account_id) ON DELETE CASCADE,
    item_id        TEXT NOT NULL CHECK (char_length(item_id) BETWEEN 1 AND 64),
    source         TEXT NOT NULL CHECK (source IN ('case', 'grant', 'import')),
    acquired_at_ms BIGINT NOT NULL,
    PRIMARY KEY (account_id, item_id)
);

CREATE TABLE cosmetic_loadouts (
    account_id BYTEA NOT NULL,
    slot       TEXT NOT NULL CHECK (slot IN ('frame', 'bead', 'flair', 'scene', 'bubble')),
    item_id    TEXT NOT NULL,
    PRIMARY KEY (account_id, slot),
    FOREIGN KEY (account_id, item_id) REFERENCES cosmetic_items (account_id, item_id)
        ON DELETE CASCADE
);
