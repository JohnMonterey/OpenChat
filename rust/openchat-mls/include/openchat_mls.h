#ifndef OPENCHAT_MLS_H
#define OPENCHAT_MLS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct oc_mls_client oc_mls_client;

typedef enum oc_mls_error {
    OC_MLS_OK = 0,
    OC_MLS_INVALID_INPUT = 1,
    OC_MLS_MISSING_GROUP = 2,
    OC_MLS_INVALID_MESSAGE = 3,
    OC_MLS_CRYPTO_ERROR = 4,
    OC_MLS_STORAGE_ERROR = 5,
    OC_MLS_UNSUPPORTED = 6,
    OC_MLS_INTERNAL_ERROR = 7
} oc_mls_error;

typedef enum oc_mls_process_kind {
    OC_MLS_APPLICATION = 1,
    OC_MLS_PROPOSAL = 2,
    OC_MLS_COMMIT = 3
} oc_mls_process_kind;

typedef struct oc_mls_buffer {
    uint8_t *data;
    size_t len;
    size_t capacity;
} oc_mls_buffer;

typedef struct oc_mls_add_result {
    oc_mls_buffer commit;
    oc_mls_buffer welcome;
} oc_mls_add_result;

typedef struct oc_mls_process_result {
    uint32_t kind;
    oc_mls_buffer payload;
} oc_mls_process_result;

/*
 * The load callback is called first with data=NULL and capacity=0 to query the
 * required size, then once with a suitably sized buffer. A zero required size
 * means that no state exists yet. Callbacks return zero on success. The store
 * callback must replace the prior snapshot atomically; the data pointer is
 * valid only until the callback returns.
 */
typedef int32_t (*oc_mls_load_callback)(void *context,
                                        uint8_t *data,
                                        size_t capacity,
                                        size_t *actual_size);
typedef int32_t (*oc_mls_store_callback)(void *context,
                                         const uint8_t *data,
                                         size_t size);

typedef struct oc_mls_storage_callbacks {
    void *context;
    oc_mls_load_callback load;
    oc_mls_store_callback store;
} oc_mls_storage_callbacks;

int32_t oc_mls_client_create(const uint8_t *identity,
                             size_t identity_len,
                             const oc_mls_storage_callbacks *storage,
                             oc_mls_client **out_client);
void oc_mls_client_free(oc_mls_client *client);

int32_t oc_mls_generate_key_package(oc_mls_client *client,
                                    oc_mls_buffer *out_key_package);
int32_t oc_mls_create_group(oc_mls_client *client,
                            const uint8_t conversation_id[16]);
int32_t oc_mls_join_group(oc_mls_client *client,
                          const uint8_t conversation_id[16],
                          const uint8_t *welcome,
                          size_t welcome_len);

/* Member lists use: u16 count, then repeated u32 byte length + bytes. */
int32_t oc_mls_add_members(oc_mls_client *client,
                           const uint8_t conversation_id[16],
                           const uint8_t *framed_key_packages,
                           size_t framed_key_packages_len,
                           oc_mls_add_result *out_result);
int32_t oc_mls_remove_members(oc_mls_client *client,
                              const uint8_t conversation_id[16],
                              const uint8_t *framed_identities,
                              size_t framed_identities_len,
                              oc_mls_buffer *out_commit);

int32_t oc_mls_encrypt(oc_mls_client *client,
                       const uint8_t conversation_id[16],
                       const uint8_t *plaintext,
                       size_t plaintext_len,
                       oc_mls_buffer *out_ciphertext);
int32_t oc_mls_process(oc_mls_client *client,
                       const uint8_t conversation_id[16],
                       const uint8_t *message,
                       size_t message_len,
                       oc_mls_process_result *out_result);

void oc_mls_free_buffer(oc_mls_buffer buffer);

#ifdef __cplusplus
}
#endif

#endif
