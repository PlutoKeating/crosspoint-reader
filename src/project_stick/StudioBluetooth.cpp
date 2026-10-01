#include "StudioBluetooth.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <atomic>
#include <mutex>

#include "FirmwareUpdateState.h"
#include "StudioFrame.h"
#ifndef SIMULATOR
#include <optional>

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <esp_bt.h>
#include <esp_system.h>
#include <mbedtls/aes.h>
#include <mbedtls/md.h>
#include <sys/time.h>

namespace studio_ble {
namespace {
constexpr const char* SERVICE = "9fe10000-6bc2-4ce7-8e62-77262df32ef1";
constexpr const char* CONTROL = "9fe10001-6bc2-4ce7-8e62-77262df32ef1";
constexpr const char* DATA = "9fe10002-6bc2-4ce7-8e62-77262df32ef1";
constexpr const char* STATUS = "9fe10003-6bc2-4ce7-8e62-77262df32ef1";
constexpr const char* CREDENTIALS = "/.crosspoint/studio/ble.json";
// Lock rules (a violation can freeze the NimBLE host and with it every cloud
// request): `mutex` guards this file's state and is taken by the NimBLE host
// callbacks, so it is never held while calling a NimBLE API. `radioMutex`
// serialises NimBLE init/deinit/advertising/disconnect from app tasks and is
// never taken by callbacks (deinit waits for the host task to stop).
std::recursive_mutex mutex;
std::mutex radioMutex;
std::string authorityOwner, identity, secret, nonce, control, state = "ready", error, task, hash, baseTask;
int64_t taskExpires = 0, planBoundary = 0;
uint32_t epoch = 0, lastActivity = 0;
// released: NimBLE deinitialised to lend its heap to TLS or the flash writer.
bool released = false;
bool started = false, authenticated = false, isConnected = false;
// The user's switch (Settings > Bluetooth); off keeps the stack deinitialised.
bool userEnabled = true;
// Start diagnostics: the stage and code of the last failed start, shown on the
// Bluetooth settings screen so a silent radio can be told apart from a phone
// that is not scanning.
std::string startError, advertisedAs, address;
int startErrorCode = 0;
uint32_t starts = 0, startFailures = 0, connections = 0, advertisingRestarts = 0;
uint32_t completedTransfers = 0, failedTransfers = 0;
uint32_t lastConnectMs = 0, lastDisconnectMs = 0;
bool everConnected = false, everDisconnected = false;
size_t transferTotal = 0;
// Bumped on every radio/session change the UI should repaint for.
std::atomic<uint32_t> linkGeneration{0};
void changed() { linkGeneration.fetch_add(1); }
// What NimBLE (host pools, task stacks) and the controller need to come up;
// below this the start is postponed instead of risking an allocation abort.
constexpr uint32_t START_MIN_FREE_HEAP = 36 * 1024;
constexpr uint32_t START_MIN_MAX_ALLOC = 12 * 1024;
// A phone that holds the link without talking blocks advertising and cloud
// requests; Wi-Fi setup polls STATUS, so real sessions never sit this long.
constexpr uint32_t IDLE_LINK_TIMEOUT_MS = 5UL * 60UL * 1000UL;
// Setup mode (unbound): one-time key K shown in the QR. Each connection pins
// its key: a setup session keeps K until disconnect even after a bind.
std::string setupKey, sessionKey;
bool sessionSetup = false;
enum class BindState { None, Pending, Done, Failed };
BindState bindState = BindState::None;
std::optional<Binding> pendingBinding;
std::optional<WifiRequest> pendingWifi;
std::optional<OtaRequest> pendingOta;
bool scanRequested = false;
WifiState wifiState = WifiState::Idle;
std::string wifiSsid, wifiError, scanState = "idle";
std::vector<ble_setup::Network> networks;
uint16_t connectionHandle = 0xffff;
mbedtls_aes_context aes{};
uint8_t counter[16]{}, stream[16]{};
size_t counterOffset = 0;

std::string hex(const uint8_t* bytes, size_t size) {
  std::string output(size * 2, '0');
  const char* alphabet = "0123456789abcdef";
  for (size_t i = 0; i < size; ++i) {
    output[i * 2] = alphabet[bytes[i] >> 4];
    output[i * 2 + 1] = alphabet[bytes[i] & 15];
  }
  return output;
}
bool unhex(const std::string& value, uint8_t* output, size_t size) {
  if (value.size() != size * 2) return false;
  auto nibble = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
  for (size_t i = 0; i < size; ++i) {
    int a = nibble(value[i * 2]), b = nibble(value[i * 2 + 1]);
    if (a < 0 || b < 0) return false;
    output[i] = (a << 4) | b;
  }
  return true;
}
std::string mac(const std::string& message) { return ble_setup::mac(sessionKey, message); }
bool equalProof(const std::string& a, const std::string& b) {
  if (a.size() != 64 || b.size() != 64) return false;
  unsigned diff = 0;
  for (size_t i = 0; i < 64; ++i) diff |= a[i] ^ b[i];
  return diff == 0;
}
void resetSession() {
  if (authenticated && state == "receiving") {
    StudioFrame::instance().abort();
    ++failedTransfers;  // the phone left mid-transfer
  }
  transferTotal = 0;
  changed();
  if (authenticated) mbedtls_aes_free(&aes);
  authenticated = false;
  sessionSetup = secret.empty();
  sessionKey = sessionSetup ? setupKey : secret;
  control.clear();
  task.clear();
  hash.clear();
  error.clear();
  state = "ready";
  uint8_t random[16];
  esp_fill_random(random, sizeof(random));
  nonce = hex(random, sizeof(random));
  baseTask = StudioFrame::instance().snapshot().task;
  taskExpires = 0;
  planBoundary = std::max(int64_t(0), StudioFrame::instance().nextBoundary(time(nullptr)));
  counterOffset = 0;
  memset(stream, 0, sizeof(stream));
  lastActivity = millis();
}
void fail(const char* reason) {
  if (authenticated && (state == "receiving" || state == "refreshing")) ++failedTransfers;
  error = reason;
  state = "failed";
  if (authenticated) StudioFrame::instance().abort(true);
  changed();
}
const char* wifiStateName() {
  switch (wifiState) {
    case WifiState::Connecting:
      return "connecting";
    case WifiState::Connected:
      return "connected";
    case WifiState::Failed:
      return "failed";
    default:
      return "idle";
  }
}
void addWifi(JsonDocument& doc) {
  JsonObject wifi = doc["wifi"].to<JsonObject>();
  wifi["state"] = wifiStateName();
  if (!wifiSsid.empty()) wifi["ssid"] = wifiSsid;
  if (!wifiError.empty()) wifi["error"] = wifiError;
  doc["scan"] = scanState;
}
const char* otaStateName(firmware_update::Phase phase) {
  switch (phase) {
    case firmware_update::Phase::Downloading:
      return "downloading";
    case firmware_update::Phase::Verifying:
      return "verifying";
    case firmware_update::Phase::Installing:
      return "installing";
    case firmware_update::Phase::Restarting:
      return "restarting";
    case firmware_update::Phase::Failed:
      return "failed";
    default:
      return "idle";
  }
}
// Bound mode only; omitted while no update was requested since boot.
void addOta(JsonDocument& doc) {
  const auto update = firmware_update::snapshot();
  if (update.phase == firmware_update::Phase::Idle && !pendingOta) return;
  JsonObject ota = doc["ota"].to<JsonObject>();
  ota["state"] = pendingOta ? "queued" : otaStateName(update.phase);
  ota["received"] = update.done;
  if (update.phase == firmware_update::Phase::Failed && update.error[0]) ota["error"] = update.error;
}
std::string finishStatus(JsonDocument& doc) {
  std::string result;
  serializeJson(doc, result);
  return scanState == "done" ? ble_setup::withNetworks(result, networks) : result;
}
std::string statusJson() {
  if (sessionSetup) {
    JsonDocument doc;
    doc["protocol"] = 3;
    doc["mode"] = "setup";
    doc["nonce"] = nonce;
    doc["fw"] = CROSSPOINT_VERSION;
    doc["bound"] = bindState == BindState::Done;
    addWifi(doc);
    if (!error.empty()) doc["error"] = error;
    doc["proof"] = mac(ble_setup::setupProofMessage(nonce, identity));
    return finishStatus(doc);
  }
  const auto snapshot = StudioFrame::instance().snapshot();
  if (authenticated && snapshot.task == task) {
    if (snapshot.displayed)
      state = "displayed";
    else if (snapshot.size > StudioFrame::BYTES && snapshot.card.empty())
      state = "scheduled";
  }
  JsonDocument doc;
  doc["device_id"] = identity;
  doc["fw"] = CROSSPOINT_VERSION;
  doc["nonce"] = nonce;
  doc["epoch"] = epoch;
  doc["boundary"] = planBoundary;
  doc["protocol"] = 2;
  doc["base_task"] = baseTask;
  doc["expires"] = taskExpires;
  doc["state"] = state;
  doc["received"] = StudioFrame::instance().received();
  if (!error.empty()) doc["error"] = error;
  if (state == "displayed" || state == "scheduled") {
    doc["task"] = task;
    doc["hash"] = hash;
    doc["card"] = snapshot.card;
    doc["proof"] = mac(state + "2|" + task + "|" + hash + "|" + nonce + "|" + baseTask + "|" +
                       std::to_string(taskExpires) + "|" + snapshot.card);
  } else
    doc["proof"] = mac("hello|" + identity + "|" + nonce + "|" + std::to_string(epoch) + "|" + baseTask + "|" +
                       std::to_string(planBoundary));
  addWifi(doc);
  addOta(doc);
  return finishStatus(doc);
}
// Setup ops (protocol 3) never abort a frame transfer; a bad request only
// sets the STATUS error.
bool reject(const char* reason) {
  error = reason;
  return true;
}
bool validOwner(const std::string& owner) { return owner.size() == 36; }
// Returns true when the control op was a setup/Wi-Fi op (handled here).
bool handleSetupOp(const std::string& op, JsonDocument& doc) {
  const std::string proof = doc["proof"] | "";
  if (op == "scan") {
    if (!equalProof(proof, mac(ble_setup::scanMessage(nonce)))) return reject("authorization_failed");
    error.clear();
    scanRequested = true;
    scanState = "scanning";
    return true;
  }
  if (op == "wifi") {
    const std::string ssid = doc["ssid"] | "", pw = doc["pw"] | "";
    if (!equalProof(proof, mac(ble_setup::wifiMessage(nonce, ssid, pw)))) return reject("authorization_failed");
    std::string password;
    if (ssid.empty() || ssid.size() > 32 || !ble_setup::unseal(sessionKey, "wifi3", nonce, pw, password) ||
        password.size() > 63)
      return reject("invalid_control");
    error.clear();
    pendingWifi = WifiRequest{ssid, password};
    wifiState = WifiState::Connecting;
    wifiSsid = ssid;
    wifiError.clear();
    return true;
  }
  if (op == "bind") {
    if (!sessionSetup || bindState == BindState::Pending || bindState == BindState::Done)
      return reject("invalid_control");
    const std::string owner = doc["owner"] | "", ct = doc["ct"] | "";
    const uint32_t keyEpoch = doc["epoch"] | uint32_t(0);
    if (!equalProof(proof, mac(ble_setup::bindMessage(nonce, owner, keyEpoch, ct))))
      return reject("authorization_failed");
    std::string plaintext;
    Binding binding;
    binding.owner = owner;
    binding.epoch = keyEpoch;
    if (!validOwner(owner) || keyEpoch == 0 || !ble_setup::unseal(sessionKey, "bind3", nonce, ct, plaintext) ||
        !ble_setup::splitBindPlaintext(plaintext, binding.token, binding.secret))
      return reject("invalid_control");
    error.clear();
    pendingBinding = std::move(binding);
    bindState = BindState::Pending;
    return true;
  }
  if (op == "ota") {
    OtaRequest request;
    request.version = doc["version"] | "";
    request.sha256 = doc["sha256"] | "";
    request.bytes = doc["bytes"] | size_t(0);
    request.url = doc["url"] | "";
    if (sessionSetup) return reject("invalid_control");
    if (!equalProof(proof, mac(ble_setup::otaMessage(nonce, request.version, request.sha256, request.bytes,
                                                     request.url))))
      return reject("authorization_failed");
    // The worker validates the image source, size and hash before any download.
    if (request.version.empty() || request.url.empty() || request.sha256.size() != 64 || request.bytes == 0 ||
        pendingOta || firmware_update::snapshot().busy())
      return reject("invalid_control");
    error.clear();
    pendingOta = std::move(request);
    return true;
  }
  return false;
}
class ServerCallbacks final : public NimBLEServerCallbacks {
  // Host-task context: state under `mutex`, NimBLE calls after releasing it.
  void onConnect(NimBLEServer* server, NimBLEConnInfo& info) override {
    bool reject = false;
    {
      std::lock_guard<std::recursive_mutex> lock(mutex);
      reject = isConnected;
      if (!reject) {
        isConnected = true;
        connectionHandle = info.getConnHandle();
        ++connections;
        lastConnectMs = millis();
        everConnected = true;
        resetSession();
      }
    }
    if (reject) server->disconnect(info.getConnHandle());
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo& info, int) override {
    bool advertise = false;
    {
      std::lock_guard<std::recursive_mutex> lock(mutex);
      if (connectionHandle != info.getConnHandle()) return;
      resetSession();
      isConnected = false;
      connectionHandle = 0xffff;
      lastDisconnectMs = millis();
      everDisconnected = true;
      advertise = started && !released && (!secret.empty() || !setupKey.empty());
    }
    if (advertise) NimBLEDevice::startAdvertising();
  }
};
class Callbacks final : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic* characteristic, NimBLEConnInfo& info) override {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (info.getConnHandle() != connectionHandle) return;
    lastActivity = millis();  // a phone polling STATUS is an active session
    characteristic->setValue(statusJson());
  }
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& info) override {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (info.getConnHandle() != connectionHandle) return;
    lastActivity = millis();
    const auto value = characteristic->getValue();
    if (characteristic->getUUID() == NimBLEUUID(DATA)) {
      if (!authenticated || state != "receiving" || value.size() <= 4 || value.size() > 244) {
        fail("unauthorized_or_invalid_chunk");
        return;
      }
      const auto* bytes = reinterpret_cast<const uint8_t*>(value.data());
      const uint32_t offset =
          uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 | uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24;
      if (offset != StudioFrame::instance().received()) {
        fail("offset_mismatch");
        return;
      }
      uint8_t plain[240];
      if (mbedtls_aes_crypt_ctr(&aes, value.size() - 4, &counterOffset, counter, stream, bytes + 4, plain) != 0 ||
          !StudioFrame::instance().append(offset, plain, value.size() - 4))
        fail("storage_or_cipher_error");
      return;
    }
    if (value.size() + control.size() > 512) {
      control.clear();
      fail("control_too_large");
      return;
    }
    control.append(reinterpret_cast<const char*>(value.data()), value.size());
    if (control.empty() || control.back() != '\n') return;
    JsonDocument doc;
    const auto parseError = deserializeJson(doc, control);
    control.clear();
    if (parseError) {
      fail("invalid_control");
      return;
    }
    const std::string op = doc["op"] | "";
    if (handleSetupOp(op, doc)) return;
    if (sessionSetup) {
      reject("unexpected_control");
      return;
    }
    if (op == "begin" && !authenticated) {
      const std::string incomingTask = doc["task"] | "", incomingHash = doc["hash"] | "", proof = doc["proof"] | "";
      const int64_t expires = doc["expires"] | int64_t(0);
      const size_t size = doc["size"] | StudioFrame::BYTES;
      const int64_t phoneTime = doc["time"] | int64_t(0);
      const std::string expected =
          mac(std::string(doc["size"].isNull() ? "studio1|" : "studio2|") + identity + "|" + nonce + "|" +
              std::to_string(epoch) + "|" + incomingTask + "|" + incomingHash + "|" + std::to_string(expires) +
              (doc["size"].isNull() ? "" : "|" + std::to_string(size) + "|" + std::to_string(phoneTime)));
      if (!equalProof(proof, expected)) {
        fail("authorization_failed");
        return;
      }
      if (phoneTime >= 1735689600 && phoneTime < 4102444800 && time(nullptr) < 1735689600) {
        timeval tv{static_cast<time_t>(phoneTime), 0};
        settimeofday(&tv, nullptr);
      }
      const auto active = StudioFrame::instance().snapshot();
      const bool completed = active.task == incomingTask && active.hash == incomingHash;
      if (!completed && !StudioFrame::instance().start(incomingTask, incomingHash, expires, size)) {
        fail("device_busy_or_invalid_frame");
        return;
      }
      uint8_t encryptionKey[32];
      if (!unhex(mac("enc|" + nonce), encryptionKey, sizeof(encryptionKey)) ||
          !unhex(nonce, counter, sizeof(counter))) {
        StudioFrame::instance().abort();
        fail("invalid_key");
        return;
      }
      mbedtls_aes_init(&aes);
      mbedtls_aes_setkey_enc(&aes, encryptionKey, 256);
      authenticated = true;
      task = incomingTask;
      hash = incomingHash;
      taskExpires = expires;
      transferTotal = size;
      state = completed ? (active.displayed ? "displayed" : "refreshing") : "receiving";
      changed();
      size_t skip = completed ? 0 : StudioFrame::instance().received();
      uint64_t blocks = skip / 16;
      for (int i = 15; i >= 0 && blocks; --i) {
        const uint64_t sum = counter[i] + (blocks & 255);
        counter[i] = sum & 255;
        blocks = (blocks >> 8) + (sum >> 8);
      }
      if (skip % 16) {
        uint8_t zero[16]{}, discard[16];
        mbedtls_aes_crypt_ctr(&aes, skip % 16, &counterOffset, counter, stream, zero, discard);
      }
    } else if (op == "commit" && authenticated && state == "receiving") {
      if (!StudioFrame::instance().commit()) {
        fail("frame_validation_failed");
        return;
      }
      StudioFrame::instance().tick(time(nullptr));
      state = "refreshing";
      ++completedTransfers;
      changed();
    } else {
      fail("unexpected_control");
    }
  }
};
ServerCallbacks serverCallbacks;
Callbacks callbacks;

void writeCredentials() {
  Storage.mkdir("/.crosspoint/studio", true);
  HalFile file;
  if (Storage.openFileForWrite("STUDIO", CREDENTIALS, file)) {
    JsonDocument doc;
    doc["owner_id"] = authorityOwner;
    doc["device_id"] = identity;
    doc["secret"] = secret;
    doc["epoch"] = epoch;
    serializeJson(doc, file);
    file.close();
  }
}
bool validIdentity() { return identity.size() == 36; }
bool validSecret() {
  uint8_t decoded[32];
  return unhex(secret, decoded, 32);
}
// Records a failed start and leaves the stack fully torn down, so the next
// tick() retry starts from scratch. Caller holds radioMutex.
void failStartLocked(const char* stage, int code) {
  NimBLEDevice::deinit(true);
  // A start that died between controller init and host sync leaves the
  // controller up; the next attempt needs it idle again.
  if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED) esp_bt_controller_disable();
  if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED) esp_bt_controller_deinit();
  std::lock_guard<std::recursive_mutex> lock(mutex);
  started = false;
  startError = stage;
  startErrorCode = code;
  ++startFailures;
  changed();
  LOG_ERR("BLE", "Start failed at %s (code=%d heap=%u max=%u)", stage, code, (unsigned)ESP.getFreeHeap(),
          (unsigned)ESP.getMaxAllocHeap());
}
// Advertising packet (31 bytes): flags, the 128-bit service UUID phones filter
// on, and the device id prefix as manufacturer data, so a passive scan already
// identifies the device. The complete name goes in the scan response: it does
// not fit next to the UUID. Caller holds radioMutex.
bool configureAdvertisingLocked(const std::string& name) {
  auto* advertising = NimBLEDevice::getAdvertising();
  NimBLEAdvertisementData packet, response;
  // Company id 0xFFFF (unassigned), then the four id characters of the name.
  std::string manufacturer = "\xff\xff";
  manufacturer += name.substr(name.size() >= 4 ? name.size() - 4 : 0);
  if (!packet.setFlags(BLE_HS_ADV_F_DISC_GEN) || !packet.addServiceUUID(NimBLEUUID(SERVICE)) ||
      !packet.setManufacturerData(manufacturer) || !response.setName(name))
    return false;
  advertising->enableScanResponse(true);
  return advertising->setAdvertisementData(packet) && advertising->setScanResponseData(response);
}
// Starts NimBLE for the current identity. Caller holds radioMutex. Returns
// false when the start failed (recorded for diagnostics; tick() retries).
bool startRadioLocked() {
  std::string name;
  {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (started || released || !userEnabled) return true;
    if (secret.empty()) {
      HalFile file;
      if (Storage.openFileForRead("STUDIO", CREDENTIALS, file)) {
        JsonDocument doc;
        const auto parseError = deserializeJson(doc, file);
        file.close();
        if (!parseError) {
          authorityOwner = doc["owner_id"] | "";
          identity = doc["device_id"] | "";
          secret = doc["secret"] | "";
          epoch = doc["epoch"] | 0;
        }
      }
    }
    if (!validIdentity() || (!validSecret() && setupKey.empty())) return true;
    if (validSecret()) setupKey.clear();
    name = ble_setup::advertisedName(identity);
    ++starts;
  }
  const uint32_t freeHeap = ESP.getFreeHeap(), maxAlloc = ESP.getMaxAllocHeap();
  if (freeHeap < START_MIN_FREE_HEAP || maxAlloc < START_MIN_MAX_ALLOC) {
    failStartLocked("low_memory", static_cast<int>(freeHeap / 1024));
    return false;
  }
  if (!NimBLEDevice::init(name)) {
    failStartLocked("init", static_cast<int>(esp_bt_controller_get_status()));
    return false;
  }
  NimBLEDevice::setMTU(247);
  auto* server = NimBLEDevice::createServer();
  server->setCallbacks(&serverCallbacks, false);
  auto* service = server->createService(SERVICE);
  service->createCharacteristic(CONTROL, NIMBLE_PROPERTY::WRITE, 512)->setCallbacks(&callbacks);
  service->createCharacteristic(DATA, NIMBLE_PROPERTY::WRITE, 244)->setCallbacks(&callbacks);
  service->createCharacteristic(STATUS, NIMBLE_PROPERTY::READ, 512)->setCallbacks(&callbacks);
  if (!server->start()) {
    failStartLocked("gatt", 0);
    return false;
  }
  if (!configureAdvertisingLocked(name)) {
    failStartLocked("adv_data", 0);
    return false;
  }
  if (!NimBLEDevice::getAdvertising()->start()) {
    failStartLocked("adv_start", 0);
    return false;
  }
  const std::string mac = NimBLEDevice::getAddress().toString();
  std::lock_guard<std::recursive_mutex> lock(mutex);
  started = true;
  advertisedAs = name;
  address = mac;
  startError.clear();
  startErrorCode = 0;
  changed();
  LOG_INF("BLE", "NimBLE up as %s %s (heap=%u max=%u)", name.c_str(), mac.c_str(), (unsigned)ESP.getFreeHeap(),
          (unsigned)ESP.getMaxAllocHeap());
  return true;
}
// Stops the stack and forgets the live session. Caller holds radioMutex.
void stopRadioLocked() {
  {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (!started) return;
    started = false;
  }
  NimBLEDevice::deinit(true);
  std::lock_guard<std::recursive_mutex> lock(mutex);
  if (isConnected) resetSession();
  isConnected = false;
  connectionHandle = 0xffff;
  changed();
}
// Caller holds radioMutex.
void disconnectLocked(uint16_t handle) {
  if (handle == 0xffff) return;
  if (auto* server = NimBLEDevice::getServer()) server->disconnect(handle);
}
// Caller holds radioMutex.
void advertiseIfIdleLocked() {
  bool advertise = false;
  {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    advertise = started && !released && !isConnected && (!secret.empty() || !setupKey.empty());
  }
  if (advertise) NimBLEDevice::startAdvertising();
}
}  // namespace
bool releaseRadio() {
  std::lock_guard<std::mutex> radio(radioMutex);
  {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (!started || released || isConnected) return false;
    released = true;
    started = false;
  }
  const uint32_t before = ESP.getFreeHeap();
  NimBLEDevice::deinit(true);
  {
    // A connection racing the shutdown is gone with the stack.
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (isConnected) resetSession();
    isConnected = false;
    connectionHandle = 0xffff;
    changed();
  }
  LOG_INF("BLE", "NimBLE released for network/flash (heap %u -> %u, max=%u)", (unsigned)before,
          (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  return true;
}
void restoreRadio() {
  std::lock_guard<std::mutex> radio(radioMutex);
  {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (!released) return;
    released = false;
    changed();
  }
  startRadioLocked();
}
void revoke() {
  std::lock_guard<std::mutex> radio(radioMutex);
  uint16_t handle = 0xffff;
  bool stopAdvertising = false;
  {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (secret.empty()) return;
    if (isConnected) handle = connectionHandle;
    resetSession();
    identity.clear();
    secret.clear();
    epoch = 0;
    bindState = BindState::None;
    Storage.remove(CREDENTIALS);
    stopAdvertising = started;
  }
  disconnectLocked(handle);
  if (stopAdvertising) NimBLEDevice::getAdvertising()->stop();
}
bool adoptAuthority(const std::string& deviceId, const std::string& key, uint32_t keyEpoch,
                    const std::string& owner) {
  std::lock_guard<std::mutex> radio(radioMutex);
  uint16_t handle = 0xffff;
  bool needStart = false;
  {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (deviceId.size() != 36 || !ble_setup::validAuthority(key, keyEpoch)) return false;
    if (identity == deviceId && secret == key && epoch == keyEpoch && authorityOwner == owner) return false;
    if (!authorityOwner.empty() && authorityOwner != owner) StudioFrame::instance().clear();
    if (isConnected) handle = connectionHandle;
    authorityOwner = owner;
    identity = deviceId;
    secret = key;
    epoch = keyEpoch;
    setupKey.clear();
    writeCredentials();
    needStart = !started && !released;
  }
  disconnectLocked(handle);
  if (needStart)
    startRadioLocked();
  else
    advertiseIfIdleLocked();
  return true;
}
void begin() {
  std::lock_guard<std::mutex> radio(radioMutex);
  startRadioLocked();
}
Link link() {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  Link result;
  result.radio = !userEnabled   ? Radio::Off
                 : released     ? Radio::Paused
                 : isConnected  ? Radio::Connected
                 : started      ? Radio::Advertising
                 : startError.empty() ? Radio::Idle
                                      : Radio::Failed;
  result.setupMode = secret.empty();
  if (isConnected && authenticated && state == "receiving") {
    result.transfer = Transfer::Receiving;
    result.received = StudioFrame::instance().received();
    result.total = transferTotal;
  } else if (isConnected && authenticated && state == "refreshing") {
    result.transfer = Transfer::Refreshing;
  }
  result.completed = completedTransfers;
  result.failed = failedTransfers;
  result.generation = linkGeneration.load();
  return result;
}
Diagnostics diagnostics() {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  Diagnostics result;
  result.link = link();
  result.name = advertisedAs.empty() && validIdentity() ? ble_setup::advertisedName(identity) : advertisedAs;
  result.address = address;
  result.error = startError;
  result.errorCode = startErrorCode;
  result.starts = starts;
  result.startFailures = startFailures;
  result.connections = connections;
  result.advertisingRestarts = advertisingRestarts;
  const uint32_t now = millis();
  if (everConnected) result.sinceConnect = static_cast<int32_t>((now - lastConnectMs) / 1000);
  if (everDisconnected) result.sinceDisconnect = static_cast<int32_t>((now - lastDisconnectMs) / 1000);
  return result;
}
bool enabled() {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  return userEnabled;
}
void setEnabled(const bool enable) {
  std::lock_guard<std::mutex> radio(radioMutex);
  {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (userEnabled == enable) return;
    userEnabled = enable;
    startError.clear();
    startErrorCode = 0;
    changed();
  }
  LOG_INF("BLE", "Bluetooth switched %s", enable ? "on" : "off");
  if (enable)
    startRadioLocked();
  else
    stopRadioLocked();
}
void restart() {
  std::lock_guard<std::mutex> radio(radioMutex);
  {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (released || !userEnabled) return;
  }
  LOG_INF("BLE", "Restarting NimBLE on request");
  stopRadioLocked();
  startRadioLocked();
}
// UI loop. Starts the radio (retrying a failed start with backoff), keeps it
// advertising, and drops stalled sessions. Never blocks on the radio: while
// the worker is releasing or restoring the stack this tick is skipped.
void tick() {
  static uint32_t nextStartMs = 0, lastWatchMs = 0, retryDelayMs = 5000;
  static uint8_t advertisingFaults = 0;
  const uint32_t now = millis();
  bool start = false, watch = false;
  uint16_t stalled = 0xffff;
  {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    start = userEnabled && !started && !released && static_cast<int32_t>(now - nextStartMs) >= 0;
    watch = started && !released && !isConnected && (!secret.empty() || !setupKey.empty()) &&
            now - lastWatchMs >= 3000;
    if (isConnected && now - lastActivity > 30000 && state == "receiving") {
      StudioFrame::instance().abort();
      ++failedTransfers;
      error = "transfer_timeout";
      state = "paused";
      stalled = connectionHandle;
      changed();
    } else if (isConnected && state != "receiving" && now - lastActivity > IDLE_LINK_TIMEOUT_MS) {
      stalled = connectionHandle;
    }
  }
  if (!start && !watch && stalled == 0xffff) return;
  std::unique_lock<std::mutex> radio(radioMutex, std::try_to_lock);
  if (!radio.owns_lock()) return;
  if (start) {
    if (startRadioLocked()) {
      retryDelayMs = 5000;
      nextStartMs = now + retryDelayMs;
    } else {
      nextStartMs = now + retryDelayMs;
      retryDelayMs = std::min<uint32_t>(retryDelayMs * 2, 60000);
    }
  }
  if (watch) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    watch = started && !released && !isConnected;  // the worker may have released it meanwhile
  }
  if (watch) {
    lastWatchMs = now;
    // The controller can stop advertising on its own (host reset, a failed
    // restart after a disconnect); without this the device stays invisible
    // until the next reboot.
    if (NimBLEDevice::getAdvertising()->isAdvertising()) {
      advertisingFaults = 0;
    } else if (NimBLEDevice::startAdvertising()) {
      advertisingFaults = 0;
      std::lock_guard<std::recursive_mutex> lock(mutex);
      ++advertisingRestarts;
      changed();
      LOG_INF("BLE", "Advertising restarted");
    } else if (++advertisingFaults >= 3) {
      advertisingFaults = 0;
      LOG_ERR("BLE", "Advertising cannot be restarted; restarting the stack");
      stopRadioLocked();
      failStartLocked("adv_restart", 0);
      nextStartMs = now + 1000;
    }
  }
  if (stalled != 0xffff) disconnectLocked(stalled);
}
bool connected() {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  return isConnected;
}
void setup(const std::string& deviceId) {
  std::lock_guard<std::mutex> radio(radioMutex);
  bool needStart = false;
  {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (!secret.empty() || deviceId.size() != 36) return;
    if (identity == deviceId && !setupKey.empty()) return;
    identity = deviceId;
    uint8_t key[ble_setup::SETUP_KEY_BYTES];
    esp_fill_random(key, sizeof(key));
    setupKey = ble_setup::hex(key, sizeof(key));
    bindState = BindState::None;
    needStart = !started && !released;
  }
  if (needStart)
    startRadioLocked();
  else
    advertiseIfIdleLocked();
}
std::string setupPayload() {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  if (!secret.empty() || setupKey.empty() || !validIdentity()) return "";
  return ble_setup::setupQrPayload(identity, setupKey);
}
bool takeBinding(Binding& out) {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  if (!pendingBinding) return false;
  out = std::move(*pendingBinding);
  pendingBinding.reset();
  return true;
}
void finishBinding(bool ok, const Binding& binding) {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  if (!ok) {
    bindState = BindState::Failed;
    error = "bind_failed";
    return;
  }
  // The live setup session keeps K (sessionKey) until the phone disconnects;
  // the next connection resets into bound mode with this secret.
  authorityOwner = binding.owner;
  secret = binding.secret;
  epoch = binding.epoch;
  setupKey.clear();
  bindState = BindState::Done;
  writeCredentials();
}
bool takeWifiRequest(WifiRequest& out) {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  if (!pendingWifi) return false;
  out = std::move(*pendingWifi);
  pendingWifi.reset();
  return true;
}
bool takeScanRequest() {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  const bool requested = scanRequested;
  scanRequested = false;
  return requested;
}
bool takeOtaRequest(OtaRequest& out) {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  if (!pendingOta) return false;
  out = std::move(*pendingOta);
  pendingOta.reset();
  return true;
}
void reportWifi(WifiState state, const std::string& ssid, const char* reason) {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  wifiState = state;
  wifiSsid = ssid;
  wifiError = reason ? reason : "";
}
void reportScan(bool scanning, std::vector<ble_setup::Network> found) {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  scanState = scanning ? "scanning" : "done";
  if (!scanning) networks = ble_setup::normalizeNetworks(std::move(found));
}
}  // namespace studio_ble
#else
#include <Arduino.h>

#include <cstdlib>
#include <random>

namespace studio_ble {
void revoke() {}
bool adoptAuthority(const std::string&, const std::string&, uint32_t, const std::string&) { return false; }
bool releaseRadio() { return false; }
void restoreRadio() {}
void begin() {}
void tick() {}
namespace {
bool simEnabled = true;
}
// The simulator has no radio. CROSSPOINT_SIM_BLE_DEMO plays a scripted phone
// session (advertise, connect, transfer, refresh) so the status notices and
// the Bluetooth screen can be previewed; without it the radio reads as idle.
Link link() {
  Link result;
  result.setupMode = true;
  if (!simEnabled) {
    result.radio = Radio::Off;
    return result;
  }
  static const bool demo = std::getenv("CROSSPOINT_SIM_BLE_DEMO") != nullptr;
  if (!demo) return result;
  static const uint32_t startedMs = millis();
  const uint32_t elapsed = millis() - startedMs;
  constexpr size_t total = 400000;
  result.radio = elapsed >= 4000 && elapsed < 20000 ? Radio::Connected : Radio::Advertising;
  result.total = total;
  if (elapsed >= 7000 && elapsed < 17000) {
    result.transfer = Transfer::Receiving;
    result.received = total / 10000 * (elapsed - 7000);
  } else if (elapsed >= 17000 && elapsed < 20000) {
    result.transfer = Transfer::Refreshing;
  }
  result.completed = elapsed >= 17000 ? 1 : 0;
  result.generation = elapsed < 4000 ? 1 : elapsed < 7000 ? 2 : elapsed < 17000 ? 3 : elapsed < 20000 ? 4 : 5;
  return result;
}
Diagnostics diagnostics() {
  Diagnostics result;
  result.link = link();
  result.name = "StockStick-SIM0";
  result.address = "00:00:00:00:00:00";
  result.starts = 1;
  result.connections = result.link.generation >= 2 ? 1 : 0;
  return result;
}
void setEnabled(const bool enable) { simEnabled = enable; }
bool enabled() { return simEnabled; }
void restart() {}
bool connected() { return link().radio == Radio::Connected; }
// The desktop/web simulator has no BLE radio; it still shows the setup QR so
// the unbound screen can be previewed.
namespace {
std::string simIdentity, simKey;
}
void setup(const std::string& deviceId) {
  if (deviceId.size() != 36 || (simIdentity == deviceId && !simKey.empty())) return;
  simIdentity = deviceId;
  std::random_device random;
  uint8_t key[ble_setup::SETUP_KEY_BYTES];
  for (auto& byte : key) byte = static_cast<uint8_t>(random());
  simKey = ble_setup::hex(key, sizeof(key));
}
std::string setupPayload() { return simKey.empty() ? "" : ble_setup::setupQrPayload(simIdentity, simKey); }
bool takeBinding(Binding&) { return false; }
void finishBinding(bool, const Binding&) {}
bool takeWifiRequest(WifiRequest&) { return false; }
bool takeScanRequest() { return false; }
bool takeOtaRequest(OtaRequest&) { return false; }
void reportWifi(WifiState, const std::string&, const char*) {}
void reportScan(bool, std::vector<ble_setup::Network>) {}
}  // namespace studio_ble
#endif
