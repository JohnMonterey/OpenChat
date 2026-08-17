use std::{
    ffi::c_void,
    panic::{AssertUnwindSafe, catch_unwind},
    ptr,
    slice,
};

use zeroize::{Zeroize, Zeroizing};

use crate::{MlsClient, MlsError, ProcessResult};

const MAX_IDENTITY_BYTES: usize = 1024;
const MAX_INPUT_BYTES: usize = 1024 * 1024;
const MAX_STATE_BYTES: usize = 8 * 1024 * 1024;
const MAX_LIST_ITEMS: usize = 256;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct OcMlsBuffer {
    pub data: *mut u8,
    pub len: usize,
    pub capacity: usize,
}

impl Default for OcMlsBuffer {
    fn default() -> Self {
        Self {
            data: ptr::null_mut(),
            len: 0,
            capacity: 0,
        }
    }
}

#[repr(C)]
#[derive(Default)]
pub struct OcMlsAddResult {
    pub commit: OcMlsBuffer,
    pub welcome: OcMlsBuffer,
}

#[repr(C)]
#[derive(Default)]
pub struct OcMlsProcessResult {
    pub kind: u32,
    pub payload: OcMlsBuffer,
    // MLS-authenticated sender credential for an application message; empty for
    // proposals/commits. Freed by the caller like payload.
    pub sender: OcMlsBuffer,
}

pub type LoadCallback = unsafe extern "C" fn(
    context: *mut c_void,
    data: *mut u8,
    capacity: usize,
    actual_size: *mut usize,
) -> i32;
pub type StoreCallback = unsafe extern "C" fn(
    context: *mut c_void,
    data: *const u8,
    size: usize,
) -> i32;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct OcMlsStorageCallbacks {
    pub context: *mut c_void,
    pub load: Option<LoadCallback>,
    pub store: Option<StoreCallback>,
}

#[repr(C)]
pub struct OcMlsClient {
    client: MlsClient,
    storage: Option<OcMlsStorageCallbacks>,
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn oc_mls_client_create(
    identity: *const u8,
    identity_len: usize,
    storage: *const OcMlsStorageCallbacks,
    out_client: *mut *mut OcMlsClient,
) -> i32 {
    ffi_guard(|| {
        if out_client.is_null() {
            return Err(MlsError::InvalidInput);
        }
        // SAFETY: out_client was checked for null and is an out-parameter supplied by C.
        unsafe { out_client.write(ptr::null_mut()) };
        // SAFETY: pointer/length validation happens inside required_slice.
        let identity = unsafe { required_slice(identity, identity_len, MAX_IDENTITY_BYTES)? };
        let callbacks = if storage.is_null() {
            None
        } else {
            // SAFETY: a non-null callbacks pointer is borrowed only for this call.
            let callbacks = unsafe { *storage };
            match (callbacks.load, callbacks.store) {
                (Some(_), Some(_)) => Some(callbacks),
                _ => return Err(MlsError::InvalidInput),
            }
        };

        let mut client = match callbacks {
            Some(callbacks) => match load_state(callbacks)? {
                Some(state) => {
                    let state = Zeroizing::new(state);
                    let restored = MlsClient::from_snapshot(&state)?;
                    if restored.identity() != identity {
                        return Err(MlsError::InvalidInput);
                    }
                    restored
                }
                None => MlsClient::new(identity)?,
            },
            None => MlsClient::new(identity)?,
        };
        if let Some(callbacks) = callbacks {
            persist(&mut client, callbacks)?;
        }
        let boxed = Box::new(OcMlsClient {
            client,
            storage: callbacks,
        });
        // SAFETY: ownership is transferred to the caller and reclaimed by oc_mls_client_free.
        unsafe { out_client.write(Box::into_raw(boxed)) };
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn oc_mls_client_free(client: *mut OcMlsClient) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        if !client.is_null() {
            // SAFETY: client must be a live pointer returned by oc_mls_client_create.
            drop(unsafe { Box::from_raw(client) });
        }
    }));
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn oc_mls_generate_key_package(
    client: *mut OcMlsClient,
    out_key_package: *mut OcMlsBuffer,
) -> i32 {
    ffi_guard(|| {
        // SAFETY: validated by helper functions.
        let handle = unsafe { handle(client)? };
        // SAFETY: out parameter is checked by write_buffer.
        unsafe { clear_buffer_out(out_key_package)? };
        let encoded = mutate(handle, |client| client.generate_key_package())?;
        // SAFETY: out parameter remains valid for this call.
        unsafe { write_buffer(out_key_package, encoded) }
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn oc_mls_create_group(
    client: *mut OcMlsClient,
    conversation_id: *const u8,
) -> i32 {
    ffi_guard(|| {
        // SAFETY: validated by helper functions.
        let handle = unsafe { handle(client)? };
        // SAFETY: conversation IDs always contain exactly 16 bytes by ABI contract.
        let conversation = unsafe { conversation_id_from_ptr(conversation_id)? };
        mutate(handle, |client| client.create_group(conversation))
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn oc_mls_join_group(
    client: *mut OcMlsClient,
    conversation_id: *const u8,
    welcome: *const u8,
    welcome_len: usize,
) -> i32 {
    ffi_guard(|| {
        // SAFETY: validated by helper functions.
        let handle = unsafe { handle(client)? };
        // SAFETY: conversation IDs always contain exactly 16 bytes by ABI contract.
        let conversation = unsafe { conversation_id_from_ptr(conversation_id)? };
        // SAFETY: pointer/length validation happens inside required_slice.
        let welcome = unsafe { required_slice(welcome, welcome_len, MAX_INPUT_BYTES)? };
        mutate(handle, |client| client.join_group(conversation, welcome))
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn oc_mls_add_members(
    client: *mut OcMlsClient,
    conversation_id: *const u8,
    framed_key_packages: *const u8,
    framed_key_packages_len: usize,
    out_result: *mut OcMlsAddResult,
) -> i32 {
    ffi_guard(|| {
        if out_result.is_null() {
            return Err(MlsError::InvalidInput);
        }
        // SAFETY: out_result was checked for null.
        unsafe { out_result.write(OcMlsAddResult::default()) };
        // SAFETY: validated by helper functions.
        let handle = unsafe { handle(client)? };
        // SAFETY: conversation IDs always contain exactly 16 bytes by ABI contract.
        let conversation = unsafe { conversation_id_from_ptr(conversation_id)? };
        // SAFETY: pointer/length validation happens inside required_slice.
        let framed = unsafe {
            required_slice(
                framed_key_packages,
                framed_key_packages_len,
                MAX_INPUT_BYTES,
            )?
        };
        let members = parse_list(framed)?;
        let output = mutate(handle, |client| client.add_members(conversation, &members))?;
        // SAFETY: out_result remains valid for this call.
        unsafe {
            (*out_result).commit = into_buffer(output.commit);
            (*out_result).welcome = into_buffer(output.welcome);
        }
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn oc_mls_remove_members(
    client: *mut OcMlsClient,
    conversation_id: *const u8,
    framed_identities: *const u8,
    framed_identities_len: usize,
    out_commit: *mut OcMlsBuffer,
) -> i32 {
    ffi_guard(|| {
        // SAFETY: validated by helper functions.
        let handle = unsafe { handle(client)? };
        // SAFETY: conversation IDs always contain exactly 16 bytes by ABI contract.
        let conversation = unsafe { conversation_id_from_ptr(conversation_id)? };
        // SAFETY: pointer/length validation happens inside required_slice.
        let framed = unsafe {
            required_slice(
                framed_identities,
                framed_identities_len,
                MAX_INPUT_BYTES,
            )?
        };
        let identities = parse_list(framed)?;
        let output = mutate(handle, |client| {
            client.remove_members(conversation, &identities)
        })?;
        // SAFETY: out parameter is checked by write_buffer.
        unsafe { write_buffer(out_commit, output) }
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn oc_mls_inspect_welcome(
    client: *mut OcMlsClient,
    welcome: *const u8,
    welcome_len: usize,
    out_members: *mut OcMlsBuffer,
) -> i32 {
    ffi_guard(|| {
        // SAFETY: validated by helper functions.
        let handle = unsafe { handle(client)? };
        // SAFETY: out parameter is checked by clear_buffer_out.
        unsafe { clear_buffer_out(out_members)? };
        // SAFETY: pointer/length validation happens inside required_slice.
        let welcome = unsafe { required_slice(welcome, welcome_len, MAX_INPUT_BYTES)? };
        // Read-only: routed through `inspect`, which never invokes the store
        // callback, so inspection leaves the persisted snapshot untouched.
        let members = inspect(handle, |client| client.inspect_welcome(welcome))?;
        let framed = frame_list(&members)?;
        // SAFETY: out parameter remains valid for this call.
        unsafe { write_buffer(out_members, framed) }
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn oc_mls_inspect_key_package(
    client: *mut OcMlsClient,
    key_package: *const u8,
    key_package_len: usize,
    out_credential: *mut OcMlsBuffer,
) -> i32 {
    ffi_guard(|| {
        // SAFETY: validated by helper functions.
        let handle = unsafe { handle(client)? };
        // SAFETY: out parameter is checked by clear_buffer_out.
        unsafe { clear_buffer_out(out_credential)? };
        // SAFETY: pointer/length validation happens inside required_slice.
        let key_package =
            unsafe { required_slice(key_package, key_package_len, MAX_INPUT_BYTES)? };
        // Read-only: routed through `inspect`, which never invokes the store
        // callback, so inspection leaves the persisted snapshot untouched.
        let credential = inspect(handle, |client| client.inspect_key_package(key_package))?;
        // SAFETY: out parameter remains valid for this call.
        unsafe { write_buffer(out_credential, credential) }
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn oc_mls_encrypt(
    client: *mut OcMlsClient,
    conversation_id: *const u8,
    plaintext: *const u8,
    plaintext_len: usize,
    out_ciphertext: *mut OcMlsBuffer,
) -> i32 {
    ffi_guard(|| {
        // SAFETY: validated by helper functions.
        let handle = unsafe { handle(client)? };
        // SAFETY: conversation IDs always contain exactly 16 bytes by ABI contract.
        let conversation = unsafe { conversation_id_from_ptr(conversation_id)? };
        // SAFETY: pointer/length validation happens inside required_slice.
        let plaintext = unsafe { required_slice(plaintext, plaintext_len, MAX_INPUT_BYTES)? };
        let output = mutate(handle, |client| client.encrypt(conversation, plaintext))?;
        // SAFETY: out parameter is checked by write_buffer.
        unsafe { write_buffer(out_ciphertext, output) }
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn oc_mls_process(
    client: *mut OcMlsClient,
    conversation_id: *const u8,
    message: *const u8,
    message_len: usize,
    out_result: *mut OcMlsProcessResult,
) -> i32 {
    ffi_guard(|| {
        if out_result.is_null() {
            return Err(MlsError::InvalidInput);
        }
        // SAFETY: out_result was checked for null.
        unsafe { out_result.write(OcMlsProcessResult::default()) };
        // SAFETY: validated by helper functions.
        let handle = unsafe { handle(client)? };
        // SAFETY: conversation IDs always contain exactly 16 bytes by ABI contract.
        let conversation = unsafe { conversation_id_from_ptr(conversation_id)? };
        // SAFETY: pointer/length validation happens inside required_slice.
        let message = unsafe { required_slice(message, message_len, MAX_INPUT_BYTES)? };
        let (output, sender) = mutate(handle, |client| client.process(conversation, message))?;
        // SAFETY: out_result remains valid for this call.
        unsafe {
            match output {
                ProcessResult::Application(payload) => {
                    (*out_result).kind = 1;
                    (*out_result).payload = into_buffer(payload);
                    (*out_result).sender = into_buffer(sender);
                }
                ProcessResult::Proposal => (*out_result).kind = 2,
                ProcessResult::Commit => (*out_result).kind = 3,
            }
        }
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn oc_mls_free_buffer(buffer: OcMlsBuffer) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        if buffer.data.is_null() {
            return;
        }
        if buffer.len > buffer.capacity {
            return;
        }
        // SAFETY: buffers are created by into_buffer and freed exactly once by the caller.
        let mut owned = unsafe { Vec::from_raw_parts(buffer.data, buffer.len, buffer.capacity) };
        owned.zeroize();
    }));
}

fn ffi_guard(operation: impl FnOnce() -> Result<(), MlsError>) -> i32 {
    match catch_unwind(AssertUnwindSafe(operation)) {
        Ok(Ok(())) => 0,
        Ok(Err(error)) => error as i32,
        Err(_) => MlsError::Internal as i32,
    }
}

fn mutate<T>(
    handle: &mut OcMlsClient,
    operation: impl FnOnce(&mut MlsClient) -> Result<T, MlsError>,
) -> Result<T, MlsError> {
    let before = Zeroizing::new(handle.client.snapshot()?);
    let result = match catch_unwind(AssertUnwindSafe(|| operation(&mut handle.client))) {
        Ok(Ok(value)) => value,
        Ok(Err(error)) => {
            handle.client = MlsClient::from_snapshot(&before)?;
            return Err(error);
        }
        Err(_) => {
            handle.client = MlsClient::from_snapshot(&before)?;
            return Err(MlsError::Internal);
        }
    };

    if let Some(callbacks) = handle.storage {
        if persist(&mut handle.client, callbacks).is_err() {
            handle.client = MlsClient::from_snapshot(&before)?;
            return Err(MlsError::Storage);
        }
    }
    Ok(result)
}

// Runs a read-only operation. Unlike `mutate`, it never persists: no store()
// callback fires, so inspection leaves the durable snapshot untouched. A
// pre-call snapshot backs out any in-memory residue if the operation panics.
fn inspect<T>(
    handle: &mut OcMlsClient,
    operation: impl FnOnce(&MlsClient) -> Result<T, MlsError>,
) -> Result<T, MlsError> {
    let before = Zeroizing::new(handle.client.snapshot()?);
    match catch_unwind(AssertUnwindSafe(|| operation(&handle.client))) {
        Ok(Ok(value)) => Ok(value),
        Ok(Err(error)) => {
            handle.client = MlsClient::from_snapshot(&before)?;
            Err(error)
        }
        Err(_) => {
            handle.client = MlsClient::from_snapshot(&before)?;
            Err(MlsError::Internal)
        }
    }
}

fn load_state(callbacks: OcMlsStorageCallbacks) -> Result<Option<Vec<u8>>, MlsError> {
    let load = callbacks.load.ok_or(MlsError::InvalidInput)?;
    let mut required = 0usize;
    // SAFETY: the callback contract permits a null query buffer and requires actual_size.
    if unsafe { load(callbacks.context, ptr::null_mut(), 0, &mut required) } != 0 {
        return Err(MlsError::Storage);
    }
    if required == 0 {
        return Ok(None);
    }
    if required > MAX_STATE_BYTES {
        return Err(MlsError::Storage);
    }
    let mut state = vec![0u8; required];
    let mut actual = 0usize;
    // SAFETY: state has `required` writable bytes and actual is a valid out pointer.
    if unsafe { load(callbacks.context, state.as_mut_ptr(), state.len(), &mut actual) } != 0
        || actual != required
    {
        state.zeroize();
        return Err(MlsError::Storage);
    }
    Ok(Some(state))
}

fn persist(client: &mut MlsClient, callbacks: OcMlsStorageCallbacks) -> Result<(), MlsError> {
    let mut snapshot = client.snapshot()?;
    let store = callbacks.store.ok_or(MlsError::InvalidInput)?;
    // SAFETY: snapshot remains alive and immutable for the duration of the callback.
    let status = unsafe { store(callbacks.context, snapshot.as_ptr(), snapshot.len()) };
    snapshot.zeroize();
    if status == 0 {
        Ok(())
    } else {
        Err(MlsError::Storage)
    }
}

// Inverse of `parse_list`: u16 count, then per item a u32 byte length + bytes.
// Shares the framing the C++ side already parses for member lists.
fn frame_list(values: &[Vec<u8>]) -> Result<Vec<u8>, MlsError> {
    if values.len() > MAX_LIST_ITEMS {
        return Err(MlsError::InvalidInput);
    }
    let count = u16::try_from(values.len()).map_err(|_| MlsError::InvalidInput)?;
    let mut framed = Vec::new();
    framed.extend_from_slice(&count.to_be_bytes());
    for value in values {
        let length = u32::try_from(value.len()).map_err(|_| MlsError::InvalidInput)?;
        framed.extend_from_slice(&length.to_be_bytes());
        framed.extend_from_slice(value);
    }
    Ok(framed)
}

fn parse_list(input: &[u8]) -> Result<Vec<Vec<u8>>, MlsError> {
    if input.len() < 2 {
        return Err(MlsError::InvalidInput);
    }
    let count = usize::from(u16::from_be_bytes([input[0], input[1]]));
    if count == 0 || count > MAX_LIST_ITEMS {
        return Err(MlsError::InvalidInput);
    }
    let mut position = 2usize;
    let mut values = Vec::with_capacity(count);
    for _ in 0..count {
        let length_end = position.checked_add(4).ok_or(MlsError::InvalidInput)?;
        let length_bytes: [u8; 4] = input
            .get(position..length_end)
            .ok_or(MlsError::InvalidInput)?
            .try_into()
            .map_err(|_| MlsError::InvalidInput)?;
        position = length_end;
        let length = usize::try_from(u32::from_be_bytes(length_bytes))
            .map_err(|_| MlsError::InvalidInput)?;
        if length == 0 || length > MAX_INPUT_BYTES {
            return Err(MlsError::InvalidInput);
        }
        let end = position.checked_add(length).ok_or(MlsError::InvalidInput)?;
        values.push(
            input
                .get(position..end)
                .ok_or(MlsError::InvalidInput)?
                .to_vec(),
        );
        position = end;
    }
    if position != input.len() {
        return Err(MlsError::InvalidInput);
    }
    Ok(values)
}

unsafe fn handle<'a>(client: *mut OcMlsClient) -> Result<&'a mut OcMlsClient, MlsError> {
    if client.is_null() {
        return Err(MlsError::InvalidInput);
    }
    // SAFETY: the C caller guarantees exclusive access to a live handle for this call.
    Ok(unsafe { &mut *client })
}

unsafe fn conversation_id_from_ptr(pointer: *const u8) -> Result<[u8; 16], MlsError> {
    if pointer.is_null() {
        return Err(MlsError::InvalidInput);
    }
    let mut conversation = [0u8; 16];
    // SAFETY: the ABI requires pointer to reference at least 16 readable bytes.
    unsafe { ptr::copy_nonoverlapping(pointer, conversation.as_mut_ptr(), conversation.len()) };
    Ok(conversation)
}

unsafe fn required_slice<'a>(
    pointer: *const u8,
    length: usize,
    maximum: usize,
) -> Result<&'a [u8], MlsError> {
    if pointer.is_null() || length == 0 || length > maximum || length > isize::MAX as usize {
        return Err(MlsError::InvalidInput);
    }
    // SAFETY: the C caller guarantees pointer references `length` readable bytes.
    Ok(unsafe { slice::from_raw_parts(pointer, length) })
}

unsafe fn clear_buffer_out(output: *mut OcMlsBuffer) -> Result<(), MlsError> {
    if output.is_null() {
        return Err(MlsError::InvalidInput);
    }
    // SAFETY: output was checked for null and is a C out-parameter.
    unsafe { output.write(OcMlsBuffer::default()) };
    Ok(())
}

unsafe fn write_buffer(output: *mut OcMlsBuffer, bytes: Vec<u8>) -> Result<(), MlsError> {
    // SAFETY: delegated validation and initialization of the out-parameter.
    unsafe { clear_buffer_out(output)? };
    // SAFETY: output is valid after clear_buffer_out succeeds.
    unsafe { output.write(into_buffer(bytes)) };
    Ok(())
}

fn into_buffer(mut bytes: Vec<u8>) -> OcMlsBuffer {
    if bytes.is_empty() {
        return OcMlsBuffer::default();
    }
    bytes.shrink_to_fit();
    let buffer = OcMlsBuffer {
        data: bytes.as_mut_ptr(),
        len: bytes.len(),
        capacity: bytes.capacity(),
    };
    std::mem::forget(bytes);
    buffer
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn guard_contains_panics() {
        assert_eq!(
            ffi_guard(|| -> Result<(), MlsError> { panic!("contained") }),
            MlsError::Internal as i32
        );
    }

    #[test]
    fn malformed_pointer_length_pairs_fail_closed() {
        let mut client = ptr::null_mut();
        // SAFETY: this deliberately passes a null pointer to verify pre-dereference validation.
        let result = unsafe {
            oc_mls_client_create(ptr::null(), 1, ptr::null(), &mut client)
        };
        assert_eq!(result, MlsError::InvalidInput as i32);
        assert!(client.is_null());

        // SAFETY: null handles and empty buffers are explicitly supported cleanup inputs.
        unsafe {
            oc_mls_client_free(ptr::null_mut());
            oc_mls_free_buffer(OcMlsBuffer::default());
        }
    }

    #[test]
    fn framed_lists_reject_trailing_or_empty_items() {
        assert_eq!(parse_list(&[0, 1, 0, 0, 0, 0]), Err(MlsError::InvalidInput));
        assert_eq!(
            parse_list(&[0, 1, 0, 0, 0, 1, 42, 0]),
            Err(MlsError::InvalidInput)
        );
    }

    #[test]
    fn frame_list_round_trips_through_parse_list() {
        let values = vec![vec![1u8, 2, 3], vec![9u8]];
        let framed = frame_list(&values).unwrap();
        assert_eq!(parse_list(&framed).unwrap(), values);
    }
}
