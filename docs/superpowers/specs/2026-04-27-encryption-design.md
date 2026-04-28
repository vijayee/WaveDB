# WaveDB Encryption Design

## Overview

Add optional data-at-rest encryption for WAL files and page storage. Encryption is opt-in via a separate database creation API. User-supplied keys only — no KMS integration. Supports both symmetric (AES-256-GCM) and asymmetric (any OpenSSL EVP_PKEY key type) encryption. No key rotation.

## Public API

Encryption uses a separate creation function, distinct from `database_create_with_config`:

```c
typedef enum {
    ENCRYPTION_NONE,
    ENCRYPTION_SYMMETRIC,
    ENCRYPTION_ASYMMETRIC
} encryption_type_t;

typedef struct {
    database_config_t config;
    encryption_type_t type;
    union {
        struct {
            const uint8_t* key;       // 32-byte AES-256 key
            size_t key_length;
        } symmetric;
        struct {
            const uint8_t* private_key_der;   // DER-encoded private key (NULL for write-only)
            size_t private_key_len;
            const uint8_t* public_key_der;    // DER-encoded public key
            size_t public_key_len;
        } asymmetric;
    };
} encrypted_database_config_t;

database_t* database_create_encrypted(const char* location, encrypted_database_config_t* config);
```

- `database_create_with_config` remains unchanged — unencrypted databases work exactly as before
- `database_create_encrypted` creates an encrypted database; reopening requires the same key
- The `.config` file stores `encryption_type` and verification data (never the key itself)
- Opening an encrypted database with `database_create_with_config` returns `DATABASE_ERR_ENCRYPTION_REQUIRED`

## Encryption Module

`src/Storage/encryption.h` / `encryption.c` wraps OpenSSL and provides encrypt/decrypt operations.

```c
typedef struct {
    encryption_type_t type;
    EVP_CIPHER_CTX* cipher_ctx;    // AES-256-GCM context
    EVP_PKEY* pkey;                // asymmetric: private key (NULL in write-only mode)
    EVP_PKEY* pubkey;              // asymmetric: public key
    uint8_t dek[32];               // asymmetric: data encryption key
    uint8_t wrapped_dek[512];      // asymmetric: wrapped DEK
    size_t wrapped_dek_len;
} encryption_t;

encryption_t* encryption_create_symmetric(const uint8_t* key, size_t key_length);
encryption_t* encryption_create_asymmetric(const uint8_t* private_key_der, size_t private_key_len,
                                          const uint8_t* public_key_der, size_t public_key_len);
int encryption_encrypt(encryption_t* enc, const uint8_t* plaintext, size_t pt_len, uint8_t** ciphertext, size_t* ct_len);
int encryption_decrypt(encryption_t* enc, const uint8_t* ciphertext, size_t ct_len, uint8_t** plaintext, size_t* pt_len);
void encryption_destroy(encryption_t* enc);
```

### Symmetric Mode (AES-256-GCM)

- **Encrypt**: generate 12-byte random IV, encrypt with AES-256-GCM, append 16-byte auth tag
- **Decrypt**: extract IV (first 12 bytes) and tag (last 16 bytes), decrypt and verify
- **On-disk format**: `[IV:12][ciphertext][tag:16]`
- Overhead: 28 bytes per encrypted block

### Asymmetric Mode (EVP_PKEY envelope + AES-256-GCM)

- On `encryption_create()`: generate random 32-byte DEK, wrap with `EVP_SealInit` using the public key
- Supports any OpenSSL key type: RSA (OAEP padding), EC (ECDH key agreement), Ed25519, etc.
- `EVP_SealInit` / `EVP_OpenInit` abstract over the key type automatically
- **Encrypt**: AES-256-GCM using the DEK (same as symmetric)
- **Decrypt**: if private key provided, `EVP_OpenInit` unwraps the DEK; then AES-256-GCM decrypt
- **Write-only mode** (public key only): can encrypt but not decrypt
- **On-disk format**: `[wrapped_dek_len:2 BE][wrapped_dek][IV:12][ciphertext][tag:16]`
- Overhead: 2 + wrapped_dek_len + 28 bytes per encrypted block (wrapped_dek_len varies by key type/size)

### Key Lifecycle

- Key held in memory for the lifetime of `encryption_t` (matches database lifetime)
- Key zeroed on `encryption_destroy()` using `OPENSSL_cleanse()`
- No key material persisted to disk

## WAL Encryption

### Write Path (`thread_wal_write`)

- After serializing the entry payload, before computing CRC32: call `encryption_encrypt()` on the payload buffer
- CRC32 computed over the encrypted payload (integrity of what's on disk)
- New type byte `0xE1` (`WAL_ENCRYPTED_MAGIC`) signals encrypted payload
- Header format unchanged: `[type:1][txn_id:24][crc32:4][data_len:4][encrypted_data]`
- `encrypted_data` is the opaque output of `encryption_encrypt` — symmetric: `[IV:12][ciphertext][tag:16]`, asymmetric: `[wrapped_dek_len:2][wrapped_dek][IV:12][ciphertext][tag:16]`
- The WAL layer treats `encrypted_data` as an opaque blob; the encryption module handles its own framing

### Read Path (`read_wal_file`)

- After reading header, check type byte
- `0xE1`: call `encryption_decrypt()` before format detection
- `0xB1` or CBOR: no decryption (backward compatible)
- CRC32 verified against encrypted data on disk

### Recovery (`wal_manager_recover`)

- No structural changes — type byte dispatches decryption automatically
- Encrypted entry encountered without encryption key: return error

### Migration

Opening an existing unencrypted database with `database_create_encrypted` starts encrypting new WAL entries going forward. Old `0xB1` entries are read unencrypted during recovery. No full dump/reload required.

## Page File Encryption

### Write Path (`page_file_write_node`)

- After V3 serialization, before `pwrite`: call `encryption_encrypt()` on node data
- 4-byte size prefix remains unencrypted (stores ciphertext length, needed for read)
- On disk: `[size:4 BE][encrypted_data]`

### Read Path (`page_file_read_node`)

- After reading size prefix and raw bytes: call `encryption_decrypt()` before returning to `bnode_deserialize_v3()`
- Single-block fast path: decrypt in-place after `pread`, then extract node data
- Multi-block path: assemble blocks, then decrypt concatenated data

### Superblock

- Superblock left **unencrypted** — contains only root offset, revision, last txn ID (no user data)
- Allows early detection of encryption requirement on open
- Superblock CRC covers unencrypted content

### Bnode Cache

- Stores decrypted (plaintext) nodes
- Encryption/decryption at I/O boundary only
- Cache hits never re-decrypt

### Stale Region Tracking

- No changes — tracks offsets and sizes, same whether encrypted or not

## Config Persistence and Key Verification

### Config Fields (stored in `.config` as CBOR)

- `encryption_type`: 0=NONE, 1=SYMMETRIC, 2=ASYMMETRIC
- `salt`: 16-byte random value, generated on first creation
- `encryption_check`: 28-byte ciphertext+tag from encrypting a known constant with the salt as AAD

### Key Verification on Reopen

1. On first `database_create_encrypted`: encrypt `0x00*16` using the salt as AAD in AES-256-GCM, store result as `encryption_check`
2. On subsequent opens: decrypt `encryption_check` using provided key and salt as AAD. If tag verification fails, wrong key was supplied — return error immediately, before any WAL or page file reads

### Error Codes

- `DATABASE_ERR_ENCRYPTION_REQUIRED` — unencrypted open attempted on encrypted database
- `DATABASE_ERR_ENCRYPTION_KEY_INVALID` — wrong key supplied (verification failed)
- `DATABASE_ERR_ENCRYPTION_UNSUPPORTED` — OpenSSL not available or algorithm not supported

### Backward Compatibility

- Existing `.config` without encryption fields opens normally via `database_create_with_config`
- `database_create_encrypted` on existing unencrypted database: migration path (encrypts new writes)
- `database_create_with_config` on encrypted database: returns `DATABASE_ERR_ENCRYPTION_REQUIRED`

## Build Integration

- OpenSSL added as a dependency (following existing libcbor dependency pattern)
- `encryption.c` compiled conditionally — if OpenSSL not found, `database_create_encrypted` returns `DATABASE_ERR_ENCRYPTION_UNSUPPORTED`
- No change to existing build for unencrypted databases

## Test Plan

1. **Symmetric round-trip** — create encrypted DB, put/get/verify, destroy, reopen with key, verify data persists
2. **Asymmetric round-trip** — same with RSA key pair, then EC key pair
3. **Wrong key rejection** — open with wrong symmetric key, verify `DATABASE_ERR_ENCRYPTION_KEY_INVALID`
4. **Unencrypted open on encrypted DB** — verify `DATABASE_ERR_ENCRYPTION_REQUIRED`
5. **WAL recovery** — encrypted DB, kill without clean shutdown, reopen with key, verify WAL replay succeeds
6. **Migration** — create unencrypted DB, put data, reopen with `database_create_encrypted`, verify old data readable, verify new entries encrypted on disk
7. **Page file verification** — raw-read `.wdbp`, verify node data is not plaintext
8. **Write-only mode** — create with public key only, put data, verify get fails (no private key)

## Binding API Updates

Both Node.js and Dart bindings need a separate creation API for encrypted databases, mirroring the C `database_create_encrypted` function.

### C API Additions (`src/Database/database.h`)

```c
database_t* database_create_encrypted(const char* location, encrypted_database_config_t* config);
void encrypted_database_config_destroy(encrypted_database_config_t* config);

// Setter functions for encrypted config (called after encrypted_database_config_default)
void encrypted_database_config_set_type(encrypted_database_config_t* config, encryption_type_t type);
void encrypted_database_config_set_symmetric_key(encrypted_database_config_t* config, const uint8_t* key, size_t key_length);
void encrypted_database_config_set_asymmetric_private_key(encrypted_database_config_t* config, const uint8_t* key, size_t key_length);
void encrypted_database_config_set_asymmetric_public_key(encrypted_database_config_t* config, const uint8_t* key, size_t key_length);

// Also needed: setters for the embedded database_config_t fields
// (these also fill the gap in the Dart binding which currently references missing setters)
void database_config_set_chunk_size(database_config_t* config, uint8_t chunk_size);
void database_config_set_btree_node_size(database_config_t* config, uint32_t node_size);
void database_config_set_enable_persist(database_config_t* config, uint8_t enable);
void database_config_set_lru_memory_mb(database_config_t* config, size_t mb);
void database_config_set_lru_shards(database_config_t* config, uint16_t shards);
void database_config_set_bnode_cache_memory_mb(database_config_t* config, size_t mb);
void database_config_set_bnode_cache_shards(database_config_t* config, uint16_t shards);
void database_config_set_worker_threads(database_config_t* config, uint8_t threads);
void database_config_set_wal_sync_mode(database_config_t* config, uint8_t mode);
void database_config_set_wal_debounce_ms(database_config_t* config, uint64_t ms);
void database_config_set_wal_max_file_size(database_config_t* config, size_t size);
```

The `database_config_set_*` functions are also exported because the Dart binding currently references them but they are not exported from `libwavedb.so`. This is a prerequisite fix regardless of encryption.

### Node.js Binding (`bindings/nodejs/`)

New JS API on the `WaveDB` wrapper class:

```js
const db = new WaveDB(path, {
  encryption: {
    type: 'symmetric',         // 'symmetric' or 'asymmetric'
    key: Buffer.from(...),     // 32-byte AES key for symmetric
    // OR for asymmetric:
    // publicKey: Buffer.from(...),   // DER-encoded public key
    // privateKey: Buffer.from(...), // DER-encoded private key (optional, for read-write)
  },
  // ...existing config options (chunkSize, enablePersist, etc.)
});
```

The native addon (`database.cc`) gets a new `CreateEncrypted` static method that:
1. Creates an `encrypted_database_config_t` via the C API
2. Sets encryption type and key(s) from the JS options
3. Sets the embedded `database_config_t` fields from remaining options
4. Calls `database_create_encrypted()`
5. Throws a JS error if creation fails (including `ENCRYPTION_REQUIRED` / `ENCRYPTION_KEY_INVALID`)

Opening an existing encrypted database uses the same constructor with the same `encryption` options.

### Dart Binding (`bindings/dart/`)

New `WaveDBEncryption` class and `WaveDB` constructor parameter:

```dart
class WaveDBEncryption {
  final String type;                    // 'symmetric' or 'asymmetric'
  final Uint8List? key;                 // symmetric: 32-byte AES key
  final Uint8List? publicKey;           // asymmetric: DER-encoded public key
  final Uint8List? privateKey;          // asymmetric: DER-encoded private key (optional)

  WaveDBEncryption.symmetric({required Uint8List key})
      : type = 'symmetric', this.key = key;

  WaveDBEncryption.asymmetric({required Uint8List publicKey, Uint8List? privateKey})
      : type = 'asymmetric', this.publicKey = publicKey, this.privateKey = privateKey;
}

WaveDB(String path, {String delimiter = '/', WaveDBConfig? config, WaveDBEncryption? encryption})
```

When `encryption` is provided, the Dart constructor calls `databaseCreateEncrypted()` instead of `databaseCreateWithConfig()`. The FFI layer (`wavedb_bindings.dart`) adds a `databaseCreateEncrypted()` method that:
1. Calls `encrypted_database_config_default()` to get defaults
2. Sets encryption type and key(s) via the C setter functions
3. Sets config fields via the existing `_configSet*` calls on the embedded config
4. Calls `database_create_encrypted()`
5. Throws `WaveDBException` on failure with appropriate error code

### Error Handling in Bindings

Both bindings map C error codes to native exceptions:
- `DATABASE_ERR_ENCRYPTION_REQUIRED` → `EncryptionRequiredError` / `WaveDBException(code: encryptionRequired)`
- `DATABASE_ERR_ENCRYPTION_KEY_INVALID` → `EncryptionKeyInvalidError` / `WaveDBException(code: encryptionKeyInvalid)`
- `DATABASE_ERR_ENCRYPTION_UNSUPPORTED` → `EncryptionUnsupportedError` / `WaveDBException(code: encryptionUnsupported)`

## Performance Expectations

- AES-256-GCM with AES-NI: ~1-2% overhead on I/O-bound workloads
- Asymmetric: DEK wrapping is one-time cost on create; per-entry overhead same as symmetric
- No overhead when encryption is disabled (no-op function pointers)