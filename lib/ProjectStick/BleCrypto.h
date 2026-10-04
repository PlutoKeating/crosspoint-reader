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

// Streaming SHA-256 (frame digests in BLE transfer protocol 4). mbedtls on the
// device and in the simulator, OpenSSL in host tests. Fixed-size state, no heap.
class Sha256 {
 public:
  Sha256();
  ~Sha256();
  Sha256(const Sha256&) = delete;
  Sha256& operator=(const Sha256&) = delete;
  void reset();
  void update(const uint8_t* data, size_t size);
  void finish(uint8_t out[32]);

 private:
  alignas(8) uint8_t state[256];
};
}  // namespace ble_crypto
