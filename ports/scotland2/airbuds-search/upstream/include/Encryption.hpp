#pragma once

#include <vector>
#include <string_view>

namespace AirbudsSearch {

using Bytes = std::vector<uint8_t>;

// Derive key from password and salt using PBKDF2
Bytes deriveKey(std::string_view password, const Bytes &salt, int keyLen = 32);

// Encrypt (AES-256-GCM)
Bytes encrypt(std::string_view password, const Bytes &plaintext);
Bytes encrypt(std::string_view password, std::string_view plaintext);

// Decrypt (AES-256-GCM)
Bytes decrypt(std::string_view password, const Bytes &input);

}
