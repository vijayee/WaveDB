//
// test_encryption.cpp — Unit tests for the encryption module
//

#include <gtest/gtest.h>
#include <cstring>
#include <vector>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/rand.h>

extern "C" {
#include "Storage/encryption.h"
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Generate a random 32-byte key. Returns a vector of exactly 32 bytes.
static std::vector<uint8_t> make_key() {
    std::vector<uint8_t> key(32, 0);
    RAND_bytes(key.data(), 32);
    return key;
}

// Generate a 2048-bit RSA key pair and return DER-encoded private and public
// keys through the output parameters. Returns true on success.
static bool generate_rsa_keypair(std::vector<uint8_t>& priv_der,
                                  std::vector<uint8_t>& pub_der) {
    EVP_PKEY* pkey = EVP_PKEY_new();
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
    if (!ctx) { EVP_PKEY_free(pkey); return false; }

    if (EVP_PKEY_keygen_init(ctx) != 1 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048) != 1 ||
        EVP_PKEY_keygen(ctx, &pkey) != 1) {
        EVP_PKEY_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        return false;
    }
    EVP_PKEY_CTX_free(ctx);

    // Serialize private key to DER
    unsigned char* priv_buf = nullptr;
    int priv_len = i2d_PrivateKey(pkey, &priv_buf);
    if (priv_len <= 0) { EVP_PKEY_free(pkey); return false; }
    priv_der.assign(priv_buf, priv_buf + priv_len);
    OPENSSL_free(priv_buf);

    // Serialize public key to DER
    unsigned char* pub_buf = nullptr;
    int pub_len = i2d_PUBKEY(pkey, &pub_buf);
    if (pub_len <= 0) { EVP_PKEY_free(pkey); return false; }
    pub_der.assign(pub_buf, pub_buf + pub_len);
    OPENSSL_free(pub_buf);

    EVP_PKEY_free(pkey);
    return true;
}

// ---------------------------------------------------------------------------
// 1. SymmetricRoundTrip
// ---------------------------------------------------------------------------

TEST(EncryptionTest, SymmetricRoundTrip) {
    auto key = make_key();
    encryption_t* enc = encryption_create_symmetric(key.data(), key.size());
    ASSERT_NE(enc, nullptr);

    const char* plaintext = "Hello, WaveDB!";
    size_t pt_len = strlen(plaintext);

    uint8_t* ciphertext = nullptr;
    size_t ct_len = 0;

    int rc = encryption_encrypt(enc, (const uint8_t*)plaintext, pt_len,
                                &ciphertext, &ct_len);
    EXPECT_EQ(rc, 0);
    ASSERT_NE(ciphertext, nullptr);
    EXPECT_GT(ct_len, pt_len);  // ciphertext should be larger (IV + tag overhead)

    uint8_t* decrypted = nullptr;
    size_t dec_len = 0;

    rc = encryption_decrypt(enc, ciphertext, ct_len, &decrypted, &dec_len);
    EXPECT_EQ(rc, 0);
    ASSERT_NE(decrypted, nullptr);
    EXPECT_EQ(dec_len, pt_len);
    EXPECT_EQ(memcmp(decrypted, plaintext, pt_len), 0);

    free(ciphertext);
    free(decrypted);
    encryption_destroy(enc);
}

// ---------------------------------------------------------------------------
// 2. SymmetricMultipleEncrypts
// ---------------------------------------------------------------------------

TEST(EncryptionTest, SymmetricMultipleEncrypts) {
    auto key = make_key();
    encryption_t* enc = encryption_create_symmetric(key.data(), key.size());
    ASSERT_NE(enc, nullptr);

    const char* msgs[] = {
        "first message",
        "second message with different length",
        "third"
    };

    uint8_t* cts[3] = {};
    size_t ct_lens[3] = {};

    // Encrypt all three
    for (int i = 0; i < 3; i++) {
        int rc = encryption_encrypt(enc,
            (const uint8_t*)msgs[i], strlen(msgs[i]),
            &cts[i], &ct_lens[i]);
        EXPECT_EQ(rc, 0);
        ASSERT_NE(cts[i], nullptr);
    }

    // Verify ciphertexts are different (due to random IVs)
    // At least the first 12 bytes (IV) should differ between encryptions
    bool any_iv_different = false;
    for (int i = 1; i < 3; i++) {
        if (memcmp(cts[0], cts[i], 12) != 0) {
            any_iv_different = true;
            break;
        }
    }
    EXPECT_TRUE(any_iv_different);

    // Decrypt all three and verify match
    for (int i = 0; i < 3; i++) {
        uint8_t* pt = nullptr;
        size_t pt_len = 0;
        int rc = encryption_decrypt(enc, cts[i], ct_lens[i], &pt, &pt_len);
        EXPECT_EQ(rc, 0);
        ASSERT_NE(pt, nullptr);
        EXPECT_EQ(pt_len, strlen(msgs[i]));
        EXPECT_EQ(memcmp(pt, msgs[i], strlen(msgs[i])), 0);
        free(pt);
        free(cts[i]);
    }

    encryption_destroy(enc);
}

// ---------------------------------------------------------------------------
// 3. SymmetricWrongKey
// ---------------------------------------------------------------------------

TEST(EncryptionTest, SymmetricWrongKey) {
    auto key1 = make_key();
    auto key2 = make_key();
    // Ensure key2 is different from key1
    key2[0] ^= 0xFF;

    encryption_t* enc1 = encryption_create_symmetric(key1.data(), key1.size());
    ASSERT_NE(enc1, nullptr);

    const char* plaintext = "secret data";
    size_t pt_len = strlen(plaintext);

    uint8_t* ciphertext = nullptr;
    size_t ct_len = 0;

    int rc = encryption_encrypt(enc1, (const uint8_t*)plaintext, pt_len,
                                &ciphertext, &ct_len);
    EXPECT_EQ(rc, 0);

    // Create a second context with a different key
    encryption_t* enc2 = encryption_create_symmetric(key2.data(), key2.size());
    ASSERT_NE(enc2, nullptr);

    // Try to decrypt with the wrong key — should fail (return -1)
    uint8_t* decrypted = nullptr;
    size_t dec_len = 0;
    rc = encryption_decrypt(enc2, ciphertext, ct_len, &decrypted, &dec_len);
    EXPECT_EQ(rc, -1);

    free(ciphertext);
    encryption_destroy(enc1);
    encryption_destroy(enc2);
}

// ---------------------------------------------------------------------------
// 4. AsymmetricRoundTripRSA
// ---------------------------------------------------------------------------

TEST(EncryptionTest, AsymmetricRoundTripRSA) {
    std::vector<uint8_t> priv_der, pub_der;
    ASSERT_TRUE(generate_rsa_keypair(priv_der, pub_der));

    encryption_t* enc = encryption_create_asymmetric(
        priv_der.data(), priv_der.size(),
        pub_der.data(), pub_der.size());
    ASSERT_NE(enc, nullptr);

    const char* plaintext = "RSA encrypted data for WaveDB";
    size_t pt_len = strlen(plaintext);

    uint8_t* ciphertext = nullptr;
    size_t ct_len = 0;

    int rc = encryption_encrypt(enc, (const uint8_t*)plaintext, pt_len,
                                &ciphertext, &ct_len);
    EXPECT_EQ(rc, 0);
    ASSERT_NE(ciphertext, nullptr);
    EXPECT_GT(ct_len, pt_len);

    uint8_t* decrypted = nullptr;
    size_t dec_len = 0;

    rc = encryption_decrypt(enc, ciphertext, ct_len, &decrypted, &dec_len);
    EXPECT_EQ(rc, 0);
    ASSERT_NE(decrypted, nullptr);
    EXPECT_EQ(dec_len, pt_len);
    EXPECT_EQ(memcmp(decrypted, plaintext, pt_len), 0);

    free(ciphertext);
    free(decrypted);
    encryption_destroy(enc);
}

// ---------------------------------------------------------------------------
// 5. AsymmetricWriteOnly
// ---------------------------------------------------------------------------

TEST(EncryptionTest, AsymmetricWriteOnly) {
    std::vector<uint8_t> priv_der, pub_der;
    ASSERT_TRUE(generate_rsa_keypair(priv_der, pub_der));

    // Create with public key only (NULL private key) — write-only mode
    encryption_t* enc = encryption_create_asymmetric(
        NULL, 0,
        pub_der.data(), pub_der.size());
    ASSERT_NE(enc, nullptr);

    const char* plaintext = "write-only data";
    size_t pt_len = strlen(plaintext);

    // Encryption should succeed
    uint8_t* ciphertext = nullptr;
    size_t ct_len = 0;
    int rc = encryption_encrypt(enc, (const uint8_t*)plaintext, pt_len,
                                &ciphertext, &ct_len);
    EXPECT_EQ(rc, 0);
    ASSERT_NE(ciphertext, nullptr);

    // Decryption should fail (no private key)
    uint8_t* decrypted = nullptr;
    size_t dec_len = 0;
    rc = encryption_decrypt(enc, ciphertext, ct_len, &decrypted, &dec_len);
    EXPECT_EQ(rc, -1);

    free(ciphertext);
    encryption_destroy(enc);
}

// ---------------------------------------------------------------------------
// 6. KeyVerification
// ---------------------------------------------------------------------------

TEST(EncryptionTest, KeyVerification) {
    auto key = make_key();
    encryption_t* enc = encryption_create_symmetric(key.data(), key.size());
    ASSERT_NE(enc, nullptr);

    // generate_check is called during create_symmetric, but call it again
    // explicitly to verify it returns success.
    int rc = encryption_generate_check(enc);
    EXPECT_EQ(rc, 0);

    // verify_check should succeed with the same context
    rc = encryption_verify_check(enc);
    EXPECT_EQ(rc, 0);

    encryption_destroy(enc);
}

// ---------------------------------------------------------------------------
// 7. KeyVerificationWrongKey
// ---------------------------------------------------------------------------

TEST(EncryptionTest, KeyVerificationWrongKey) {
    auto key1 = make_key();
    auto key2 = make_key();
    // Ensure keys are different
    key2[0] ^= 0xFF;

    encryption_t* enc1 = encryption_create_symmetric(key1.data(), key1.size());
    ASSERT_NE(enc1, nullptr);

    // Get the salt and check from enc1
    const uint8_t* salt = encryption_get_salt(enc1);
    const uint8_t* check = encryption_get_check(enc1);
    ASSERT_NE(salt, nullptr);
    ASSERT_NE(check, nullptr);

    // Try to create a context with key2 but enc1's salt and check — should fail
    // because verify_check will try to decrypt the check data with key2.
    encryption_t* enc2 = encryption_create_from_config(
        ENCRYPTION_SYMMETRIC,
        key2.data(), key2.size(),
        NULL, 0, NULL, 0,
        salt, check);
    // enc2 should be NULL because verification fails
    EXPECT_EQ(enc2, nullptr);

    encryption_destroy(enc1);
}

// ---------------------------------------------------------------------------
// 8. NullKeyRejected
// ---------------------------------------------------------------------------

TEST(EncryptionTest, NullKeyRejected) {
    // NULL key should return NULL
    encryption_t* enc = encryption_create_symmetric(NULL, 32);
    EXPECT_EQ(enc, nullptr);

    // Wrong length key should return NULL
    uint8_t short_key[16] = {0};
    enc = encryption_create_symmetric(short_key, 16);
    EXPECT_EQ(enc, nullptr);

    // Zero-length key should return NULL
    enc = encryption_create_symmetric((const uint8_t*)"x", 0);
    EXPECT_EQ(enc, nullptr);
}

// ---------------------------------------------------------------------------
// 9. DestroyNull
// ---------------------------------------------------------------------------

TEST(EncryptionTest, DestroyNull) {
    // Should not crash
    encryption_destroy(NULL);
    SUCCEED();
}

// ---------------------------------------------------------------------------
// 10. EncryptionDestroyCleansUp
// ---------------------------------------------------------------------------

TEST(EncryptionTest, EncryptionDestroyCleansUp) {
    auto key = make_key();
    encryption_t* enc = encryption_create_symmetric(key.data(), key.size());
    ASSERT_NE(enc, nullptr);

    // Do some work to allocate internal state
    const char* plaintext = "cleanup test";
    uint8_t* ct = nullptr;
    size_t ct_len = 0;
    int rc = encryption_encrypt(enc, (const uint8_t*)plaintext, strlen(plaintext),
                                &ct, &ct_len);
    EXPECT_EQ(rc, 0);
    free(ct);

    // Destroy should not leak — verified externally with valgrind
    encryption_destroy(enc);
    SUCCEED();
}

// ---------------------------------------------------------------------------
// SymmetricFromConfig round-trip
// ---------------------------------------------------------------------------

TEST(EncryptionTest, SymmetricFromConfigRoundTrip) {
    auto key = make_key();
    encryption_t* enc1 = encryption_create_symmetric(key.data(), key.size());
    ASSERT_NE(enc1, nullptr);

    // Save the salt and check for later re-opening
    const uint8_t* salt = encryption_get_salt(enc1);
    const uint8_t* check = encryption_get_check(enc1);
    ASSERT_NE(salt, nullptr);
    ASSERT_NE(check, nullptr);

    // Encrypt some data with enc1
    const char* plaintext = "persistent secret";
    uint8_t* ct = nullptr;
    size_t ct_len = 0;
    int rc = encryption_encrypt(enc1, (const uint8_t*)plaintext, strlen(plaintext),
                                &ct, &ct_len);
    EXPECT_EQ(rc, 0);

    // Re-create context from saved config
    encryption_t* enc2 = encryption_create_from_config(
        ENCRYPTION_SYMMETRIC,
        key.data(), key.size(),
        NULL, 0, NULL, 0,
        salt, check);
    ASSERT_NE(enc2, nullptr);

    // Decrypt with the re-opened context
    uint8_t* pt = nullptr;
    size_t pt_len = 0;
    rc = encryption_decrypt(enc2, ct, ct_len, &pt, &pt_len);
    EXPECT_EQ(rc, 0);
    ASSERT_NE(pt, nullptr);
    EXPECT_EQ(pt_len, strlen(plaintext));
    EXPECT_EQ(memcmp(pt, plaintext, strlen(plaintext)), 0);

    free(ct);
    free(pt);
    encryption_destroy(enc1);
    encryption_destroy(enc2);
}

// ---------------------------------------------------------------------------
// AsymmetricFromConfigRoundTrip
// ---------------------------------------------------------------------------

TEST(EncryptionTest, AsymmetricFromConfigRoundTrip) {
    std::vector<uint8_t> priv_der, pub_der;
    ASSERT_TRUE(generate_rsa_keypair(priv_der, pub_der));

    encryption_t* enc1 = encryption_create_asymmetric(
        priv_der.data(), priv_der.size(),
        pub_der.data(), pub_der.size());
    ASSERT_NE(enc1, nullptr);

    // Get the symmetric key from enc1 for re-opening
    // For asymmetric, we need the raw DEK, salt, and check to re-open
    // The DEK is in enc->key, but we can't access it directly.
    // Instead, verify that encryption/decryption works, then re-open
    // with the same key material.

    const char* plaintext = "asymmetric persistence";
    uint8_t* ct = nullptr;
    size_t ct_len = 0;
    int rc = encryption_encrypt(enc1, (const uint8_t*)plaintext, strlen(plaintext),
                                &ct, &ct_len);
    EXPECT_EQ(rc, 0);

    // For asymmetric mode, we need the wrapped_dek from enc1's ciphertext
    // to recover. But for from_config with asymmetric, we need:
    // the DEK (key), salt, check, and the key pair.
    // Since we can't get the DEK directly, we copy it from the struct.
    uint8_t saved_key[32];
    memcpy(saved_key, enc1->key, 32);
    const uint8_t* salt = encryption_get_salt(enc1);
    const uint8_t* check = encryption_get_check(enc1);
    ASSERT_NE(salt, nullptr);
    ASSERT_NE(check, nullptr);

    encryption_t* enc2 = encryption_create_from_config(
        ENCRYPTION_ASYMMETRIC,
        saved_key, 32,
        priv_der.data(), priv_der.size(),
        pub_der.data(), pub_der.size(),
        salt, check);
    ASSERT_NE(enc2, nullptr);

    // Decrypt data encrypted by enc1
    uint8_t* pt = nullptr;
    size_t pt_len = 0;
    rc = encryption_decrypt(enc2, ct, ct_len, &pt, &pt_len);
    EXPECT_EQ(rc, 0);
    ASSERT_NE(pt, nullptr);
    EXPECT_EQ(pt_len, strlen(plaintext));
    EXPECT_EQ(memcmp(pt, plaintext, strlen(plaintext)), 0);

    free(ct);
    free(pt);
    encryption_destroy(enc1);
    encryption_destroy(enc2);
}

// ---------------------------------------------------------------------------
// Empty plaintext round-trip
// ---------------------------------------------------------------------------

TEST(EncryptionTest, EmptyPlaintextSymmetric) {
    auto key = make_key();
    encryption_t* enc = encryption_create_symmetric(key.data(), key.size());
    ASSERT_NE(enc, nullptr);

    uint8_t* ct = nullptr;
    size_t ct_len = 0;
    int rc = encryption_encrypt(enc, (const uint8_t*)"", 0, &ct, &ct_len);
    EXPECT_EQ(rc, 0);

    uint8_t* pt = nullptr;
    size_t pt_len = 0;
    rc = encryption_decrypt(enc, ct, ct_len, &pt, &pt_len);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(pt_len, 0u);

    free(ct);
    free(pt);
    encryption_destroy(enc);
}

// ---------------------------------------------------------------------------
// Large plaintext round-trip
// ---------------------------------------------------------------------------

TEST(EncryptionTest, LargePlaintextSymmetric) {
    auto key = make_key();
    encryption_t* enc = encryption_create_symmetric(key.data(), key.size());
    ASSERT_NE(enc, nullptr);

    std::vector<uint8_t> large_data(65536);
    RAND_bytes(large_data.data(), large_data.size());

    uint8_t* ct = nullptr;
    size_t ct_len = 0;
    int rc = encryption_encrypt(enc, large_data.data(), large_data.size(),
                                &ct, &ct_len);
    EXPECT_EQ(rc, 0);

    uint8_t* pt = nullptr;
    size_t pt_len = 0;
    rc = encryption_decrypt(enc, ct, ct_len, &pt, &pt_len);
    EXPECT_EQ(rc, 0);
    ASSERT_NE(pt, nullptr);
    EXPECT_EQ(pt_len, large_data.size());
    EXPECT_EQ(memcmp(pt, large_data.data(), large_data.size()), 0);

    free(ct);
    free(pt);
    encryption_destroy(enc);
}