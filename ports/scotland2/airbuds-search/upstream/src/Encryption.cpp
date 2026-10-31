#include "Encryption.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>

namespace AirbudsSearch {

// Derive key from password and salt using PBKDF2
Bytes deriveKey(const std::string_view password, const Bytes &salt, int keyLen) {
    Bytes key(keyLen);
    if (!PKCS5_PBKDF2_HMAC(password.data(), password.size(),
                           salt.data(), salt.size(),
                           100000, EVP_sha256(),
                           keyLen, key.data())) {
        throw std::runtime_error("Key derivation failed");
    }
    return key;
}

// Encrypt (AES-256-GCM)
Bytes encrypt(const std::string_view password, const Bytes &plaintext) {
    Bytes salt(16), iv(12), tag(16);
    RAND_bytes(salt.data(), salt.size());
    RAND_bytes(iv.data(), iv.size());

    Bytes key = deriveKey(password, salt);
    Bytes ciphertext(plaintext.size());

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw std::runtime_error("EVP_CIPHER_CTX_new failed");

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1)
        throw std::runtime_error("EncryptInit failed");
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, iv.size(), nullptr);
    EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), iv.data());

    int len = 0;
    if (EVP_EncryptUpdate(ctx, ciphertext.data(), &len, plaintext.data(), plaintext.size()) != 1)
        throw std::runtime_error("EncryptUpdate failed");

    int ciphertextLen = len;
    if (EVP_EncryptFinal_ex(ctx, ciphertext.data() + len, &len) != 1)
        throw std::runtime_error("EncryptFinal failed");

    ciphertextLen += len;
    ciphertext.resize(ciphertextLen);

    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, tag.size(), tag.data()) != 1)
        throw std::runtime_error("GetTag failed");

    EVP_CIPHER_CTX_free(ctx);

    // Store: [salt][iv][tag][ciphertext]
    Bytes out;
    out.reserve(salt.size() + iv.size() + tag.size() + ciphertext.size());
    out.insert(out.end(), salt.begin(), salt.end());
    out.insert(out.end(), iv.begin(), iv.end());
    out.insert(out.end(), tag.begin(), tag.end());
    out.insert(out.end(), ciphertext.begin(), ciphertext.end());
    return out;
}

Bytes encrypt(const std::string_view password, const std::string_view plaintext) {
    return encrypt(password, std::vector<uint8_t>(plaintext.begin(), plaintext.end()));
}

// Decrypt (AES-256-GCM)
Bytes decrypt(const std::string_view password, const Bytes &input) {
    if (input.size() < 16 + 12 + 16)
        throw std::runtime_error("Input too short");

    const uint8_t *p = input.data();
    Bytes salt(p, p + 16);
    Bytes iv(p + 16, p + 28);
    Bytes tag(p + 28, p + 44);
    Bytes ciphertext(p + 44, input.data() + input.size());

    Bytes key = deriveKey(password, salt);
    Bytes plaintext(ciphertext.size());

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw std::runtime_error("EVP_CIPHER_CTX_new failed");

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1)
        throw std::runtime_error("DecryptInit failed");
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, iv.size(), nullptr);
    EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), iv.data());

    int len = 0;
    if (EVP_DecryptUpdate(ctx, plaintext.data(), &len, ciphertext.data(), ciphertext.size()) != 1)
        throw std::runtime_error("DecryptUpdate failed");

    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, tag.size(), (void *)tag.data());

    int ret = EVP_DecryptFinal_ex(ctx, plaintext.data() + len, &len);
    EVP_CIPHER_CTX_free(ctx);

    if (ret <= 0) {
        throw std::runtime_error("Authentication failed (bad password or modified data)");
    }

    plaintext.resize(len + ciphertext.size());
    return plaintext;
}

}