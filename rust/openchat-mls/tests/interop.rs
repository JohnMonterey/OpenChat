use openchat_mls::{MlsClient, ProcessResult};

const CONVERSATION: [u8; 16] = *b"conversation-one";

fn joined_pair() -> (MlsClient, MlsClient) {
    let mut alice = MlsClient::new(b"alice-device").unwrap();
    let mut bob = MlsClient::new(b"bob-device").unwrap();
    let bob_key_package = bob.generate_key_package().unwrap();

    alice.create_group(CONVERSATION).unwrap();
    let add = alice
        .add_members(CONVERSATION, &[bob_key_package])
        .unwrap();
    bob.join_group(CONVERSATION, &add.welcome).unwrap();
    (alice, bob)
}

#[test]
fn two_clients_exchange_application_messages() {
    let (mut alice, mut bob) = joined_pair();
    let encrypted = alice.encrypt(CONVERSATION, b"hello bob").unwrap();
    assert_eq!(
        bob.process(CONVERSATION, &encrypted).unwrap(),
        ProcessResult::Application(b"hello bob".to_vec())
    );
}

#[test]
fn tampered_ciphertext_is_rejected_without_consuming_the_valid_message() {
    let (mut alice, mut bob) = joined_pair();
    let encrypted = alice.encrypt(CONVERSATION, b"authenticated").unwrap();
    let mut tampered = encrypted.clone();
    let last = tampered.len() - 1;
    tampered[last] ^= 0x80;

    assert!(bob.process(CONVERSATION, &tampered).is_err());
    assert_eq!(
        bob.process(CONVERSATION, &encrypted).unwrap(),
        ProcessResult::Application(b"authenticated".to_vec())
    );
}

#[test]
fn application_messages_can_arrive_out_of_order_within_the_bound() {
    let (mut alice, mut bob) = joined_pair();
    let first = alice.encrypt(CONVERSATION, b"first").unwrap();
    let second = alice.encrypt(CONVERSATION, b"second").unwrap();

    assert_eq!(
        bob.process(CONVERSATION, &second).unwrap(),
        ProcessResult::Application(b"second".to_vec())
    );
    assert_eq!(
        bob.process(CONVERSATION, &first).unwrap(),
        ProcessResult::Application(b"first".to_vec())
    );
}

#[test]
fn removed_member_cannot_decrypt_the_next_epoch() {
    let (mut alice, mut bob) = joined_pair();
    let remove = alice
        .remove_members(CONVERSATION, &[b"bob-device".to_vec()])
        .unwrap();
    assert_eq!(
        bob.process(CONVERSATION, &remove).unwrap(),
        ProcessResult::Commit
    );

    let after_removal = alice.encrypt(CONVERSATION, b"alice only").unwrap();
    assert!(bob.process(CONVERSATION, &after_removal).is_err());
}

#[test]
fn three_member_group_uses_the_same_encryption_flow() {
    let mut alice = MlsClient::new(b"alice-device").unwrap();
    let mut bob = MlsClient::new(b"bob-device").unwrap();
    let mut charlie = MlsClient::new(b"charlie-device").unwrap();
    let bob_key_package = bob.generate_key_package().unwrap();
    let charlie_key_package = charlie.generate_key_package().unwrap();

    alice.create_group(CONVERSATION).unwrap();
    let add = alice
        .add_members(CONVERSATION, &[bob_key_package, charlie_key_package])
        .unwrap();
    bob.join_group(CONVERSATION, &add.welcome).unwrap();
    charlie.join_group(CONVERSATION, &add.welcome).unwrap();

    let encrypted = alice.encrypt(CONVERSATION, b"hello group").unwrap();
    assert_eq!(
        bob.process(CONVERSATION, &encrypted).unwrap(),
        ProcessResult::Application(b"hello group".to_vec())
    );
    assert_eq!(
        charlie.process(CONVERSATION, &encrypted).unwrap(),
        ProcessResult::Application(b"hello group".to_vec())
    );
}

#[test]
fn opaque_snapshot_restores_ratchet_state() {
    let (mut alice, mut bob) = joined_pair();
    let snapshot = bob.snapshot().unwrap();
    let mut restored = MlsClient::from_snapshot(&snapshot).unwrap();

    let encrypted = alice.encrypt(CONVERSATION, b"after restart").unwrap();
    assert_eq!(
        restored.process(CONVERSATION, &encrypted).unwrap(),
        ProcessResult::Application(b"after restart".to_vec())
    );

    // The original copy proves the snapshot represents the same pre-message state.
    assert_eq!(
        bob.process(CONVERSATION, &encrypted).unwrap(),
        ProcessResult::Application(b"after restart".to_vec())
    );
}
