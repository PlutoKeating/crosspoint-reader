#include "BleSetupProtocol.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "BleCrypto.h"

namespace ble_setup {

std::string hex(const uint8_t* bytes, size_t size) {
  static const char* alphabet = "0123456789abcdef";
  std::string output(size * 2, '0');
  for (size_t i = 0; i < size; ++i) {
    output[i * 2] = alphabet[bytes[i] >> 4];
    output[i * 2 + 1] = alphabet[bytes[i] & 15];
  }
  return output;
}

bool unhex(const std::string& value, std::vector<uint8_t>& out) {
  if (value.size() % 2) return false;
  auto nibble = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
  out.resize(value.size() / 2);
  for (size_t i = 0; i < out.size(); ++i) {
    const int a = nibble(value[i * 2]), b = nibble(value[i * 2 + 1]);
    if (a < 0 || b < 0) return false;
    out[i] = static_cast<uint8_t>(a << 4 | b);
  }
  return true;
}

std::string mac(const std::string& keyHex, const std::string& message) {
  std::vector<uint8_t> key;
  uint8_t output[32];
  if (!unhex(keyHex, key) || key.empty() ||
      !ble_crypto::hmacSha256(key.data(), key.size(), reinterpret_cast<const uint8_t*>(message.data()), message.size(),
                              output))
    return "";
  return hex(output, sizeof(output));
}

namespace {
bool cipher(const std::string& keyHex, const char* label, const std::string& nonce, const uint8_t* input,
            size_t size, std::vector<uint8_t>& output) {
  std::vector<uint8_t> key, counter;
  if (!unhex(mac(keyHex, std::string(label) + "|" + nonce), key) || key.size() != 32 || !unhex(nonce, counter) ||
      counter.size() != 16)
    return false;
  output.resize(size);
  return size == 0 || ble_crypto::aes256Ctr(key.data(), counter.data(), input, output.data(), size);
}
}  // namespace

bool seal(const std::string& keyHex, const char* label, const std::string& nonce, const std::string& plaintext,
          std::string& cipherHex) {
  std::vector<uint8_t> output;
  if (!cipher(keyHex, label, nonce, reinterpret_cast<const uint8_t*>(plaintext.data()), plaintext.size(), output))
    return false;
  cipherHex = hex(output.data(), output.size());
  return true;
}

bool unseal(const std::string& keyHex, const char* label, const std::string& nonce, const std::string& cipherHex,
            std::string& plaintext) {
  std::vector<uint8_t> input, output;
  if (!unhex(cipherHex, input) || !cipher(keyHex, label, nonce, input.data(), input.size(), output)) return false;
  plaintext.assign(output.begin(), output.end());
  return true;
}

std::string setupProofMessage(const std::string& nonce, const std::string& deviceId) {
  return "setup3|" + nonce + "|" + deviceId;
}
std::string scanMessage(const std::string& nonce) { return "scan3|" + nonce; }
std::string wifiMessage(const std::string& nonce, const std::string& ssid, const std::string& pw) {
  return "wifi3|" + nonce + "|" + ssid + "|" + pw;
}
std::string bindMessage(const std::string& nonce, const std::string& owner, uint32_t epoch, const std::string& ct) {
  return "bind3|" + nonce + "|" + owner + "|" + std::to_string(epoch) + "|" + ct;
}

std::string otaMessage(const std::string& nonce, const std::string& version, const std::string& sha256, size_t bytes,
                       const std::string& url) {
  return "ota3|" + nonce + "|" + version + "|" + sha256 + "|" + std::to_string(bytes) + "|" + url;
}

std::string syncMessage(const std::string& nonce, int64_t time, int trading, size_t ackCount, bool otaAck) {
  return "sync3|" + nonce + "|" + std::to_string(time) + "|" + std::to_string(trading) + "|" +
         std::to_string(ackCount) + "|" + (otaAck ? "1" : "0");
}
std::string unbindMessage(const std::string& nonce) { return "unbind3|" + nonce; }

std::string setupQrPayload(const std::string& deviceId, const std::string& keyHex) {
  return "stockstick://setup?d=" + deviceId + "&k=" + keyHex;
}

std::string advertisedName(const std::string& deviceId) {
  std::string name = "StockStick-";
  for (size_t i = 0; i < 4 && i < deviceId.size(); ++i)
    name += static_cast<char>(std::toupper(static_cast<unsigned char>(deviceId[i])));
  return name;
}

std::vector<Network> normalizeNetworks(std::vector<Network> raw) {
  std::vector<Network> networks;
  for (auto& network : raw) {
    if (network.ssid.empty()) continue;
    auto it = std::find_if(networks.begin(), networks.end(),
                           [&](const Network& other) { return other.ssid == network.ssid; });
    if (it == networks.end())
      networks.push_back(std::move(network));
    else if (network.rssi > it->rssi)
      *it = std::move(network);
  }
  std::stable_sort(networks.begin(), networks.end(), [](const Network& a, const Network& b) { return a.rssi > b.rssi; });
  if (networks.size() > MAX_NETWORKS) networks.resize(MAX_NETWORKS);
  return networks;
}

namespace {
std::string jsonString(const std::string& value) {
  std::string out = "\"";
  for (const unsigned char c : value) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += static_cast<char>(c);
    } else if (c < 0x20) {
      char escaped[8];
      snprintf(escaped, sizeof(escaped), "\\u%04x", c);
      out += escaped;
    } else {
      out += static_cast<char>(c);
    }
  }
  return out + "\"";
}
}  // namespace

std::string withNetworks(const std::string& base, const std::vector<Network>& networks, size_t limit) {
  if (base.empty() || base.back() != '}') return base;
  const std::string head = base.substr(0, base.size() - 1) + (base.size() > 2 ? "," : "") + "\"networks\":[";
  std::vector<std::string> items;
  for (const auto& network : networks)
    items.push_back("{\"s\":" + jsonString(network.ssid) + ",\"r\":" + std::to_string(network.rssi) +
                    ",\"l\":" + (network.locked ? "1" : "0") + "}");
  for (size_t count = items.size() + 1; count-- > 0;) {
    std::string out = head;
    for (size_t i = 0; i < count; ++i) out += (i ? "," : "") + items[i];
    out += "]}";
    if (out.size() <= limit) return out;
  }
  return base;
}

bool validAuthority(const std::string& secretHex, int64_t epoch) {
  std::vector<uint8_t> decoded;
  return secretHex.size() == 64 && unhex(secretHex, decoded) && epoch > 0 && epoch <= int64_t(UINT32_MAX);
}

bool splitBindPlaintext(const std::string& plaintext, std::string& token, std::string& secret) {
  const size_t bar = plaintext.rfind('|');
  if (bar == std::string::npos) return false;
  token = plaintext.substr(0, bar);
  secret = plaintext.substr(bar + 1);
  std::vector<uint8_t> decoded;
  const bool tokenOk = token.size() >= 32 && token.size() <= 96 &&
                       std::all_of(token.begin(), token.end(), [](unsigned char c) {
                         return std::isalnum(c) || c == '-' || c == '_';
                       });
  return tokenOk && secret.size() == 64 && unhex(secret, decoded);
}

}  // namespace ble_setup
