#include "BleCrypto.h"

#if defined(BLE_CRYPTO_OPENSSL)
#include <openssl/evp.h>
#include <openssl/hmac.h>

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
}  // namespace ble_crypto

#elif defined(SIMULATOR)

namespace ble_crypto {
bool hmacSha256(const uint8_t*, size_t, const uint8_t*, size_t, uint8_t*) { return false; }
bool aes256Ctr(const uint8_t*, const uint8_t*, const uint8_t*, uint8_t*, size_t) { return false; }
}  // namespace ble_crypto

#else
#include <mbedtls/aes.h>
#include <mbedtls/md.h>

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
#endif
