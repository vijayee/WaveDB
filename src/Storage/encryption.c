#include "encryption.h"
#include "../Util/allocator.h"
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/err.h>
#include <string.h>

#define IV_LEN 12
#define TAG_LEN 16
#define CHECK_PT_LEN 16
#define CHECK_CT_LEN (IV_LEN + CHECK_PT_LEN + TAG_LEN)

static int aes_gcm_encrypt(const uint8_t* key, const uint8_t* iv,
                           const uint8_t* pt, size_t pt_len,
                           const uint8_t* aad, size_t aad_len,
                           uint8_t* ct, uint8_t* tag) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;

    int len = 0;
    int ct_len = 0;

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, IV_LEN, NULL) != 1 ||
        EVP_EncryptInit_ex(ctx, NULL, NULL, key, iv) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return -1;
    }

    if (aad && aad_len > 0) {
        if (EVP_EncryptUpdate(ctx, NULL, &len, aad, (int)aad_len) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            return -1;
        }
    }

    if (pt_len > 0) {
        if (EVP_EncryptUpdate(ctx, ct, &len, pt, (int)pt_len) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            return -1;
        }
        ct_len = len;
    }

    if (EVP_EncryptFinal_ex(ctx, ct + ct_len, &len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return -1;
    }
    ct_len += len;

    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, TAG_LEN, tag) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return -1;
    }

    EVP_CIPHER_CTX_free(ctx);
    return ct_len;
}

static int aes_gcm_decrypt(const uint8_t* key, const uint8_t* iv,
                            const uint8_t* ct, size_t ct_len,
                            const uint8_t* aad, size_t aad_len,
                            const uint8_t* tag,
                            uint8_t* pt) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;

    int len = 0;
    int pt_len = 0;

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, IV_LEN, NULL) != 1 ||
        EVP_DecryptInit_ex(ctx, NULL, NULL, key, iv) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return -1;
    }

    if (aad && aad_len > 0) {
        if (EVP_DecryptUpdate(ctx, NULL, &len, aad, (int)aad_len) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            return -1;
        }
    }

    if (ct_len > 0) {
        if (EVP_DecryptUpdate(ctx, pt, &len, ct, (int)ct_len) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            return -1;
        }
        pt_len = len;
    }

    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, TAG_LEN, (void*)tag) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return -1;
    }

    if (EVP_DecryptFinal_ex(ctx, pt + pt_len, &len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return -1;
    }
    pt_len += len;

    EVP_CIPHER_CTX_free(ctx);
    return pt_len;
}

static size_t wrap_dek(EVP_PKEY* pubkey, const uint8_t* dek, size_t dek_len,
                       uint8_t* out, size_t out_size) {
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(pubkey, NULL);
    if (!ctx) return 0;

    if (EVP_PKEY_encrypt_init(ctx) != 1 ||
        EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING) != 1 ||
        EVP_PKEY_CTX_set_rsa_oaep_md(ctx, EVP_sha256()) != 1) {
        EVP_PKEY_CTX_free(ctx);
        return 0;
    }

    size_t wrapped_len = out_size;
    if (EVP_PKEY_encrypt(ctx, out, &wrapped_len, dek, dek_len) != 1) {
        EVP_PKEY_CTX_free(ctx);
        return 0;
    }

    EVP_PKEY_CTX_free(ctx);
    return wrapped_len;
}

static int unwrap_dek(EVP_PKEY* pkey, const uint8_t* wrapped, size_t wrapped_len,
                       uint8_t* dek_out, size_t* dek_len) {
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(pkey, NULL);
    if (!ctx) return -1;

    if (EVP_PKEY_decrypt_init(ctx) != 1 ||
        EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING) != 1 ||
        EVP_PKEY_CTX_set_rsa_oaep_md(ctx, EVP_sha256()) != 1) {
        EVP_PKEY_CTX_free(ctx);
        return -1;
    }

    *dek_len = 32;
    if (EVP_PKEY_decrypt(ctx, dek_out, dek_len, wrapped, wrapped_len) != 1) {
        EVP_PKEY_CTX_free(ctx);
        return -1;
    }

    EVP_PKEY_CTX_free(ctx);
    return 0;
}

encryption_t* encryption_create_symmetric(const uint8_t* key, size_t key_length) {
    if (!key || key_length != 32) return NULL;

    encryption_t* enc = get_clear_memory(sizeof(encryption_t));
    enc->type = ENCRYPTION_SYMMETRIC;
    memcpy(enc->key, key, 32);

    if (RAND_bytes(enc->salt, sizeof(enc->salt)) != 1) {
        OPENSSL_cleanse(enc->key, 32);
        free(enc);
        return NULL;
    }

    if (encryption_generate_check(enc) != 0) {
        OPENSSL_cleanse(enc->key, 32);
        free(enc);
        return NULL;
    }

    return enc;
}

encryption_t* encryption_create_asymmetric(const uint8_t* private_key_der, size_t private_key_len,
                                           const uint8_t* public_key_der, size_t public_key_len) {
    if (!public_key_der || public_key_len == 0) return NULL;

    encryption_t* enc = get_clear_memory(sizeof(encryption_t));
    enc->type = ENCRYPTION_ASYMMETRIC;

    const unsigned char* pub_ptr = public_key_der;
    enc->pubkey = d2i_PUBKEY(NULL, &pub_ptr, (long)public_key_len);
    if (!enc->pubkey) {
        free(enc);
        return NULL;
    }

    if (private_key_der && private_key_len > 0) {
        const unsigned char* priv_ptr = private_key_der;
        enc->pkey = d2i_AutoPrivateKey(NULL, &priv_ptr, (long)private_key_len);
        if (!enc->pkey) {
            EVP_PKEY_free(enc->pubkey);
            free(enc);
            return NULL;
        }
    }

    if (RAND_bytes(enc->key, 32) != 1) {
        EVP_PKEY_free(enc->pubkey);
        EVP_PKEY_free(enc->pkey);
        free(enc);
        return NULL;
    }

    enc->wrapped_dek_len = wrap_dek(enc->pubkey, enc->key, 32,
                                     enc->wrapped_dek, sizeof(enc->wrapped_dek));
    if (enc->wrapped_dek_len == 0) {
        OPENSSL_cleanse(enc->key, 32);
        EVP_PKEY_free(enc->pubkey);
        EVP_PKEY_free(enc->pkey);
        free(enc);
        return NULL;
    }

    if (RAND_bytes(enc->salt, sizeof(enc->salt)) != 1) {
        OPENSSL_cleanse(enc->key, 32);
        EVP_PKEY_free(enc->pubkey);
        EVP_PKEY_free(enc->pkey);
        free(enc);
        return NULL;
    }

    if (encryption_generate_check(enc) != 0) {
        OPENSSL_cleanse(enc->key, 32);
        EVP_PKEY_free(enc->pubkey);
        EVP_PKEY_free(enc->pkey);
        free(enc);
        return NULL;
    }

    return enc;
}

encryption_t* encryption_create_from_config(encryption_type_t type,
                                            const uint8_t* key, size_t key_length,
                                            const uint8_t* private_key_der, size_t private_key_len,
                                            const uint8_t* public_key_der, size_t public_key_len,
                                            const uint8_t* salt, const uint8_t* check) {
    if (!key || key_length != 32) return NULL;
    if (!salt || !check) return NULL;

    encryption_t* enc = get_clear_memory(sizeof(encryption_t));
    enc->type = type;
    memcpy(enc->key, key, 32);
    memcpy(enc->salt, salt, sizeof(enc->salt));
    memcpy(enc->check, check, sizeof(enc->check));
    enc->has_check = 1;

    if (type == ENCRYPTION_ASYMMETRIC) {
        if (!public_key_der || public_key_len == 0) {
            free(enc);
            return NULL;
        }

        const unsigned char* pub_ptr = public_key_der;
        enc->pubkey = d2i_PUBKEY(NULL, &pub_ptr, (long)public_key_len);
        if (!enc->pubkey) {
            OPENSSL_cleanse(enc->key, 32);
            free(enc);
            return NULL;
        }

        if (private_key_der && private_key_len > 0) {
            const unsigned char* priv_ptr = private_key_der;
            enc->pkey = d2i_AutoPrivateKey(NULL, &priv_ptr, (long)private_key_len);
            if (!enc->pkey) {
                OPENSSL_cleanse(enc->key, 32);
                EVP_PKEY_free(enc->pubkey);
                free(enc);
                return NULL;
            }
        }

        enc->wrapped_dek_len = wrap_dek(enc->pubkey, enc->key, 32,
                                         enc->wrapped_dek, sizeof(enc->wrapped_dek));
        if (enc->wrapped_dek_len == 0) {
            OPENSSL_cleanse(enc->key, 32);
            EVP_PKEY_free(enc->pubkey);
            EVP_PKEY_free(enc->pkey);
            free(enc);
            return NULL;
        }
    }

    if (encryption_verify_check(enc) != 0) {
        OPENSSL_cleanse(enc->key, 32);
        EVP_PKEY_free(enc->pubkey);
        EVP_PKEY_free(enc->pkey);
        free(enc);
        return NULL;
    }

    return enc;
}

int encryption_encrypt(encryption_t* enc, const uint8_t* plaintext, size_t pt_len,
                       uint8_t** ciphertext, size_t* ct_len) {
    if (!enc || !ciphertext || !ct_len) return -1;

    uint8_t iv[IV_LEN];
    if (RAND_bytes(iv, IV_LEN) != 1) return -1;

    uint8_t tag[TAG_LEN];
    size_t out_len;

    if (enc->type == ENCRYPTION_SYMMETRIC) {
        out_len = IV_LEN + pt_len + TAG_LEN;
        uint8_t* out = get_memory(out_len);
        if (!out) return -1;

        memcpy(out, iv, IV_LEN);

        int encrypted_len = aes_gcm_encrypt(enc->key, iv,
                                            plaintext, pt_len,
                                            NULL, 0,
                                            out + IV_LEN, tag);
        if (encrypted_len < 0) {
            OPENSSL_cleanse(out, out_len);
            free(out);
            return -1;
        }

        memcpy(out + IV_LEN + (size_t)encrypted_len, tag, TAG_LEN);
        out_len = IV_LEN + (size_t)encrypted_len + TAG_LEN;
        *ciphertext = out;
        *ct_len = out_len;
    } else {
        out_len = 2 + enc->wrapped_dek_len + IV_LEN + pt_len + TAG_LEN;
        uint8_t* out = get_memory(out_len);
        if (!out) return -1;

        size_t pos = 0;
        out[pos++] = (uint8_t)(enc->wrapped_dek_len >> 8);
        out[pos++] = (uint8_t)(enc->wrapped_dek_len & 0xFF);
        memcpy(out + pos, enc->wrapped_dek, enc->wrapped_dek_len);
        pos += enc->wrapped_dek_len;
        memcpy(out + pos, iv, IV_LEN);
        pos += IV_LEN;

        int encrypted_len = aes_gcm_encrypt(enc->key, iv,
                                            plaintext, pt_len,
                                            NULL, 0,
                                            out + pos, tag);
        if (encrypted_len < 0) {
            OPENSSL_cleanse(out, out_len);
            free(out);
            return -1;
        }

        pos += (size_t)encrypted_len;
        memcpy(out + pos, tag, TAG_LEN);
        pos += TAG_LEN;
        out_len = pos;
        *ciphertext = out;
        *ct_len = out_len;
    }

    return 0;
}

int encryption_decrypt(encryption_t* enc, const uint8_t* ciphertext, size_t ct_len,
                       uint8_t** plaintext, size_t* pt_len) {
    if (!enc || !ciphertext || !plaintext || !pt_len) return -1;
    if (ct_len == 0) return -1;

    const uint8_t* iv;
    const uint8_t* ct;
    const uint8_t* tag;
    size_t ct_size;

    if (enc->type == ENCRYPTION_SYMMETRIC) {
        if (ct_len < IV_LEN + TAG_LEN) return -1;

        iv = ciphertext;
        ct = ciphertext + IV_LEN;
        ct_size = ct_len - IV_LEN - TAG_LEN;
        tag = ciphertext + ct_len - TAG_LEN;
    } else {
        if (ct_len < 2) return -1;

        size_t wrapped_len = ((size_t)ciphertext[0] << 8) | ciphertext[1];
        size_t header = 2 + wrapped_len;

        if (ct_len < header + IV_LEN + TAG_LEN) return -1;

        const uint8_t* wrapped = ciphertext + 2;
        iv = ciphertext + header;
        ct = iv + IV_LEN;
        ct_size = ct_len - header - IV_LEN - TAG_LEN;
        tag = ciphertext + ct_len - TAG_LEN;

        uint8_t dek[32];
        size_t dek_len = sizeof(dek);

        if (enc->pkey) {
            if (unwrap_dek(enc->pkey, wrapped, wrapped_len, dek, &dek_len) != 0)
                return -1;
        } else {
            return -1;
        }

        uint8_t* pt = get_memory(ct_size > 0 ? ct_size : 1);
        if (!pt) return -1;

        int decrypted_len = aes_gcm_decrypt(dek, iv, ct, ct_size, NULL, 0, tag, pt);
        OPENSSL_cleanse(dek, sizeof(dek));

        if (decrypted_len < 0) {
            OPENSSL_cleanse(pt, ct_size > 0 ? ct_size : 1);
            free(pt);
            return -1;
        }

        *plaintext = pt;
        *pt_len = (size_t)decrypted_len;
        return 0;
    }

    uint8_t* pt = get_memory(ct_size > 0 ? ct_size : 1);
    if (!pt) return -1;

    int decrypted_len = aes_gcm_decrypt(enc->key, iv, ct, ct_size, NULL, 0, tag, pt);
    if (decrypted_len < 0) {
        OPENSSL_cleanse(pt, ct_size > 0 ? ct_size : 1);
        free(pt);
        return -1;
    }

    *plaintext = pt;
    *pt_len = (size_t)decrypted_len;
    return 0;
}

int encryption_generate_check(encryption_t* enc) {
    if (!enc) return -1;

    uint8_t known[CHECK_PT_LEN];
    memset(known, 0, sizeof(known));

    uint8_t iv[IV_LEN];
    if (RAND_bytes(iv, IV_LEN) != 1) return -1;

    uint8_t tag[TAG_LEN];
    uint8_t ct[CHECK_PT_LEN];

    int ct_len = aes_gcm_encrypt(enc->key, iv, known, CHECK_PT_LEN,
                                  enc->salt, sizeof(enc->salt), ct, tag);
    if (ct_len < 0) return -1;

    memcpy(enc->check, iv, IV_LEN);
    memcpy(enc->check + IV_LEN, ct, (size_t)ct_len);
    memcpy(enc->check + IV_LEN + (size_t)ct_len, tag, TAG_LEN);
    enc->has_check = 1;

    return 0;
}

int encryption_verify_check(encryption_t* enc) {
    if (!enc || !enc->has_check) return -1;

    const uint8_t* iv = enc->check;
    const uint8_t* ct = enc->check + IV_LEN;
    const uint8_t* tag = enc->check + IV_LEN + CHECK_PT_LEN;
    size_t ct_size = sizeof(enc->check) - IV_LEN - TAG_LEN;

    uint8_t pt[CHECK_PT_LEN];
    int pt_len = aes_gcm_decrypt(enc->key, iv, ct, ct_size,
                                  enc->salt, sizeof(enc->salt),
                                  tag, pt);
    if (pt_len < 0) return -1;

    uint8_t expected[CHECK_PT_LEN];
    memset(expected, 0, sizeof(expected));
    if ((size_t)pt_len != sizeof(expected) || memcmp(pt, expected, sizeof(expected)) != 0) {
        OPENSSL_cleanse(pt, sizeof(pt));
        return -1;
    }

    OPENSSL_cleanse(pt, sizeof(pt));
    return 0;
}

const uint8_t* encryption_get_salt(const encryption_t* enc) {
    if (!enc) return NULL;
    return enc->salt;
}

const uint8_t* encryption_get_check(const encryption_t* enc) {
    if (!enc) return NULL;
    return enc->check;
}

void encryption_destroy(encryption_t* enc) {
    if (!enc) return;

    OPENSSL_cleanse(enc->key, sizeof(enc->key));
    OPENSSL_cleanse(enc->wrapped_dek, sizeof(enc->wrapped_dek));
    OPENSSL_cleanse(enc->salt, sizeof(enc->salt));
    OPENSSL_cleanse(enc->check, sizeof(enc->check));

    if (enc->pkey) EVP_PKEY_free(enc->pkey);
    if (enc->pubkey) EVP_PKEY_free(enc->pubkey);

    free(enc);
}