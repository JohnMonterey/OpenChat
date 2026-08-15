use std::collections::HashMap;

use openmls_rust_crypto::OpenMlsRustCrypto;
use openmls_traits::OpenMlsProvider;
use zeroize::{Zeroize, Zeroizing};

use crate::client::MlsError;

const MAGIC: &[u8; 8] = b"OCMLS\0\x01\0";
const MAX_SNAPSHOT_BYTES: usize = 8 * 1024 * 1024;
const MAX_RECORDS: usize = 4096;
const MAX_KEY_BYTES: usize = 64 * 1024;
const MAX_VALUE_BYTES: usize = 1024 * 1024;

pub(crate) struct RestoredState {
    pub identity: Vec<u8>,
    pub signer_public_key: Vec<u8>,
    pub values: HashMap<Vec<u8>, Vec<u8>>,
}

impl RestoredState {
    pub fn into_parts(mut self) -> (Vec<u8>, Vec<u8>, HashMap<Vec<u8>, Vec<u8>>) {
        (
            std::mem::take(&mut self.identity),
            std::mem::take(&mut self.signer_public_key),
            std::mem::take(&mut self.values),
        )
    }
}

impl Drop for RestoredState {
    fn drop(&mut self) {
        self.identity.zeroize();
        self.signer_public_key.zeroize();
        wipe_values(&mut self.values);
    }
}

pub(crate) struct StorageBackup(HashMap<Vec<u8>, Vec<u8>>);

impl StorageBackup {
    pub fn capture(provider: &OpenMlsRustCrypto) -> Result<Self, MlsError> {
        Ok(Self(
            provider
                .storage()
                .values
                .read()
                .map_err(|_| MlsError::Storage)?
                .clone(),
        ))
    }

    pub fn restore(mut self, provider: &OpenMlsRustCrypto) -> Result<(), MlsError> {
        let replacement = std::mem::take(&mut self.0);
        let mut values = provider
            .storage()
            .values
            .write()
            .map_err(|_| MlsError::Storage)?;
        wipe_values(&mut values);
        *values = replacement;
        Ok(())
    }
}

impl Drop for StorageBackup {
    fn drop(&mut self) {
        wipe_values(&mut self.0);
    }
}

struct SensitiveValues(HashMap<Vec<u8>, Vec<u8>>);

impl SensitiveValues {
    fn into_inner(mut self) -> HashMap<Vec<u8>, Vec<u8>> {
        std::mem::take(&mut self.0)
    }
}

impl Drop for SensitiveValues {
    fn drop(&mut self) {
        wipe_values(&mut self.0);
    }
}

pub(crate) fn snapshot(
    provider: &OpenMlsRustCrypto,
    identity: &[u8],
    signer_public_key: &[u8],
) -> Result<Vec<u8>, MlsError> {
    if identity.is_empty() || identity.len() > u16::MAX as usize {
        return Err(MlsError::InvalidInput);
    }
    if signer_public_key.len() > u16::MAX as usize {
        return Err(MlsError::InvalidInput);
    }

    let values = provider
        .storage()
        .values
        .read()
        .map_err(|_| MlsError::Storage)?;
    if values.len() > MAX_RECORDS {
        return Err(MlsError::Storage);
    }
    let mut records: Vec<_> = values.iter().collect();
    records.sort_unstable_by(|left, right| left.0.cmp(right.0));

    let mut output = Zeroizing::new(Vec::new());
    output.extend_from_slice(MAGIC);
    push_u16(&mut output, identity.len())?;
    output.extend_from_slice(identity);
    push_u16(&mut output, signer_public_key.len())?;
    output.extend_from_slice(signer_public_key);
    push_u32(&mut output, records.len())?;

    for (key, value) in records {
        if key.len() > MAX_KEY_BYTES || value.len() > MAX_VALUE_BYTES {
            return Err(MlsError::Storage);
        }
        push_u32(&mut output, key.len())?;
        push_u32(&mut output, value.len())?;
        output.extend_from_slice(key);
        output.extend_from_slice(value);
        if output.len() > MAX_SNAPSHOT_BYTES {
            return Err(MlsError::Storage);
        }
    }
    Ok(std::mem::take(&mut *output))
}

pub(crate) fn restore(input: &[u8]) -> Result<RestoredState, MlsError> {
    if input.len() > MAX_SNAPSHOT_BYTES {
        return Err(MlsError::InvalidInput);
    }
    let mut cursor = Cursor::new(input);
    if cursor.take(MAGIC.len())? != MAGIC {
        return Err(MlsError::InvalidInput);
    }

    let identity_length = usize::from(cursor.u16()?);
    if identity_length == 0 {
        return Err(MlsError::InvalidInput);
    }
    let identity = cursor.take(identity_length)?.to_vec();
    let signer_public_key_length = usize::from(cursor.u16()?);
    let signer_public_key = cursor.take(signer_public_key_length)?.to_vec();
    let count = usize::try_from(cursor.u32()?).map_err(|_| MlsError::InvalidInput)?;
    if count > MAX_RECORDS {
        return Err(MlsError::InvalidInput);
    }

    let mut values = SensitiveValues(HashMap::with_capacity(count));
    for _ in 0..count {
        let key_length = usize::try_from(cursor.u32()?).map_err(|_| MlsError::InvalidInput)?;
        let value_length = usize::try_from(cursor.u32()?).map_err(|_| MlsError::InvalidInput)?;
        if key_length > MAX_KEY_BYTES || value_length > MAX_VALUE_BYTES {
            return Err(MlsError::InvalidInput);
        }
        let key = cursor.take(key_length)?.to_vec();
        let value = cursor.take(value_length)?.to_vec();
        if values.0.insert(key, value).is_some() {
            return Err(MlsError::InvalidInput);
        }
    }
    if !cursor.is_empty() {
        return Err(MlsError::InvalidInput);
    }
    Ok(RestoredState {
        identity,
        signer_public_key,
        values: values.into_inner(),
    })
}

fn push_u16(output: &mut Vec<u8>, value: usize) -> Result<(), MlsError> {
    let value = u16::try_from(value).map_err(|_| MlsError::Storage)?;
    output.extend_from_slice(&value.to_be_bytes());
    Ok(())
}

pub(crate) fn wipe_values(values: &mut HashMap<Vec<u8>, Vec<u8>>) {
    for (mut key, mut value) in values.drain() {
        key.zeroize();
        value.zeroize();
    }
}

fn push_u32(output: &mut Vec<u8>, value: usize) -> Result<(), MlsError> {
    let value = u32::try_from(value).map_err(|_| MlsError::Storage)?;
    output.extend_from_slice(&value.to_be_bytes());
    Ok(())
}

struct Cursor<'a> {
    input: &'a [u8],
    position: usize,
}

impl<'a> Cursor<'a> {
    fn new(input: &'a [u8]) -> Self {
        Self { input, position: 0 }
    }

    fn take(&mut self, length: usize) -> Result<&'a [u8], MlsError> {
        let end = self
            .position
            .checked_add(length)
            .filter(|end| *end <= self.input.len())
            .ok_or(MlsError::InvalidInput)?;
        let value = &self.input[self.position..end];
        self.position = end;
        Ok(value)
    }

    fn u16(&mut self) -> Result<u16, MlsError> {
        let bytes: [u8; 2] = self
            .take(2)?
            .try_into()
            .map_err(|_| MlsError::InvalidInput)?;
        Ok(u16::from_be_bytes(bytes))
    }

    fn u32(&mut self) -> Result<u32, MlsError> {
        let bytes: [u8; 4] = self
            .take(4)?
            .try_into()
            .map_err(|_| MlsError::InvalidInput)?;
        Ok(u32::from_be_bytes(bytes))
    }

    fn is_empty(&self) -> bool {
        self.position == self.input.len()
    }
}
