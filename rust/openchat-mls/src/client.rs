use openmls::prelude::{
    BasicCredential, Ciphersuite, CredentialWithKey, GroupId, KeyPackage, KeyPackageIn,
    MlsGroup, MlsGroupCreateConfig, MlsGroupJoinConfig, MlsMessageBodyIn, MlsMessageIn,
    ProcessedMessageContent, ProtocolVersion, SenderRatchetConfiguration, StagedWelcome,
    PURE_CIPHERTEXT_WIRE_FORMAT_POLICY,
};
use openmls_basic_credential::SignatureKeyPair;
use openmls_rust_crypto::OpenMlsRustCrypto;
use openmls_traits::{OpenMlsProvider, types::SignatureScheme};
use std::panic::{AssertUnwindSafe, catch_unwind};
use tls_codec::{Deserialize as TlsDeserialize, Serialize as TlsSerialize};
use zeroize::Zeroize;

use crate::storage;

const CIPHERSUITE: Ciphersuite =
    Ciphersuite::MLS_128_DHKEMX25519_CHACHA20POLY1305_SHA256_Ed25519;
const MAX_IDENTITY_BYTES: usize = 1024;
const MAX_INPUT_BYTES: usize = 1024 * 1024;
const MAX_PLAINTEXT_BYTES: usize = 256 * 1024;
const MAX_MEMBERS_PER_CHANGE: usize = 256;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(i32)]
pub enum MlsError {
    InvalidInput = 1,
    MissingGroup = 2,
    InvalidMessage = 3,
    Crypto = 4,
    Storage = 5,
    Unsupported = 6,
    Internal = 7,
}

#[derive(Debug, Eq, PartialEq)]
pub enum ProcessResult {
    Application(Vec<u8>),
    Proposal,
    Commit,
}

#[derive(Debug, Eq, PartialEq)]
pub struct AddMembersOutput {
    pub commit: Vec<u8>,
    pub welcome: Vec<u8>,
}

pub struct MlsClient {
    provider: OpenMlsRustCrypto,
    signer: SignatureKeyPair,
    credential: CredentialWithKey,
    identity: Vec<u8>,
}

impl MlsClient {
    pub fn new(identity: &[u8]) -> Result<Self, MlsError> {
        if identity.is_empty() || identity.len() > MAX_IDENTITY_BYTES {
            return Err(MlsError::InvalidInput);
        }
        let provider = OpenMlsRustCrypto::default();
        let signer = SignatureKeyPair::new(SignatureScheme::ED25519)
            .map_err(|_| MlsError::Crypto)?;
        signer
            .store(provider.storage())
            .map_err(|_| MlsError::Storage)?;
        let credential = credential(identity, signer.public());
        Ok(Self {
            provider,
            signer,
            credential,
            identity: identity.to_vec(),
        })
    }

    pub fn from_snapshot(snapshot: &[u8]) -> Result<Self, MlsError> {
        let restored = storage::restore(snapshot)?;
        if restored.identity.len() > MAX_IDENTITY_BYTES
            || restored.signer_public_key.len() != 32
        {
            return Err(MlsError::InvalidInput);
        }

        let (identity, signer_public_key, values) = restored.into_parts();
        let provider = OpenMlsRustCrypto::default();
        *provider
            .storage()
            .values
            .write()
            .map_err(|_| MlsError::Storage)? = values;
        let signer = SignatureKeyPair::read(
            provider.storage(),
            &signer_public_key,
            SignatureScheme::ED25519,
        );
        let Some(signer) = signer else {
            if let Ok(mut values) = provider.storage().values.write() {
                storage::wipe_values(&mut values);
            }
            return Err(MlsError::Storage);
        };
        let credential = credential(&identity, signer.public());
        Ok(Self {
            provider,
            signer,
            credential,
            identity,
        })
    }

    pub fn snapshot(&self) -> Result<Vec<u8>, MlsError> {
        storage::snapshot(&self.provider, &self.identity, self.signer.public())
    }

    pub(crate) fn identity(&self) -> &[u8] {
        &self.identity
    }

    pub fn generate_key_package(&mut self) -> Result<Vec<u8>, MlsError> {
        let bundle = KeyPackage::builder()
            .build(
                CIPHERSUITE,
                &self.provider,
                &self.signer,
                self.credential.clone(),
            )
            .map_err(|_| MlsError::Crypto)?;
        serialize(bundle.key_package())
    }

    pub fn create_group(&mut self, conversation: [u8; 16]) -> Result<(), MlsError> {
        let group_id = group_id(conversation);
        if MlsGroup::load(self.provider.storage(), &group_id)
            .map_err(|_| MlsError::Storage)?
            .is_some()
        {
            return Err(MlsError::InvalidInput);
        }
        let config = MlsGroupCreateConfig::builder()
            .ciphersuite(CIPHERSUITE)
            .wire_format_policy(PURE_CIPHERTEXT_WIRE_FORMAT_POLICY)
            .sender_ratchet_configuration(SenderRatchetConfiguration::new(32, 1000))
            .use_ratchet_tree_extension(true)
            .build();
        MlsGroup::new_with_group_id(
            &self.provider,
            &self.signer,
            &config,
            group_id,
            self.credential.clone(),
        )
        .map_err(|_| MlsError::Crypto)?;
        Ok(())
    }

    pub fn join_group(
        &mut self,
        conversation: [u8; 16],
        welcome_message: &[u8],
    ) -> Result<(), MlsError> {
        bounded(welcome_message, MAX_INPUT_BYTES)?;
        let message = deserialize_message(welcome_message)?;
        let welcome = match message.extract() {
            MlsMessageBodyIn::Welcome(welcome) => welcome,
            _ => return Err(MlsError::InvalidMessage),
        };
        let config = MlsGroupJoinConfig::builder()
            .wire_format_policy(PURE_CIPHERTEXT_WIRE_FORMAT_POLICY)
            .sender_ratchet_configuration(SenderRatchetConfiguration::new(32, 1000))
            .use_ratchet_tree_extension(true)
            .build();
        let group = StagedWelcome::new_from_welcome(&self.provider, &config, welcome, None)
            .map_err(|_| MlsError::InvalidMessage)?
            .into_group(&self.provider)
            .map_err(|_| MlsError::Storage)?;
        if group.group_id().as_slice() != conversation {
            return Err(MlsError::InvalidMessage);
        }
        Ok(())
    }

    /// Read-only inspection of an MLS Welcome: returns the credential identities
    /// of the group's other members (excluding self) WITHOUT durably joining.
    /// All storage mutations from staging and into_group are rolled back via
    /// StorageBackup, so this has no net effect on the provider -- the caller
    /// can still join_group later with the same Welcome.
    pub fn inspect_welcome(&self, welcome_message: &[u8]) -> Result<Vec<Vec<u8>>, MlsError> {
        bounded(welcome_message, MAX_INPUT_BYTES)?;
        let backup = storage::StorageBackup::capture(&self.provider)?;
        let result = (|| {
            let message = deserialize_message(welcome_message)?;
            let welcome = match message.extract() {
                MlsMessageBodyIn::Welcome(welcome) => welcome,
                _ => return Err(MlsError::InvalidMessage),
            };
            let config = MlsGroupJoinConfig::builder()
                .wire_format_policy(PURE_CIPHERTEXT_WIRE_FORMAT_POLICY)
                .sender_ratchet_configuration(SenderRatchetConfiguration::new(32, 1000))
                .use_ratchet_tree_extension(true)
                .build();
            let group = StagedWelcome::new_from_welcome(&self.provider, &config, welcome, None)
                .map_err(|_| MlsError::InvalidMessage)?
                .into_group(&self.provider)
                .map_err(|_| MlsError::Storage)?;
            let mut others = Vec::new();
            for member in group.members() {
                let identity = BasicCredential::try_from(member.credential.clone())
                    .map(|credential| credential.identity().to_vec())
                    .map_err(|_| MlsError::InvalidMessage)?;
                if identity != self.identity {
                    others.push(identity);
                }
            }
            Ok(others)
        })();
        // Always restore, on success or failure: inspection never persists.
        backup.restore(&self.provider)?;
        result
    }

    pub fn add_members(
        &mut self,
        conversation: [u8; 16],
        serialized_key_packages: &[Vec<u8>],
    ) -> Result<AddMembersOutput, MlsError> {
        if serialized_key_packages.is_empty()
            || serialized_key_packages.len() > MAX_MEMBERS_PER_CHANGE
        {
            return Err(MlsError::InvalidInput);
        }
        let total_bytes = serialized_key_packages.iter().try_fold(0usize, |total, value| {
            total.checked_add(value.len()).ok_or(MlsError::InvalidInput)
        })?;
        if total_bytes > MAX_INPUT_BYTES {
            return Err(MlsError::InvalidInput);
        }
        let mut key_packages = Vec::with_capacity(serialized_key_packages.len());
        for encoded in serialized_key_packages {
            bounded(encoded, MAX_INPUT_BYTES)?;
            let mut input = encoded.as_slice();
            let key_package = KeyPackageIn::tls_deserialize(&mut input)
                .map_err(|_| MlsError::InvalidMessage)?;
            if !input.is_empty() {
                return Err(MlsError::InvalidMessage);
            }
            key_packages.push(
                key_package
                    .validate(self.provider.crypto(), ProtocolVersion::Mls10)
                    .map_err(|_| MlsError::InvalidMessage)?,
            );
        }

        let mut group = self.load_group(conversation)?;
        let (commit, welcome, _) = group
            .add_members(&self.provider, &self.signer, &key_packages)
            .map_err(|_| MlsError::Crypto)?;
        let commit = serialize(&commit)?;
        let welcome = serialize(&welcome)?;
        group
            .merge_pending_commit(&self.provider)
            .map_err(|_| MlsError::Storage)?;
        Ok(AddMembersOutput { commit, welcome })
    }

    pub fn remove_members(
        &mut self,
        conversation: [u8; 16],
        identities: &[Vec<u8>],
    ) -> Result<Vec<u8>, MlsError> {
        if identities.is_empty() || identities.len() > MAX_MEMBERS_PER_CHANGE {
            return Err(MlsError::InvalidInput);
        }
        let mut group = self.load_group(conversation)?;
        let mut leaf_indices = Vec::with_capacity(identities.len());
        for identity in identities {
            bounded(identity, MAX_IDENTITY_BYTES)?;
            let member = group
                .members()
                .find(|member| {
                    BasicCredential::try_from(member.credential.clone())
                        .is_ok_and(|credential| credential.identity() == identity)
                })
                .ok_or(MlsError::InvalidInput)?;
            leaf_indices.push(member.index);
        }
        leaf_indices.sort_unstable_by_key(|index| index.u32());
        leaf_indices.dedup();
        if leaf_indices.len() != identities.len() {
            return Err(MlsError::InvalidInput);
        }

        let (commit, _, _) = group
            .remove_members(&self.provider, &self.signer, &leaf_indices)
            .map_err(|_| MlsError::Crypto)?;
        let commit = serialize(&commit)?;
        group
            .merge_pending_commit(&self.provider)
            .map_err(|_| MlsError::Storage)?;
        Ok(commit)
    }

    pub fn encrypt(
        &mut self,
        conversation: [u8; 16],
        plaintext: &[u8],
    ) -> Result<Vec<u8>, MlsError> {
        bounded(plaintext, MAX_PLAINTEXT_BYTES)?;
        let mut group = self.load_group(conversation)?;
        let message = group
            .create_message(&self.provider, &self.signer, plaintext)
            .map_err(|_| MlsError::Crypto)?;
        serialize(&message)
    }

    pub fn process(
        &mut self,
        conversation: [u8; 16],
        message: &[u8],
    ) -> Result<(ProcessResult, Vec<u8>), MlsError> {
        bounded(message, MAX_INPUT_BYTES)?;
        let message = deserialize_message(message)?
            .try_into_protocol_message()
            .map_err(|_| MlsError::InvalidMessage)?;
        let mut group = self.load_group(conversation)?;
        if message.group_id().as_slice() != conversation {
            return Err(MlsError::InvalidMessage);
        }
        let previous_storage = storage::StorageBackup::capture(&self.provider)?;
        let processed = match catch_unwind(AssertUnwindSafe(|| {
            group.process_message(&self.provider, message)
        })) {
            Ok(Ok(processed)) => processed,
            Ok(Err(_)) | Err(_) => {
                previous_storage.restore(&self.provider)?;
                return Err(MlsError::InvalidMessage);
            }
        };
        // The credential the group cryptographically bound to this sender. The
        // caller uses it to bind the plaintext to an authenticated identity
        // instead of trusting any relay-supplied envelope metadata.
        let sender_identity = BasicCredential::try_from(processed.credential().clone())
            .map(|credential| credential.identity().to_vec())
            .map_err(|_| MlsError::InvalidMessage)?;
        let result = match processed.into_content() {
            ProcessedMessageContent::ApplicationMessage(application) => {
                let mut plaintext = application.into_bytes();
                if plaintext.len() > MAX_PLAINTEXT_BYTES {
                    plaintext.zeroize();
                    Err(MlsError::InvalidInput)
                } else {
                    Ok(ProcessResult::Application(plaintext))
                }
            }
            ProcessedMessageContent::ProposalMessage(_)
            | ProcessedMessageContent::ExternalJoinProposalMessage(_) => {
                Ok(ProcessResult::Proposal)
            }
            ProcessedMessageContent::StagedCommitMessage(commit) => {
                group
                    .merge_staged_commit(&self.provider, *commit)
                    .map_err(|_| MlsError::Storage)?;
                Ok(ProcessResult::Commit)
            }
            ProcessedMessageContent::OwnPendingCommit
            | ProcessedMessageContent::OwnPrivateMessage => Err(MlsError::InvalidMessage),
        };
        match result {
            Ok(value) => Ok((value, sender_identity)),
            Err(error) => {
                previous_storage.restore(&self.provider)?;
                Err(error)
            }
        }
    }

    fn load_group(&self, conversation: [u8; 16]) -> Result<MlsGroup, MlsError> {
        MlsGroup::load(self.provider.storage(), &group_id(conversation))
            .map_err(|_| MlsError::Storage)?
            .ok_or(MlsError::MissingGroup)
    }
}

impl Drop for MlsClient {
    fn drop(&mut self) {
        self.identity.zeroize();
        let mut values = self
            .provider
            .storage()
            .values
            .write()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        storage::wipe_values(&mut values);
    }
}

fn credential(identity: &[u8], public_key: &[u8]) -> CredentialWithKey {
    CredentialWithKey {
        credential: BasicCredential::new(identity.to_vec()).into(),
        signature_key: public_key.to_vec().into(),
    }
}

fn group_id(conversation: [u8; 16]) -> GroupId {
    GroupId::from_slice(&conversation)
}

fn bounded(input: &[u8], maximum: usize) -> Result<(), MlsError> {
    if input.is_empty() || input.len() > maximum {
        Err(MlsError::InvalidInput)
    } else {
        Ok(())
    }
}

fn serialize(value: &impl TlsSerialize) -> Result<Vec<u8>, MlsError> {
    let encoded = value
        .tls_serialize_detached()
        .map_err(|_| MlsError::Internal)?;
    if encoded.len() > MAX_INPUT_BYTES {
        return Err(MlsError::InvalidInput);
    }
    Ok(encoded)
}

fn deserialize_message(input: &[u8]) -> Result<MlsMessageIn, MlsError> {
    let mut remaining = input;
    let message = MlsMessageIn::tls_deserialize(&mut remaining)
        .map_err(|_| MlsError::InvalidMessage)?;
    if !remaining.is_empty() {
        return Err(MlsError::InvalidMessage);
    }
    Ok(message)
}
