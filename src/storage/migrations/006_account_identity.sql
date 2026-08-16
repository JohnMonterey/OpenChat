-- The durable SyncEngine needs a stable, per-profile AccountId to address the
-- relay. It is generated once when the profile is created and then never
-- changes, so it lives in its own single-row table keyed by profile_id, in the
-- same spirit as local_mls_state. The row is torn down with the profile via the
-- foreign key cascade.

CREATE TABLE local_account_identity (
    profile_id BLOB PRIMARY KEY NOT NULL REFERENCES local_profiles(profile_id) ON DELETE CASCADE,
    account_id BLOB NOT NULL CHECK(length(account_id) = 16)
);

PRAGMA user_version = 6;
