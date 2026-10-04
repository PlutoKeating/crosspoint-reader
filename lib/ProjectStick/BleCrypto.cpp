#include "BleCrypto.h"

#if defined(BLE_CRYPTO_OPENSSL)
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

// SHA256_Init/Update/Final are deprecated in OpenSSL 3 but give a fixed-size
// context that fits Sha256::state (host tests only).
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

namespace ble_crypto {
bool hmacSha256(const uint8_t* key, size_t keySize, const uint8_t* message, size_t messageSize, uint8_t out[32]) {
  unsigned int length = 32;
  return HMAC(EVP_sha256(), key, static_cast<int>(keySize), message, messageSize, out, &length) != nullptr &&
         length == 32;
}
bool aes256Ctr(const uint8_t key[32], const uint8_t counter[16], const uint8_t* input, uint8_t* output, size_t size) {
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) return false;
  int written = 0, final = 0;
  const bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_ctr(), nullptr, key, counter) == 1 &&
                  EVP_EncryptUpdate(ctx, output, &written, input, static_cast<int>(size)) == 1 &&
                  EVP_EncryptFinal_ex(ctx, output + written, &final) == 1;
  EVP_CIPHER_CTX_free(ctx);
  return ok && static_cast<size_t>(written + final) == size;
}
static_assert(sizeof(SHA256_CTX) <= 256, "Sha256 state too small");
Sha256::Sha256() { reset(); }
Sha256::~Sha256() = default;
void Sha256::reset() { SHA256_Init(reinterpret_cast<SHA256_CTX*>(state)); }
void Sha256::update(const uint8_t* data, size_t size) { SHA256_Update(reinterpret_cast<SHA256_CTX*>(state), data, size); }
void Sha256::finish(uint8_t out[32]) { SHA256_Final(out, reinterpret_cast<SHA256_CTX*>(state)); }
}  // namespace ble_crypto

#elif defined(SIMULATOR)
#include <mbedtls/sha256.h>

namespace ble_crypto {
bool hmacSha256(const uint8_t*, size_t, const uint8_t*, size_t, uint8_t*) { return false; }
bool aes256Ctr(const uint8_t*, const uint8_t*, const uint8_t*, uint8_t*, size_t) { return false; }
}  // namespace ble_crypto
#define BLE_CRYPTO_MBEDTLS_SHA 1

#else
#include <mbedtls/aes.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>

#include <cstring>

namespace ble_crypto {
bool hmacSha256(const uint8_t* key, size_t keySize, const uint8_t* message, size_t messageSize, uint8_t out[32]) {
  return mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, keySize, message, messageSize, out) == 0;
}
bool aes256Ctr(const uint8_t key[32], const uint8_t counter[16], const uint8_t* input, uint8_t* output, size_t size) {
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  uint8_t block[16], stream[16]{};
  memcpy(block, counter, sizeof(block));
  size_t offset = 0;
  const bool ok = mbedtls_aes_setkey_enc(&aes, key, 256) == 0 &&
                  mbedtls_aes_crypt_ctr(&aes, size, &offset, block, stream, input, output) == 0;
  mbedtls_aes_free(&aes);
  return ok;
}
}  // namespace ble_crypto
#define BLE_CRYPTO_MBEDTLS_SHA 1
#endif

#if defined(BLE_CRYPTO_MBEDTLS_SHA)
namespace ble_crypto {
static_assert(sizeof(mbedtls_sha256_context) <= 256, "Sha256 state too small");
static mbedtls_sha256_context* context(uint8_t* state) { return reinterpret_cast<mbedtls_sha256_context*>(state); }
Sha256::Sha256() {
  mbedtls_sha256_init(context(state));
  mbedtls_sha256_starts(context(state), 0);
}
Sha256::~Sha256() { mbedtls_sha256_free(context(state)); }
void Sha256::reset() { mbedtls_sha256_starts(context(state), 0); }
void Sha256::update(const uint8_t* data, size_t size) { mbedtls_sha256_update(context(state), data, size); }
void Sha256::finish(uint8_t out[32]) { mbedtls_sha256_finish(context(state), out); }
}  // namespace ble_crypto
#endif
