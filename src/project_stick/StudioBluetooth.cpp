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
#include <StudioTransfer.h>

#include "StudioReceiver.h"
#include <optional>

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <esp_bt.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <mbedtls/aes.h>
#include <mbedtls/md.h>
#include <sys/time.h>

namespace studio_ble {
namespace {
constexpr const char* SERVICE = "9fe10000-6bc2-4ce7-8e62-77262df32ef1";
constexpr const char* CONTROL = "9fe10001-6bc2-4ce7-8e62-77262df32ef1";
constexpr const char* DATA = "9fe10002-6bc2-4ce7-8e62-77262df32ef1";
constexpr const char* STATUS = "9fe10003-6bc2-4ce7-8e62-77262df32ef1";
constexpr const char* STATE = "9fe10004-6bc2-4ce7-8e62-77262df32ef1";
constexpr const char* PROGRESS = "9fe10005-6bc2-4ce7-8e62-77262df32ef1";
// ATT MTU the phone asks for (protocol 4); DATA carries MTU - 3 bytes.
constexpr uint16_t ATT_MTU = 517;
constexpr size_t MAX_PAYLOAD = ATT_MTU - 3 - 4;
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
// A transfer stopped in a way the phone resumes on its own (a stall, a dropped
// link, an offset mismatch): the UI shows "waiting for the phone" instead of a
// failure, and only after RESUME_WAIT_MS without a new `begin` counts it as failed.
constexpr uint32_t RESUME_WAIT_MS = 45UL * 1000UL;
uint32_t resumeWaitSinceMs = 0;
void awaitResume() {
  if (resumeWaitSinceMs == 0) resumeWaitSinceMs = millis() ? millis() : 1;
}
uint32_t lastConnectMs = 0, lastDisconnectMs = 0;
bool everConnected = false, everDisconnected = false;
size_t transferTotal = 0;
// NimBLE host callbacks run on a small stack and must never touch the SD card
// (the 2.4.3 crash: a FAT long-name open from onWrite overflowed it). They
// verify, decrypt and queue; pump(), on the dedicated writer task, runs the
// protocol 4 receiver (inflate, frame copies, SD writes), so an e-paper
// refresh on the UI loop never stalls reception.
struct Chunk {
  uint32_t offset;
  uint16_t length;
  uint8_t bytes[MAX_PAYLOAD];
};
// 16 slots of one MTU-sized write (~8 KB static), matching the phone's 8 KB
// window (BLE-TRANSFER-V4 §7). DATA arrives as Write Without Response, so the
// host task never waits for a slot: a write that finds the queue full is
// dropped and the phone resends from the last PROGRESS `received`. PROGRESS
// notifications are held back while the queue is over half full, which is
// what paces the phone.
constexpr size_t QUEUE_SLOTS = 16;
Chunk chunkQueue[QUEUE_SLOTS];
size_t queueHead = 0, queueCount = 0;       // under `mutex`
size_t queuedEnd = 0, announcedOffset = 0;  // bytes accepted from the phone; offset promised at `begin4`
uint32_t sessionId = 0;                     // bumps on every resetSession; pump() drops stale results
enum class PendingOp : uint8_t { None, Start, Commit };
PendingOp pendingOp = PendingOp::None;
std::string pendingTask, pendingHash;
int64_t pendingExpires = 0;
size_t pendingSize = 0, pendingHeader = 0;
StudioReceiver::Resume pendingResume;
bool pendingAbort = false, pendingDiscard = false;
// Protocol 4 progress (PROGRESS characteristic and STATUS `need`).
NimBLECharacteristic* progressChar = nullptr;  // under `mutex`; null while the stack is down
uint32_t progressSeq = 0;
size_t lastNotified = 0, transferHeader = 0;
uint32_t packetsSinceNotify = 0, droppedWrites = 0, lastGapNotifyMs = 0;
bool progressPending = false;  // progress the phone has not been told about yet
// offset_mismatch is reported once, on the notification right after a dropped
// out-of-order write (BLE-TRANSFER-V4 §7), never as a sticky STATUS error.
bool gapReported = false;
// Nonce rotation for link reuse: after a transfer ends (displayed, scheduled,
// failed) and the phone has read that outcome once, the next STATUS read at
// least ROTATE_AFTER_MS later starts a fresh session (new N, new hello) on
// the same connection; the phone reuses a link only when N changed.
constexpr uint32_t ROTATE_AFTER_MS = 2000;
uint32_t outcomeAtMs = 0;
bool outcomeRead = false;
bool framesPhase = false;      // header resolved: `need` is known
std::string needValue;
uint16_t needCount = 0;
size_t rebuiltBytes = 0;  // bytes of the rebuilt file on the card (device progress bar)
TaskHandle_t writerTask = nullptr;
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
std::optional<SyncRequest> pendingSync;
bool pendingUnbind = false;
// STATE characteristic value, built on the UI loop (setState) and copied by onRead.
std::string stateJson = "{}";
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
// Caller holds `mutex`. Returns every queued chunk's slot and forgets queued ops.
void dropQueueLocked() {
  queueCount = 0;
  queueHead = 0;
  pendingOp = PendingOp::None;
}
// Caller holds `mutex`. pump() closes (or discards) the partial transfer.
void requestAbortLocked(const bool discard) {
  pendingAbort = true;
  pendingDiscard = pendingDiscard || discard;
}
void resetSession() {
  if (authenticated && state == "receiving") awaitResume();  // the phone left mid-transfer; it reconnects and resumes
  if (authenticated) requestAbortLocked(false);              // keep the partial for a resume
  dropQueueLocked();
  queuedEnd = announcedOffset = lastNotified = 0;
  packetsSinceNotify = 0;
  progressPending = framesPhase = gapReported = outcomeRead = false;
  outcomeAtMs = 0;
  needValue.clear();
  needCount = 0;
  rebuiltBytes = 0;
  transferHeader = 0;
  ++sessionId;
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
// `discard` drops the partial on the card (protocol, cipher or validation
// failures: the bytes cannot be trusted); a stall keeps it so the retry
// resumes from what reached the card instead of starting over.
void fail(const char* reason, const bool discard = true) {
  // The first cause is the one the phone must see; later chunks arriving
  // after a failure would otherwise overwrite it (the 2.6.0 reports all read
  // "unauthorized_or_invalid_chunk" whatever had actually gone wrong).
  if (state == "failed" && !error.empty()) return;
  if (authenticated && (state == "receiving" || state == "refreshing")) {
    if (discard)
      ++failedTransfers;
    else
      awaitResume();
  }
  error = reason;
  state = "failed";
  outcomeAtMs = millis() ? millis() : 1;
  outcomeRead = false;
  if (authenticated) {
    dropQueueLocked();
    requestAbortLocked(discard);
  }
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
// A finished transfer's outcome, from the installed content: `displayed` once
// the frame is on the panel, `scheduled` for a program with no current card.
StudioFrame::Snapshot refreshOutcomeLocked() {
  auto snapshot = StudioFrame::instance().snapshot();
  if (authenticated && snapshot.task == task && state != "receiving" && state != "failed") {
    const char* outcome = snapshot.displayed ? "displayed"
                          : snapshot.size > StudioFrame::BYTES && snapshot.card.empty() ? "scheduled"
                                                                                          : nullptr;
    if (outcome && state != outcome) {
      state = outcome;
      outcomeAtMs = millis() ? millis() : 1;
      outcomeRead = false;
    }
  }
  return snapshot;
}
studio_v4::State progressStateLocked() {
  if (state == "failed") return studio_v4::State::Failed;
  if (state == "paused") return studio_v4::State::Paused;
  if (!authenticated) return studio_v4::State::Idle;
  if (state == "receiving") return framesPhase ? studio_v4::State::ReceivingFrames : studio_v4::State::ReceivingHeader;
  if (state == "refreshing") return studio_v4::State::Committing;
  if (state == "displayed") return studio_v4::State::Displayed;
  if (state == "scheduled") return studio_v4::State::Scheduled;
  return studio_v4::State::Idle;
}
// PROGRESS value (16 bytes, BLE-TRANSFER-V4 §5); marks it as reported.
void buildProgressLocked(uint8_t out[studio_v4::PROGRESS_BYTES]) {
  if (authenticated && state == "refreshing") refreshOutcomeLocked();
  studio_v4::encodeProgress(out, static_cast<uint32_t>(queuedEnd), progressStateLocked(),
                            studio_v4::errorFromName(error), needCount, ++progressSeq);
  if (gapReported) {
    out[5] = static_cast<uint8_t>(studio_v4::Error::OffsetMismatch);
    gapReported = false;
  }
  lastNotified = queuedEnd;
  packetsSinceNotify = 0;
  progressPending = false;
}
// Notifies PROGRESS. From a NimBLE host callback the stack is up by
// definition; app tasks go through notifyProgress(), which holds radioMutex so
// the stack cannot be torn down underneath the call. `mutex` is never held
// across the NimBLE call (lock rules above).
void notifyProgressUnguarded() {
  uint8_t value[studio_v4::PROGRESS_BYTES];
  NimBLECharacteristic* characteristic = nullptr;
  uint16_t handle = 0xffff;
  {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (!isConnected || !progressChar || released) return;
    buildProgressLocked(value);
    characteristic = progressChar;
    handle = connectionHandle;
  }
  characteristic->notify(value, sizeof(value), handle);
}
void notifyProgress() {
  std::unique_lock<std::mutex> radio(radioMutex, std::try_to_lock);
  if (!radio.owns_lock()) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    progressPending = true;  // the next drain or tick reports it
    return;
  }
  notifyProgressUnguarded();
}
// Link parameters for a transfer (BLE-TRANSFER-V4 §3): 2M PHY, 251-byte link
// layer packets and a 7.5-15 ms interval; relaxed to 30-50 ms afterwards.
// Phones that refuse keep their own values. Host-callback or radioMutex context.
void fastLink(uint16_t handle) {
  auto* server = NimBLEDevice::getServer();
  if (!server || handle == 0xffff) return;
  server->updatePhy(handle, BLE_GAP_LE_PHY_2M_MASK, BLE_GAP_LE_PHY_2M_MASK, 0);
  server->setDataLen(handle, 251);
  server->updateConnParams(handle, 6, 12, 0, 400);
}
void relaxLink() {
  std::unique_lock<std::mutex> radio(radioMutex, std::try_to_lock);
  if (!radio.owns_lock()) return;
  uint16_t handle = 0xffff;
  {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (!isConnected || released || !started) return;
    handle = connectionHandle;
  }
  if (auto* server = NimBLEDevice::getServer()) server->updateConnParams(handle, 24, 40, 0, 400);
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
  const auto snapshot = refreshOutcomeLocked();
  JsonDocument doc;
  doc["device_id"] = identity;
  doc["fw"] = CROSSPOINT_VERSION;
  doc["nonce"] = nonce;
  doc["epoch"] = epoch;
  doc["boundary"] = planBoundary;
  doc["protocol"] = 4;
  doc["base_task"] = baseTask;
  doc["expires"] = taskExpires;
  doc["state"] = state;
  // While receiving: bytes accepted from the phone (queued chunks included),
  // which is what its progress check compares against.
  doc["received"] = authenticated && state == "receiving" ? queuedEnd : StudioFrame::instance().received();
  if (!error.empty()) doc["error"] = error;
  if (framesPhase) doc["need"] = needValue;
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
  if (op == "sync") {
    if (sessionSetup) return reject("invalid_control");
    SyncRequest request;
    request.time = doc["time"] | int64_t(0);
    request.trading = doc["trading"] | -1;
    request.otaAck = doc["ota_ack"] | false;
    JsonArrayConst ack = doc["ack"].as<JsonArrayConst>();
    if (!equalProof(proof, mac(ble_setup::syncMessage(nonce, request.time, request.trading, ack.size(),
                                                      request.otaAck))))
      return reject("authorization_failed");
    if (request.trading < -1 || request.trading > 1 || ack.size() > 32) return reject("invalid_control");
    request.ack.reserve(ack.size());
    for (JsonVariantConst entry : ack) {
      const char* id = entry.as<const char*>();
      if (id && *id) request.ack.emplace_back(id);
    }
    error.clear();
    pendingSync = std::move(request);
    return true;
  }
  if (op == "unbind") {
    if (sessionSetup) return reject("invalid_control");
    if (!equalProof(proof, mac(ble_setup::unbindMessage(nonce)))) return reject("authorization_failed");
    error.clear();
    pendingUnbind = true;
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
    if (reject) {
      server->disconnect(info.getConnHandle());
      return;
    }
    // Most sessions are content pushes: ask for the fast link right away.
    fastLink(info.getConnHandle());
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
    if (characteristic->getUUID() == NimBLEUUID(STATE)) {
      characteristic->setValue(stateJson);
      return;
    }
    if (characteristic->getUUID() == NimBLEUUID(PROGRESS)) {
      uint8_t value[studio_v4::PROGRESS_BYTES];
      buildProgressLocked(value);
      characteristic->setValue(value, sizeof(value));
      return;
    }
    const bool ended = state == "displayed" || state == "scheduled" || state == "failed";
    if (ended && outcomeRead && outcomeAtMs && millis() - outcomeAtMs >= ROTATE_AFTER_MS) {
      resetSession();  // fresh N: the next push may reuse this connection
    }
    characteristic->setValue(statusJson());
    if (state == "displayed" || state == "scheduled" || state == "failed") outcomeRead = true;
  }
  // DATA: one encrypted chunk of the protocol 4 stream (Write Without
  // Response). Never blocks the host task: a duplicate or out-of-order write
  // (the phone rewinding its window) and a write that finds the queue full are
  // dropped, and the phone resends from the last PROGRESS `received`.
  void onData(NimBLECharacteristic* characteristic, NimBLEConnInfo& info) {
    const auto value = characteristic->getValue();
    bool notify = false;
    {
      std::lock_guard<std::recursive_mutex> lock(mutex);
      if (info.getConnHandle() != connectionHandle) return;
      lastActivity = millis();
      if (state == "failed" || state == "paused") return;  // in-flight writes after a stop: drop, keep the cause
      if (!authenticated || state != "receiving" || value.size() <= 4 || value.size() > 4 + MAX_PAYLOAD) {
        fail("unauthorized_or_invalid_chunk");
        notify = true;
      } else {
        const auto* bytes = reinterpret_cast<const uint8_t*>(value.data());
        const uint32_t offset =
            uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 | uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24;
        const uint32_t now = millis();
        if (offset != queuedEnd) {
          ++droppedWrites;
          // A gap means writes were lost: tell the phone where to resume (rate-limited).
          if (offset > queuedEnd && now - lastGapNotifyMs >= 200) {
            lastGapNotifyMs = now;
            gapReported = true;
            notify = true;
          }
        } else if (queueCount == QUEUE_SLOTS) {
          ++droppedWrites;
          progressPending = true;
        } else {
          Chunk& slot = chunkQueue[(queueHead + queueCount) % QUEUE_SLOTS];
          slot.offset = offset;
          slot.length = static_cast<uint16_t>(value.size() - 4);
          if (mbedtls_aes_crypt_ctr(&aes, slot.length, &counterOffset, counter, stream, bytes + 4, slot.bytes) !=
              0) {
            fail("storage_or_cipher_error");
            notify = true;
          } else {
            ++queueCount;
            queuedEnd += slot.length;
            ++packetsSinceNotify;
            if (packetsSinceNotify >= 4 || queuedEnd - lastNotified >= 4096) {
              if (queueCount <= QUEUE_SLOTS / 2)
                notify = true;
              else
                progressPending = true;  // reported once the writer drains the queue
            }
          }
        }
      }
    }
    if (writerTask) xTaskNotifyGive(writerTask);
    if (notify) notifyProgressUnguarded();
  }
  // begin4 (BLE-TRANSFER-V4 §4.2). Caller holds `mutex`. Returns whether the
  // phone should get a PROGRESS notification now (fresh header phase or a
  // failure); a resume or a single frame is announced once the writer task
  // has resolved `need`.
  bool beginTransferLocked(JsonDocument& doc) {
    const std::string incomingTask = doc["task"] | "", incomingHash = doc["hash"] | "", proof = doc["proof"] | "";
    const int64_t expires = doc["expires"] | int64_t(0);
    const size_t size = doc["size"] | size_t(0), header = doc["header"] | size_t(0);
    const int64_t phoneTime = doc["time"] | int64_t(0);
    if (!equalProof(proof, mac(studio_v4::beginMessage(identity, nonce, epoch, incomingTask, incomingHash, expires,
                                                       size, header, phoneTime)))) {
      fail("authorization_failed");
      return true;
    }
    // Geometry: a single frame, or an SSP1 header plus whole frames (§4.6).
    const bool single = header == 0;
    const size_t frames = single ? 1 : (size > header ? (size - header) / studio_v4::FRAME_BYTES : 0);
    if (single ? size != StudioFrame::BYTES
               : header < 10 || header > 8 + 32768 || size < header + studio_v4::FRAME_BYTES ||
                     (size - header) % studio_v4::FRAME_BYTES != 0 || size > StudioFrame::MAX_BYTES) {
      fail("frame_validation_failed");
      return true;
    }
    if (frames > studio_v4::MAX_FRAMES) {
      fail("too_many_frames");
      return true;
    }
    if (phoneTime >= 1735689600 && phoneTime < 4102444800 && time(nullptr) < 1735689600) {
      timeval tv{static_cast<time_t>(phoneTime), 0};
      settimeofday(&tv, nullptr);
    }
    const auto active = StudioFrame::instance().snapshot();
    const bool completed = active.task == incomingTask && active.hash == incomingHash;
    StudioReceiver::Resume resume;
    if (!completed) {
      if (StudioFrame::instance().busy() || pendingOp != PendingOp::None || pendingAbort ||
          incomingTask.size() != 36) {
        fail("device_busy");
        return true;
      }
      // The RAM copy of the partial record: no SD access in the host task.
      resume = StudioReceiver::resumeFor(header, size,
                                         StudioFrame::instance().resumeOffset(incomingTask, incomingHash, size));
    }
    uint8_t encryptionKey[32];
    if (!unhex(mac("enc|" + nonce), encryptionKey, sizeof(encryptionKey)) ||
        !unhex(nonce, counter, sizeof(counter))) {
      fail("authorization_failed");
      return true;
    }
    announcedOffset = queuedEnd = lastNotified = completed ? 0 : resume.offset;
    if (!completed) {
      pendingTask = incomingTask;
      pendingHash = incomingHash;
      pendingExpires = expires;
      pendingSize = size;
      pendingHeader = header;
      pendingResume = resume;
      pendingOp = PendingOp::Start;
    }
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, encryptionKey, 256);
    authenticated = true;
    resumeWaitSinceMs = 0;  // the phone is back (same task resumes, or a new one starts)
    task = incomingTask;
    hash = incomingHash;
    taskExpires = expires;
    transferTotal = size;
    transferHeader = header;
    framesPhase = false;
    needValue.clear();
    needCount = 0;
    packetsSinceNotify = 0;
    state = completed ? (active.displayed ? "displayed" : "refreshing") : "receiving";
    if (state == "displayed") {
      outcomeAtMs = millis() ? millis() : 1;
      outcomeRead = false;
    }
    changed();
    // Seek the cipher stream to the resume point.
    const size_t skip = queuedEnd;
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
    // A fresh program starts with the header; everything else waits for `need`.
    return completed || (!single && !resume.headerDone);
  }
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& info) override {
    if (characteristic->getUUID() == NimBLEUUID(DATA)) {
      onData(characteristic, info);
      return;
    }
    bool notify = false, fast = false;
    uint16_t handle = 0xffff;
    {
      std::lock_guard<std::recursive_mutex> lock(mutex);
      if (info.getConnHandle() != connectionHandle) return;
      lastActivity = millis();
      const auto value = characteristic->getValue();
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
      if (op == "begin4" && !authenticated) {
        notify = beginTransferLocked(doc);
        fast = state == "receiving";
      } else if (op == "commit" && authenticated && state == "receiving" && pendingOp == PendingOp::None) {
        // The writer task checks, once every queued chunk is applied, that the
        // stream delivered every needed frame; STATUS stays "receiving" until then.
        pendingOp = PendingOp::Commit;
      } else {
        fail("unexpected_control");
        notify = true;
      }
      handle = connectionHandle;
    }
    if (writerTask) xTaskNotifyGive(writerTask);
    if (fast) fastLink(handle);
    if (notify) notifyProgressUnguarded();
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
  {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    progressChar = nullptr;
  }
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
  NimBLEDevice::setMTU(ATT_MTU);
  // Prefer 2M PHY for every connection (BLE-TRANSFER-V4 §3); fastLink() asks again per link.
  NimBLEDevice::setDefaultPhy(BLE_GAP_LE_PHY_2M_MASK, BLE_GAP_LE_PHY_2M_MASK);
  auto* server = NimBLEDevice::createServer();
  server->setCallbacks(&serverCallbacks, false);
  auto* service = server->createService(SERVICE);
  service->createCharacteristic(CONTROL, NIMBLE_PROPERTY::WRITE, 512)->setCallbacks(&callbacks);
  service->createCharacteristic(DATA, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR, 4 + MAX_PAYLOAD)
      ->setCallbacks(&callbacks);
  service->createCharacteristic(STATUS, NIMBLE_PROPERTY::READ, 512)->setCallbacks(&callbacks);
  service->createCharacteristic(STATE, NIMBLE_PROPERTY::READ, 512)->setCallbacks(&callbacks);
  auto* progress =
      service->createCharacteristic(PROGRESS, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY, studio_v4::PROGRESS_BYTES);
  progress->setCallbacks(&callbacks);
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
  progressChar = progress;
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
    progressChar = nullptr;
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
    progressChar = nullptr;
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
void writerLoop(void*);
// Created once from setup() with a static stack and TCB (like the sync
// worker), so it exists before Wi-Fi and NimBLE allocate and can never fail.
// 6 KB: the deepest path is a commit (StudioFrame verify with a 512-byte
// block, program JSON parse, state write); every receiver buffer is static.
// The high-water mark is logged after each transfer.
void startWriter() {
  constexpr uint32_t STACK_BYTES = 6144;
  static StackType_t stack[STACK_BYTES];
  static StaticTask_t taskBuffer;
  if (writerTask) return;
  // Priority 2: above the UI loop and the render task (1), so an e-paper
  // refresh never stalls the chunk queue; below the NimBLE host.
  TaskHandle_t handle = xTaskCreateStatic(&writerLoop, "StudioWriter", STACK_BYTES, nullptr, 2, stack, &taskBuffer);
  std::lock_guard<std::recursive_mutex> lock(mutex);
  writerTask = handle;
}
uint32_t writerStackFree() { return writerTask ? uxTaskGetStackHighWaterMark(writerTask) : 0; }
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
    // Progress of the rebuilt file: held frames count as soon as they are copied.
    result.received = rebuiltBytes;
    result.total = transferTotal;
  } else if (isConnected && authenticated && state == "refreshing") {
    result.transfer = Transfer::Refreshing;
  }
  result.completed = completedTransfers;
  result.failed = failedTransfers;
  result.resuming = resumeWaitSinceMs != 0 && !(isConnected && authenticated);
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
  // Stack headroom of the NimBLE host task (its overflow was the 2.4.3 crash).
  if (started) {
    if (TaskHandle_t host = xTaskGetHandle("nimble_host")) result.hostStackFree = uxTaskGetStackHighWaterMark(host);
  }
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
// Writer task: applies the transfer work the host callbacks queued, in order
// (abort, start, chunks, commit), through the protocol 4 receiver. The SD card
// is never touched with `mutex` held, so the callbacks keep answering the
// phone meanwhile.
void pump() {
  static Chunk chunk;  // off the writer task's stack
  auto& receiver = StudioReceiver::instance();
  bool notify = false, relax = false;
  for (size_t guard = 0; guard < QUEUE_SLOTS + 2; ++guard) {
    enum class Job : uint8_t { None, Abort, Start, Chunk, Commit } job = Job::None;
    std::string startTask, startHash;
    int64_t startExpires = 0;
    size_t startSize = 0, startHeader = 0;
    StudioReceiver::Resume startResume;
    bool discard = false;
    uint32_t session = 0;
    {
      std::lock_guard<std::recursive_mutex> lock(mutex);
      session = sessionId;
      if (pendingAbort) {
        job = Job::Abort;
        discard = pendingDiscard;
        pendingAbort = pendingDiscard = false;
      } else if (pendingOp == PendingOp::Start) {
        job = Job::Start;
        startTask = pendingTask;
        startHash = pendingHash;
        startExpires = pendingExpires;
        startSize = pendingSize;
        startHeader = pendingHeader;
        startResume = pendingResume;
        pendingOp = PendingOp::None;
      } else if (queueCount) {
        job = Job::Chunk;
        chunk = chunkQueue[queueHead];
        queueHead = (queueHead + 1) % QUEUE_SLOTS;
        --queueCount;
      } else if (pendingOp == PendingOp::Commit) {
        job = Job::Commit;
        pendingOp = PendingOp::None;
      }
    }
    if (job == Job::None) break;
    if (job == Job::Abort) {
      receiver.abort(discard);
      continue;
    }
    // Moves to the frames phase once the receiver knows `need` (and tells the phone).
    auto publishNeedLocked = [&]() {
      const auto phase = receiver.phase();
      if (framesPhase || (phase != studio_v4::Assembler::Phase::Frames &&
                          phase != studio_v4::Assembler::Phase::Complete))
        return;
      framesPhase = true;
      needValue = receiver.needHex();
      needCount = static_cast<uint16_t>(receiver.needCount());
      notify = true;
    };
    auto receiverFailureLocked = [&]() {
      const auto error = receiver.error();
      // A resume point that moved or a short heap keeps the partial for the retry.
      const bool keep = error == studio_v4::Error::OffsetMismatch || error == studio_v4::Error::DeviceBusy;
      fail(error == studio_v4::Error::None ? "storage_or_cipher_error" : studio_v4::errorName(error), !keep);
      notify = relax = true;
    };
    if (job == Job::Start) {
      const bool ok = receiver.start(startTask, startHash, startExpires, startSize, startHeader, startResume);
      std::lock_guard<std::recursive_mutex> lock(mutex);
      if (session != sessionId) {
        if (ok) requestAbortLocked(false);  // the phone left during the start
      } else if (!ok) {
        receiverFailureLocked();
      } else {
        rebuiltBytes = receiver.rebuiltBytes();
        publishNeedLocked();
      }
      continue;
    }
    if (job == Job::Chunk) {
      const bool ok = receiver.feed(chunk.bytes, chunk.length);
      std::lock_guard<std::recursive_mutex> lock(mutex);
      if (session != sessionId) continue;
      if (!ok) {
        receiverFailureLocked();
        continue;
      }
      rebuiltBytes = receiver.rebuiltBytes();
      publishNeedLocked();
      if (progressPending && queueCount <= QUEUE_SLOTS / 2) notify = true;
      continue;
    }
    const bool ok = receiver.commit();
    if (ok) StudioFrame::instance().tick(time(nullptr));
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (session != sessionId) continue;
    relax = true;
    notify = true;
    if (!ok) {
      receiverFailureLocked();
      continue;
    }
    state = "refreshing";
    resumeWaitSinceMs = 0;
    ++completedTransfers;
    changed();
    LOG_INF("BLE", "Transfer installed (%u bytes received, %u writes dropped, writer stack free %u)",
            (unsigned)queuedEnd, (unsigned)droppedWrites, (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  }
  if (notify) notifyProgress();
  if (relax) relaxLink();
}
void writerLoop(void*) {
  for (;;) {
    // Woken by every queued chunk or op; the timeout only bounds a missed wake-up.
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
    pump();
  }
}
// Main loop. Applies queued transfer work, starts the radio (retrying a
// failed start with backoff), keeps it advertising, and drops stalled
// sessions. Never blocks on the radio: while the worker is releasing or
// restoring the stack this tick is skipped.
void tick() {
  static uint32_t nextStartMs = 0, lastWatchMs = 0, retryDelayMs = 5000;
  static uint8_t advertisingFaults = 0;
  const uint32_t now = millis();
  {
    // The render task marks the frame displayed; report the outcome (and any
    // progress the writer could not notify) to the phone.
    bool notify = false, relax = false;
    {
      std::lock_guard<std::recursive_mutex> lock(mutex);
      if (isConnected && authenticated && state == "refreshing") {
        refreshOutcomeLocked();
        notify = relax = state != "refreshing";
      }
      notify = notify || (isConnected && progressPending && queueCount <= QUEUE_SLOTS / 2);
    }
    if (notify) notifyProgress();
    if (relax) relaxLink();
  }
  bool start = false, watch = false;
  uint16_t stalled = 0xffff;
  {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    start = userEnabled && !started && !released && static_cast<int32_t>(now - nextStartMs) >= 0;
    watch = started && !released && !isConnected && (!secret.empty() || !setupKey.empty()) &&
            now - lastWatchMs >= 3000;
    if (isConnected && now - lastActivity > 30000 && state == "receiving") {
      dropQueueLocked();
      requestAbortLocked(false);
      awaitResume();
      error = "transfer_timeout";
      state = "paused";
      stalled = connectionHandle;
      changed();
    } else if (isConnected && state != "receiving" && now - lastActivity > IDLE_LINK_TIMEOUT_MS) {
      stalled = connectionHandle;
    }
    if (resumeWaitSinceMs && now - resumeWaitSinceMs >= RESUME_WAIT_MS && !(isConnected && authenticated)) {
      resumeWaitSinceMs = 0;
      ++failedTransfers;  // the phone never came back: now it is a failure the user should see
      changed();
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
void setState(std::string json) {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  if (json.size() > ble_setup::STATUS_LIMIT) json = "{}";
  stateJson = std::move(json);
}
bool takeSyncRequest(SyncRequest& out) {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  if (!pendingSync) return false;
  out = std::move(*pendingSync);
  pendingSync.reset();
  return true;
}
bool takeUnbindRequest() {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  const bool requested = pendingUnbind;
  pendingUnbind = false;
  return requested;
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
void startWriter() {}
uint32_t writerStackFree() { return 0; }
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
void setState(std::string) {}
bool takeSyncRequest(SyncRequest&) { return false; }
bool takeUnbindRequest() { return false; }
void reportWifi(WifiState, const std::string&, const char*) {}
void reportScan(bool, std::vector<ble_setup::Network>) {}
}  // namespace studio_ble
#endif
