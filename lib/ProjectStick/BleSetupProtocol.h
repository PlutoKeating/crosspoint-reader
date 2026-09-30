#pragma once
// BLE setup protocol 3 (Project.StockStick docs/product/BLE-SETUP.md): the
// pure parts shared by the firmware and host tests. MAC message strings, the
// AES-256-CTR "seal", the setup QR payload and the size-capped STATUS network
// list. Crypto primitives come from BleCrypto.cpp (mbedtls on the device,
// OpenSSL in host tests).
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ble_setup {

constexpr size_t STATUS_LIMIT = 512;
constexpr size_t SETUP_KEY_BYTES = 16;
constexpr size_t MAX_NETWORKS = 5;

// Lowercase hex helpers; unhex accepts lowercase only (the wire format).
std::string hex(const uint8_t* bytes, size_t size);
bool unhex(const std::string& value, std::vector<uint8_t>& out);

// HMAC-SHA256(key, message) as lowercase hex; empty on a malformed key.
std::string mac(const std::string& keyHex, const std::string& message);
// seal(key, label, n, data): AES-256-CTR, key = raw mac(key, label|n), initial
// counter = raw n (big-endian 128-bit increment). The same call unseals.
// Output is hex; returns false on malformed inputs.
bool seal(const std::string& keyHex, const char* label, const std::string& nonce, const std::string& plaintext,
          std::string& cipherHex);
bool unseal(const std::string& keyHex, const char* label, const std::string& nonce, const std::string& cipherHex,
            std::string& plaintext);

// MAC message strings (§5, §6).
std::string setupProofMessage(const std::string& nonce, const std::string& deviceId);
std::string scanMessage(const std::string& nonce);
std::string wifiMessage(const std::string& nonce, const std::string& ssid, const std::string& pw);
std::string bindMessage(const std::string& nonce, const std::string& owner, uint32_t epoch, const std::string& ct);

// stockstick://setup?d=<device_id>&k=<K>; 94 characters (QR version 5-L, byte mode).
std::string setupQrPayload(const std::string& deviceId, const std::string& keyHex);
// "StockStick-" + the first four hex digits of the device id, uppercase.
std::string advertisedName(const std::string& deviceId);

struct Network {
  std::string ssid;
  int rssi = -127;
  bool locked = true;
};
// Drops hidden (empty) SSIDs, keeps the strongest entry per SSID, sorts by
// signal (strongest first) and keeps at most MAX_NETWORKS.
std::vector<Network> normalizeNetworks(std::vector<Network> raw);
// Appends ,"networks":[...] to a serialized JSON object (`base` must end with
// '}'), dropping entries from the end until the result fits `limit` bytes.
// Returns `base` unchanged when even an empty list would not fit.
std::string withNetworks(const std::string& base, const std::vector<Network>& networks, size_t limit = STATUS_LIMIT);

// Splits the bind plaintext "<device_token>|<secret>"; validates both parts.
bool splitBindPlaintext(const std::string& plaintext, std::string& token, std::string& secret);

}  // namespace ble_setup
