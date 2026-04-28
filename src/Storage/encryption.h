//
// Created by victor on 4/27/26.
//

#ifndef WAVEDB_ENCRYPTION_H
#define WAVEDB_ENCRYPTION_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Encryption type selector.
 * NONE: no encryption (pass-through)
 * SYMMETRIC: AES-256-GCM with user-supplied 32-byte key
 * ASYMMETRIC: AES-256-GCM with random DEK, wrapped by user-supplied EVP_PKEY
 */
typedef enum {
    ENCRYPTION_NONE = 0,
    ENCRYPTION_SYMMETRIC = 1,
    ENCRYPTION_ASYMMETRIC = 2
} encryption_type_t;

/**
 * Encryption context. Holds the key material and OpenSSL state
 * for the lifetime of a database connection.
 */
typedef struct {
    encryption_type_t type;
    void* pkey;            /* EVP_PKEY* — private key (NULL in write-only mode) */
    void* pubkey;          /* EVP_PKEY* — public key */
    uint8_t key[32];         /* Symmetric: AES-256 key. Asymmetric: DEK */
    uint8_t wrapped_dek[512]; /* Wrapped DEK (asymmetric mode) */
    size_t wrapped_dek_len;
    uint8_t salt[16];      /* Verification salt */
    uint8_t check[44];     /* Verification ciphertext+tag: IV(12) + CHECK_PT(16) + TAG(16) */
    uint8_t has_check;     /* Whether check was loaded from config */
} encryption_t;

/**
 * Create encryption context for symmetric mode.
 * Key must be exactly 32 bytes (AES-256).
 */
encryption_t* encryption_create_symmetric(const uint8_t* key, size_t key_length);

/**
 * Create encryption context for asymmetric mode.
 * private_key_der may be NULL for write-only mode.
 * public_key_der is required.
 * Both must be DER-encoded.
 */
encryption_t* encryption_create_asymmetric(const uint8_t* private_key_der, size_t private_key_len,
                                           const uint8_t* public_key_der, size_t public_key_len);

/**
 * Create encryption context from loaded config (re-open).
 * Verifies the key against the stored salt/check.
 * Returns NULL if key verification fails.
 */
encryption_t* encryption_create_from_config(encryption_type_t type,
                                            const uint8_t* key, size_t key_length,
                                            const uint8_t* private_key_der, size_t private_key_len,
                                            const uint8_t* public_key_der, size_t public_key_len,
                                            const uint8_t* salt, const uint8_t* check);

/**
 * Encrypt plaintext. Caller must free *ciphertext.
 * Returns 0 on success, -1 on error.
 * For symmetric: output = [IV:12][ciphertext][tag:16]
 * For asymmetric: output = [wrapped_dek_len:2 BE][wrapped_dek][IV:12][ciphertext][tag:16]
 */
int encryption_encrypt(encryption_t* enc, const uint8_t* plaintext, size_t pt_len,
                       uint8_t** ciphertext, size_t* ct_len);

/**
 * Decrypt ciphertext. Caller must free *plaintext.
 * Returns 0 on success, -1 on error (includes auth tag failure).
 */
int encryption_decrypt(encryption_t* enc, const uint8_t* ciphertext, size_t ct_len,
                       uint8_t** plaintext, size_t* pt_len);

/**
 * Generate verification check. Encrypts a known constant using
 * the salt as AAD. Stores result in enc->check.
 * Returns 0 on success, -1 on error.
 */
int encryption_generate_check(encryption_t* enc);

/**
 * Verify the key against stored check. Returns 0 on success, -1 on failure.
 */
int encryption_verify_check(encryption_t* enc);

/**
 * Get the salt for config storage.
 */
const uint8_t* encryption_get_salt(const encryption_t* enc);

/**
 * Get the verification check for config storage.
 */
const uint8_t* encryption_get_check(const encryption_t* enc);

/**
 * Destroy encryption context. Zeroes key material before freeing.
 */
void encryption_destroy(encryption_t* enc);

#ifdef __cplusplus
}
#endif

#endif // WAVEDB_ENCRYPTION_H