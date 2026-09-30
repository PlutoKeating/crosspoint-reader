#pragma once
#include <cstddef>
#include <cstdint>

// Primitive crypto for the BLE setup protocol. Device builds use mbedtls, host
// tests OpenSSL (BLE_CRYPTO_OPENSSL); the desktop/web simulator has no BLE and
// gets stubs that fail.
namespace ble_crypto {
bool hmacSha256(const uint8_t* key, size_t keySize, const uint8_t* message, size_t messageSize, uint8_t out[32]);
// AES-256-CTR with a 16-byte big-endian initial counter block.
bool aes256Ctr(const uint8_t key[32], const uint8_t counter[16], const uint8_t* input, uint8_t* output, size_t size);
}  // namespace ble_crypto
