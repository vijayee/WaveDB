//
// test_encrypted_database.cpp — Integration tests for encrypted database
//

#include <gtest/gtest.h>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <string>
#include <sys/stat.h>

#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/rand.h>

extern "C" {
#include "Database/database.h"
#include "Database/database_config.h"
#include "Time/wheel.h"
#include "Workers/pool.h"
#include "Storage/encryption.h"
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string make_temp_dir() {
    char tmpl[] = "/tmp/wavedb_enc_test_XXXXXX";
    char* dir = mkdtemp(tmpl);
    return std::string(dir);
}

static void remove_temp_dir(const std::string& dir) {
    std::string cmd = "rm -rf " + dir;
    system(cmd.c_str());
}

// Generate a random 32-byte AES key
static std::vector<uint8_t> make_symmetric_key() {
    std::vector<uint8_t> key(32, 0);
    RAND_bytes(key.data(), 32);
    return key;
}

// Generate a 2048-bit RSA key pair, returning DER-encoded private and public keys
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

// Safe destroy for encrypted_database_config_t.
// The config setters store pointers to caller-owned memory, but
// encrypted_database_config_destroy() tries to free() those pointers.
// We null them out before destroying to avoid double-free.
static void safe_destroy_encrypted_config(encrypted_database_config_t* config) {
    if (config == nullptr) return;
    // Null out key material pointers so destroy won't free caller-owned memory
    config->symmetric.key = nullptr;
    config->symmetric.key_length = 0;
    config->asymmetric.private_key_der = nullptr;
    config->asymmetric.private_key_len = 0;
    config->asymmetric.public_key_der = nullptr;
    config->asymmetric.public_key_len = 0;
    config->config.encryption.key = nullptr;
    config->config.encryption.key_length = 0;
    config->config.encryption.private_key_der = nullptr;
    config->config.encryption.private_key_len = 0;
    config->config.encryption.public_key_der = nullptr;
    config->config.encryption.public_key_len = 0;
    encrypted_database_config_destroy(config);
}

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class EncryptedDatabaseTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir = make_temp_dir();
        pool = work_pool_create(platform_core_count());
        work_pool_launch(pool);
        wheel = hierarchical_timing_wheel_create(8, pool);
        hierarchical_timing_wheel_run(wheel);
    }

    void TearDown() override {
        // Stop wheel and pool before destroying database
        if (wheel) {
            hierarchical_timing_wheel_stop(wheel);
        }
        if (pool) {
            work_pool_shutdown(pool);
            work_pool_join_all(pool);
        }

        if (db) {
            database_destroy(db);
            db = nullptr;
        }

        if (wheel) {
            hierarchical_timing_wheel_destroy(wheel);
            wheel = nullptr;
        }
        if (pool) {
            work_pool_destroy(pool);
            pool = nullptr;
        }

        remove_temp_dir(test_dir);
    }

    database_t* db = nullptr;
    work_pool_t* pool = nullptr;
    hierarchical_timing_wheel_t* wheel = nullptr;
    std::string test_dir;
};

// ---------------------------------------------------------------------------
// 1. SymmetricCreatePutGet
// ---------------------------------------------------------------------------

TEST_F(EncryptedDatabaseTest, SymmetricCreatePutGet) {
    auto key = make_symmetric_key();

    encrypted_database_config_t* config = encrypted_database_config_default();
    ASSERT_NE(config, nullptr);
    encrypted_database_config_set_type(config, ENCRYPTION_SYMMETRIC);
    encrypted_database_config_set_symmetric_key(config, key.data(), key.size());
    database_config_set_enable_persist(&config->config, 1);
    config->config.external_pool = pool;
    config->config.external_wheel = wheel;

    int error = 0;
    db = database_create_encrypted(test_dir.c_str(), config, &error);
    ASSERT_NE(db, nullptr) << "Error code: " << error;
    EXPECT_EQ(error, 0);

    // Put a value using raw API
    const char* test_key = "users\037alice\037name";
    const char* test_value = "Alice Smith";
    int rc = database_put_sync_raw(db, test_key, strlen(test_key), '\037',
                                   (const uint8_t*)test_value, strlen(test_value));
    EXPECT_EQ(rc, 0);

    // Get it back
    uint8_t* value_out = nullptr;
    size_t value_len_out = 0;
    rc = database_get_sync_raw(db, test_key, strlen(test_key), '\037',
                                &value_out, &value_len_out);
    EXPECT_EQ(rc, 0);
    ASSERT_NE(value_out, nullptr);
    EXPECT_EQ(value_len_out, strlen(test_value));
    EXPECT_EQ(memcmp(value_out, test_value, strlen(test_value)), 0);
    database_raw_value_free(value_out);

    safe_destroy_encrypted_config(config);
}

// ---------------------------------------------------------------------------
// 2. SymmetricReopenWithKey
// ---------------------------------------------------------------------------

TEST_F(EncryptedDatabaseTest, SymmetricReopenWithKey) {
    auto key = make_symmetric_key();

    // First instance: create and insert
    {
        encrypted_database_config_t* config = encrypted_database_config_default();
        ASSERT_NE(config, nullptr);
        encrypted_database_config_set_type(config, ENCRYPTION_SYMMETRIC);
        encrypted_database_config_set_symmetric_key(config, key.data(), key.size());
        database_config_set_enable_persist(&config->config, 1);
        config->config.external_pool = pool;
        config->config.external_wheel = wheel;

        int error = 0;
        db = database_create_encrypted(test_dir.c_str(), config, &error);
        ASSERT_NE(db, nullptr) << "Error code: " << error;
        EXPECT_EQ(error, 0);

        // Put a value
        const char* test_key = "persistent\037key1";
        const char* test_value = "persistent_value1";
        int rc = database_put_sync_raw(db, test_key, strlen(test_key), '\037',
                                        (const uint8_t*)test_value, strlen(test_value));
        EXPECT_EQ(rc, 0);

        // Force snapshot to persist data
        database_snapshot(db);

        safe_destroy_encrypted_config(config);
    }

    // Destroy database
    database_destroy(db);
    db = nullptr;

    // Second instance: reopen with same key
    {
        encrypted_database_config_t* config = encrypted_database_config_default();
        ASSERT_NE(config, nullptr);
        encrypted_database_config_set_type(config, ENCRYPTION_SYMMETRIC);
        encrypted_database_config_set_symmetric_key(config, key.data(), key.size());
        database_config_set_enable_persist(&config->config, 1);
        config->config.external_pool = pool;
        config->config.external_wheel = wheel;

        int error = 0;
        db = database_create_encrypted(test_dir.c_str(), config, &error);
        ASSERT_NE(db, nullptr) << "Error code: " << error;
        EXPECT_EQ(error, 0);

        // Get the value back
        const char* test_key = "persistent\037key1";
        uint8_t* value_out = nullptr;
        size_t value_len_out = 0;
        int rc = database_get_sync_raw(db, test_key, strlen(test_key), '\037',
                                        &value_out, &value_len_out);
        EXPECT_EQ(rc, 0);
        ASSERT_NE(value_out, nullptr);
        EXPECT_EQ(value_len_out, strlen("persistent_value1"));
        EXPECT_EQ(memcmp(value_out, "persistent_value1", strlen("persistent_value1")), 0);
        database_raw_value_free(value_out);

        safe_destroy_encrypted_config(config);
    }
}

// ---------------------------------------------------------------------------
// 3. WrongKeyRejected
// ---------------------------------------------------------------------------

TEST_F(EncryptedDatabaseTest, WrongKeyRejected) {
    auto key1 = make_symmetric_key();
    auto key2 = make_symmetric_key();
    // Ensure key2 is different
    key2[0] ^= 0xFF;

    // Create encrypted DB with key1
    {
        encrypted_database_config_t* config = encrypted_database_config_default();
        ASSERT_NE(config, nullptr);
        encrypted_database_config_set_type(config, ENCRYPTION_SYMMETRIC);
        encrypted_database_config_set_symmetric_key(config, key1.data(), key1.size());
        database_config_set_enable_persist(&config->config, 1);
        config->config.external_pool = pool;
        config->config.external_wheel = wheel;

        int error = 0;
        db = database_create_encrypted(test_dir.c_str(), config, &error);
        ASSERT_NE(db, nullptr) << "Error code: " << error;
        EXPECT_EQ(error, 0);

        // Put a value
        const char* test_key = "secret\037data";
        const char* test_value = "classified";
        int rc = database_put_sync_raw(db, test_key, strlen(test_key), '\037',
                                        (const uint8_t*)test_value, strlen(test_value));
        EXPECT_EQ(rc, 0);

        database_snapshot(db);

        safe_destroy_encrypted_config(config);
    }

    database_destroy(db);
    db = nullptr;

    // Try to reopen with key2 — should fail
    {
        encrypted_database_config_t* config = encrypted_database_config_default();
        ASSERT_NE(config, nullptr);
        encrypted_database_config_set_type(config, ENCRYPTION_SYMMETRIC);
        encrypted_database_config_set_symmetric_key(config, key2.data(), key2.size());
        database_config_set_enable_persist(&config->config, 1);
        config->config.external_pool = pool;
        config->config.external_wheel = wheel;

        int error = 0;
        database_t* result = database_create_encrypted(test_dir.c_str(), config, &error);
        EXPECT_EQ(result, nullptr);
        EXPECT_EQ(error, DATABASE_ERR_ENCRYPTION_KEY_INVALID);

        safe_destroy_encrypted_config(config);
    }
}

// ---------------------------------------------------------------------------
// 4. UnencryptedOpenOnEncryptedDB
// ---------------------------------------------------------------------------

TEST_F(EncryptedDatabaseTest, UnencryptedOpenOnEncryptedDB) {
    auto key = make_symmetric_key();

    // Create encrypted DB
    {
        encrypted_database_config_t* config = encrypted_database_config_default();
        ASSERT_NE(config, nullptr);
        encrypted_database_config_set_type(config, ENCRYPTION_SYMMETRIC);
        encrypted_database_config_set_symmetric_key(config, key.data(), key.size());
        database_config_set_enable_persist(&config->config, 1);
        config->config.external_pool = pool;
        config->config.external_wheel = wheel;

        int error = 0;
        db = database_create_encrypted(test_dir.c_str(), config, &error);
        ASSERT_NE(db, nullptr) << "Error code: " << error;
        EXPECT_EQ(error, 0);

        const char* test_key = "enc\037data";
        const char* test_value = "secret stuff";
        int rc = database_put_sync_raw(db, test_key, strlen(test_key), '\037',
                                        (const uint8_t*)test_value, strlen(test_value));
        EXPECT_EQ(rc, 0);

        database_snapshot(db);

        safe_destroy_encrypted_config(config);
    }

    database_destroy(db);
    db = nullptr;

    // Try to open with database_create_with_config (no encryption) — should fail
    {
        database_config_t* config = database_config_default();
        ASSERT_NE(config, nullptr);
        database_config_set_enable_persist(config, 1);
        config->external_pool = pool;
        config->external_wheel = wheel;

        int error = 0;
        database_t* result = database_create_with_config(test_dir.c_str(), config, &error);
        EXPECT_EQ(result, nullptr);
        EXPECT_EQ(error, DATABASE_ERR_ENCRYPTION_REQUIRED);

        database_config_destroy(config);
    }
}

// ---------------------------------------------------------------------------
// 5. AsymmetricRoundTrip
// ---------------------------------------------------------------------------

TEST_F(EncryptedDatabaseTest, AsymmetricRoundTrip) {
    std::vector<uint8_t> priv_der, pub_der;
    ASSERT_TRUE(generate_rsa_keypair(priv_der, pub_der));

    encrypted_database_config_t* config = encrypted_database_config_default();
    ASSERT_NE(config, nullptr);
    encrypted_database_config_set_type(config, ENCRYPTION_ASYMMETRIC);
    encrypted_database_config_set_asymmetric_private_key(config, priv_der.data(), priv_der.size());
    encrypted_database_config_set_asymmetric_public_key(config, pub_der.data(), pub_der.size());
    database_config_set_enable_persist(&config->config, 1);
    config->config.external_pool = pool;
    config->config.external_wheel = wheel;

    int error = 0;
    db = database_create_encrypted(test_dir.c_str(), config, &error);
    ASSERT_NE(db, nullptr) << "Error code: " << error;
    EXPECT_EQ(error, 0);

    // Put a value
    const char* test_key = "asym\037key1";
    const char* test_value = "RSA encrypted data";
    int rc = database_put_sync_raw(db, test_key, strlen(test_key), '\037',
                                    (const uint8_t*)test_value, strlen(test_value));
    EXPECT_EQ(rc, 0);

    // Get it back
    uint8_t* value_out = nullptr;
    size_t value_len_out = 0;
    rc = database_get_sync_raw(db, test_key, strlen(test_key), '\037',
                                &value_out, &value_len_out);
    EXPECT_EQ(rc, 0);
    ASSERT_NE(value_out, nullptr);
    EXPECT_EQ(value_len_out, strlen(test_value));
    EXPECT_EQ(memcmp(value_out, test_value, strlen(test_value)), 0);
    database_raw_value_free(value_out);

    safe_destroy_encrypted_config(config);
}