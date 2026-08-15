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
    let (result, sender) = bob.process(CONVERSATION, &encrypted).unwrap();
    assert_eq!(result, ProcessResult::Application(b"hello bob".to_vec()));
    // The surfaced credential is the sender's authenticated MLS identity.
    assert_eq!(sender, b"alice-device".to_vec());
}

#[test]
fn tampered_ciphertext_is_rejected_without_consuming_the_valid_message() {
    let (mut alice, mut bob) = joined_pair();
    let encrypted = alice.encrypt(CONVERSATION, b"authenticated").unwrap();
    let mut tampered = encrypted.clone();
    let last = tampered.len() - 1;
    tampered[last] ^= 0x80;

    assert!(bob.process(CONVERSATION, &tampered).is_err());
    let (result, sender) = bob.process(CONVERSATION, &encrypted).unwrap();
    assert_eq!(result, ProcessResult::Application(b"authenticated".to_vec()));
    assert_eq!(sender, b"alice-device".to_vec());
}

#[test]
fn application_messages_can_arrive_out_of_order_within_the_bound() {
    let (mut alice, mut bob) = joined_pair();
    let first = alice.encrypt(CONVERSATION, b"first").unwrap();
    let second = alice.encrypt(CONVERSATION, b"second").unwrap();

    let (second_result, _) = bob.process(CONVERSATION, &second).unwrap();
    assert_eq!(second_result, ProcessResult::Application(b"second".to_vec()));
    let (first_result, _) = bob.process(CONVERSATION, &first).unwrap();
    assert_eq!(first_result, ProcessResult::Application(b"first".to_vec()));
}

#[test]
fn removed_member_cannot_decrypt_the_next_epoch() {
    let (mut alice, mut bob) = joined_pair();
    let remove = alice
        .remove_members(CONVERSATION, &[b"bob-device".to_vec()])
        .unwrap();
    let (remove_result, _) = bob.process(CONVERSATION, &remove).unwrap();
    assert_eq!(remove_result, ProcessResult::Commit);

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
    let (bob_result, bob_sender) = bob.process(CONVERSATION, &encrypted).unwrap();
    assert_eq!(bob_result, ProcessResult::Application(b"hello group".to_vec()));
    assert_eq!(bob_sender, b"alice-device".to_vec());
    let (charlie_result, charlie_sender) = charlie.process(CONVERSATION, &encrypted).unwrap();
    assert_eq!(charlie_result, ProcessResult::Application(b"hello group".to_vec()));
    assert_eq!(charlie_sender, b"alice-device".to_vec());
}

#[test]
fn opaque_snapshot_restores_ratchet_state() {
    let (mut alice, mut bob) = joined_pair();
    let snapshot = bob.snapshot().unwrap();
    let mut restored = MlsClient::from_snapshot(&snapshot).unwrap();

    let encrypted = alice.encrypt(CONVERSATION, b"after restart").unwrap();
    let (restored_result, _) = restored.process(CONVERSATION, &encrypted).unwrap();
    assert_eq!(restored_result, ProcessResult::Application(b"after restart".to_vec()));

    // The original copy proves the snapshot represents the same pre-message state.
    let (bob_result, _) = bob.process(CONVERSATION, &encrypted).unwrap();
    assert_eq!(bob_result, ProcessResult::Application(b"after restart".to_vec()));
}
