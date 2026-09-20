#include "StudioBluetooth.h"
#include "StudioFrame.h"
#include <ArduinoJson.h>
#include <HalStorage.h>
#include <mutex>
#ifndef SIMULATOR
#include <NimBLEDevice.h>
#include <esp_system.h>
#include <mbedtls/md.h>
#include <mbedtls/aes.h>
#include <Arduino.h>

namespace studio_ble {
namespace {
constexpr const char* SERVICE = "9fe10000-6bc2-4ce7-8e62-77262df32ef1";
constexpr const char* CONTROL = "9fe10001-6bc2-4ce7-8e62-77262df32ef1";
constexpr const char* DATA = "9fe10002-6bc2-4ce7-8e62-77262df32ef1";
constexpr const char* STATUS = "9fe10003-6bc2-4ce7-8e62-77262df32ef1";
constexpr const char* CREDENTIALS = "/.crosspoint/studio/ble.json";
std::recursive_mutex mutex;
std::string identity, secret, nonce, control, state = "ready", error, task, hash, baseTask;
int64_t taskExpires = 0;
uint32_t epoch = 0, lastActivity = 0;
bool started = false, authenticated = false, isConnected = false;
uint16_t connectionHandle = 0xffff;
mbedtls_aes_context aes{};
uint8_t counter[16]{}, stream[16]{};
size_t counterOffset = 0;

std::string hex(const uint8_t* bytes, size_t size) {
  std::string output(size * 2, '0');
  const char* alphabet = "0123456789abcdef";
  for (size_t i = 0; i < size; ++i) { output[i * 2] = alphabet[bytes[i] >> 4]; output[i * 2 + 1] = alphabet[bytes[i] & 15]; }
  return output;
}
bool unhex(const std::string& value, uint8_t* output, size_t size) {
  if (value.size() != size * 2) return false;
  auto nibble = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
  for (size_t i = 0; i < size; ++i) { int a = nibble(value[i * 2]), b = nibble(value[i * 2 + 1]); if (a < 0 || b < 0) return false; output[i] = (a << 4) | b; }
  return true;
}
std::string mac(const std::string& message) {
  uint8_t key[32], output[32];
  if (!unhex(secret, key, 32)) return "";
  if (mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, 32,
      reinterpret_cast<const uint8_t*>(message.data()), message.size(), output) != 0) return "";
  return hex(output, 32);
}
bool equalProof(const std::string& a, const std::string& b) {
  if (a.size() != 64 || b.size() != 64) return false;
  unsigned diff = 0; for (size_t i = 0; i < 64; ++i) diff |= a[i] ^ b[i]; return diff == 0;
}
void resetSession() {
  if (authenticated && state == "receiving") StudioFrame::instance().abort();
  if (authenticated) mbedtls_aes_free(&aes);
  authenticated = false; control.clear(); task.clear(); hash.clear(); error.clear(); state = "ready";
  uint8_t random[16]; esp_fill_random(random, sizeof(random)); nonce = hex(random, sizeof(random));
  baseTask = StudioFrame::instance().snapshot().task; taskExpires = 0;
  counterOffset = 0; memset(stream, 0, sizeof(stream)); lastActivity = millis();
}
void fail(const char* reason) {
  error = reason; state = "failed";
  if (authenticated) StudioFrame::instance().abort();
}
std::string statusJson() {
  const auto snapshot = StudioFrame::instance().snapshot();
  if (authenticated && snapshot.task == task && snapshot.displayed) state = "displayed";
  JsonDocument doc; doc["device_id"] = identity; doc["nonce"] = nonce; doc["epoch"] = epoch;
  doc["base_task"] = baseTask; doc["expires"] = taskExpires;
  doc["state"] = state; doc["received"] = StudioFrame::instance().received();
  if (!error.empty()) doc["error"] = error;
  if (state == "displayed") { doc["task"] = task; doc["hash"] = hash; doc["proof"] = mac("displayed|" + task + "|" + hash + "|" + nonce + "|" + baseTask + "|" + std::to_string(taskExpires)); }
  else doc["proof"] = mac("hello|" + identity + "|" + nonce + "|" + std::to_string(epoch) + "|" + baseTask);
  std::string result; serializeJson(doc, result); return result;
}
class ServerCallbacks final : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server, NimBLEConnInfo& info) override {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (isConnected) { server->disconnect(info.getConnHandle()); return; }
    isConnected = true; connectionHandle = info.getConnHandle(); resetSession();
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo& info, int) override {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (connectionHandle != info.getConnHandle()) return;
    resetSession(); isConnected = false; connectionHandle = 0xffff; NimBLEDevice::startAdvertising();
  }
};
class Callbacks final : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic* characteristic, NimBLEConnInfo& info) override {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (info.getConnHandle() == connectionHandle) characteristic->setValue(statusJson());
  }
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& info) override {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (info.getConnHandle() != connectionHandle) return;
    lastActivity = millis();
    const auto value = characteristic->getValue();
    if (characteristic->getUUID() == NimBLEUUID(DATA)) {
      if (!authenticated || state != "receiving" || value.size() <= 4 || value.size() > 244) { fail("unauthorized_or_invalid_chunk"); return; }
      const auto* bytes = reinterpret_cast<const uint8_t*>(value.data());
      const uint32_t offset = uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 | uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24;
      if (offset != StudioFrame::instance().received()) { fail("offset_mismatch"); return; }
      uint8_t plain[240];
      if (mbedtls_aes_crypt_ctr(&aes, value.size() - 4, &counterOffset, counter, stream, bytes + 4, plain) != 0 ||
          !StudioFrame::instance().append(offset, plain, value.size() - 4)) fail("storage_or_cipher_error");
      return;
    }
    if (value.size() + control.size() > 512) { control.clear(); fail("control_too_large"); return; }
    control.append(reinterpret_cast<const char*>(value.data()), value.size());
    if (control.empty() || control.back() != '\n') return;
    JsonDocument doc; const auto parseError = deserializeJson(doc, control); control.clear();
    if (parseError) { fail("invalid_control"); return; }
    const std::string op = doc["op"] | "";
    if (op == "begin" && !authenticated) {
      const std::string incomingTask = doc["task"] | "", incomingHash = doc["hash"] | "", proof = doc["proof"] | "";
      const int64_t expires = doc["expires"] | int64_t(0);
      const std::string expected = mac("studio1|" + identity + "|" + nonce + "|" + std::to_string(epoch) + "|" + incomingTask + "|" + incomingHash + "|" + std::to_string(expires));
      if (!equalProof(proof, expected)) { fail("authorization_failed"); return; }
      if (!StudioFrame::instance().start(incomingTask, incomingHash, expires, "ble")) { fail("device_busy_or_invalid_frame"); return; }
      uint8_t encryptionKey[32];
      if (!unhex(mac("enc|" + nonce), encryptionKey, sizeof(encryptionKey)) || !unhex(nonce, counter, sizeof(counter))) { StudioFrame::instance().abort(); fail("invalid_key"); return; }
      mbedtls_aes_init(&aes); mbedtls_aes_setkey_enc(&aes, encryptionKey, 256); authenticated = true;
      task = incomingTask; hash = incomingHash; taskExpires = expires; state = "receiving";
    } else if (op == "commit" && authenticated && state == "receiving") {
      if (!StudioFrame::instance().commit()) { fail("frame_validation_failed"); return; }
      state = "refreshing";
    } else { fail("unexpected_control"); }
  }
};
ServerCallbacks serverCallbacks;
Callbacks callbacks;
}
void configure(const std::string& deviceId, const std::string& key, uint32_t keyEpoch) {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  uint8_t decoded[32]; if (deviceId.size() != 36 || !unhex(key, decoded, 32)) return;
  if (identity == deviceId && secret == key && epoch == keyEpoch) return;
  if (isConnected) NimBLEDevice::getServer()->disconnect(connectionHandle);
  identity = deviceId; secret = key; epoch = keyEpoch;
  if (started) NimBLEDevice::startAdvertising();
  Storage.mkdir("/.crosspoint/studio", true);
  HalFile file; if (Storage.openFileForWrite("STUDIO", CREDENTIALS, file)) {
    JsonDocument doc; doc["device_id"] = identity; doc["secret"] = secret; doc["epoch"] = epoch;
    serializeJson(doc, file); file.close();
  }
}
void revoke() {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  if (isConnected) NimBLEDevice::getServer()->disconnect(connectionHandle);
  resetSession(); identity.clear(); secret.clear(); epoch = 0;
  Storage.remove(CREDENTIALS);
  if (started) NimBLEDevice::getAdvertising()->stop();
}
void begin() {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  if (started) return;
  if (secret.empty()) {
    HalFile file; if (!Storage.openFileForRead("STUDIO", CREDENTIALS, file)) return;
    JsonDocument doc; const auto error = deserializeJson(doc, file); file.close(); if (error) return;
    identity = doc["device_id"] | ""; secret = doc["secret"] | ""; epoch = doc["epoch"] | 0;
  }
  uint8_t decoded[32]; if (identity.size() != 36 || !unhex(secret, decoded, 32)) return;
  NimBLEDevice::init("StockStick X3"); NimBLEDevice::setMTU(247);
  auto* server = NimBLEDevice::createServer(); server->setCallbacks(&serverCallbacks, false);
  auto* service = server->createService(SERVICE);
  service->createCharacteristic(CONTROL, NIMBLE_PROPERTY::WRITE, 512)->setCallbacks(&callbacks);
  service->createCharacteristic(DATA, NIMBLE_PROPERTY::WRITE, 244)->setCallbacks(&callbacks);
  service->createCharacteristic(STATUS, NIMBLE_PROPERTY::READ, 512)->setCallbacks(&callbacks);
  service->start(); auto* advertising = NimBLEDevice::getAdvertising(); advertising->addServiceUUID(SERVICE);
  advertising->setName("StockStick X3"); advertising->enableScanResponse(true); advertising->start(); started = true;
}
void tick() {
  static uint32_t lastStartAttempt = 0;
  if (!started && millis() - lastStartAttempt > 5000) { lastStartAttempt = millis(); begin(); }
  std::lock_guard<std::recursive_mutex> lock(mutex);
  if (isConnected && millis() - lastActivity > 30000 && state == "receiving") { fail("transfer_timeout"); NimBLEDevice::getServer()->disconnect(connectionHandle); }
}
bool connected() { std::lock_guard<std::recursive_mutex> lock(mutex); return isConnected; }
}
#else
namespace studio_ble {
void configure(const std::string&, const std::string&, uint32_t) {}
void revoke() {}
void begin() {}
void tick() {}
bool connected() { return false; }
}
#endif
