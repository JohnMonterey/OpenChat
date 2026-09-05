-- The peer device behind each contact's 2-party MLS group. Application
-- messages are addressed device-to-device, so the chat layer needs the device
-- that sealed (sender side: claimed KeyPackage) or shipped (receiver side:
-- Welcome) the handshake. Bound at request time on both sides; NULL for rows
-- written before this migration.
ALTER TABLE contacts ADD COLUMN peer_device_id BLOB
    CHECK(peer_device_id IS NULL OR length(peer_device_id) = 16);

PRAGMA user_version = 11;
