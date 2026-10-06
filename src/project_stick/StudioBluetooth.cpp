#include "StudioBluetooth.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <PersistableStore.h>

#include <algorithm>
#include <cstring>
#include <atomic>
#include <memory>
#include <mutex>

#include "FirmwareUpdateState.h"
#include "StudioFrame.h"
#ifndef SIMULATOR
#include <LinkTuning.h>
#include <StudioTransfer.h>

#include "StudioReceiver.h"
#include <FirmwareTransfer.h>
#include <HalPowerManager.h>
#include <StickFirmware.h>
#include <WiFi.h>
#include <optional>
#include <vector>

#include "FirmwareInstall.h"
#include "FirmwareReceiver.h"
#include "network/OtaTrial.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <esp_bt.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <mbedtls/aes.h>
#include <mbedtls/md.h>
#include <nvs_flash.h>

// NimBLE host stack high-water mark (nimble_port_freertos.c), 0 when the host
// task does not exist.
extern "C" UBaseType_t nimble_port_freertos_get_hs_hwm(void);
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
// Last content/firmware transfer as the device saw it (2.7.10, shown on
// Settings > Bluetooth): where it stopped, why, and how long the first
// PROGRESS took after begin4/fw4. `beginAtMs` is 0 outside a begin.
uint32_t beginAtMs = 0, firstProgressMs = 0, lastTransferAtMs = 0;
std::string lastTransferStage, lastTransferReason;
bool everConnected = false, everDisconnected = false;
// GAP-level link diagnostics (2.7.7). `connections` only counts links our
// onConnect accepted; a phone whose connect never completes (link-layer
// failure: NimBLE logs it and restarts advertising, no callback) or that we
// reject was invisible, so a 2.7.6 device read "connections 0" while the
// phone kept trying. A GAP listener sees every connect completion and
// disconnect, with the HCI status, reason and the heap at that moment.
std::atomic<uint32_t> linkAttempts{0}, linkFailures{0}, linkRejected{0};
uint32_t untrackedLinks = 0;  // links the watchdog adopted (under `mutex`)
std::atomic<int> lastLinkFailure{0}, lastDisconnectReason{-1};
std::atomic<uint32_t> lastLinkFailureHeap{0};
// Connection-parameter tuning (2.7.10): one conservative update per link,
// asked after the first PROGRESS of a transfer; the GAP listener records the
// outcome for Settings > Bluetooth. `connUpdateStatus` -1 = never asked.
link_tuning::Policy linkPolicy;  // under `mutex`
uint8_t peerAddress[6]{};       // under `mutex`
std::atomic<int> connUpdateStatus{-1};
std::atomic<uint32_t> connIntervalUnits{0};
std::atomic<bool> connUpdateBlocked{false};
#ifndef SIMULATOR
struct ble_gap_event_listener gapListener;
int onGapEvent(struct ble_gap_event* event, void*) {
  if (event->type == BLE_GAP_EVENT_CONNECT) {
    linkAttempts.fetch_add(1);
    const int status = event->connect.status;
    if (status != 0 && status != BLE_ERR_UNSUPP_REM_FEATURE) {
      linkFailures.fetch_add(1);
      lastLinkFailure.store(status);
      lastLinkFailureHeap.store(ESP.getFreeHeap());
    }
  } else if (event->type == BLE_GAP_EVENT_DISCONNECT) {
    lastDisconnectReason.store(event->disconnect.reason);
  } else if (event->type == BLE_GAP_EVENT_CONN_UPDATE) {
    struct ble_gap_conn_desc desc;
    const int rc = ble_gap_conn_find(event->conn_update.conn_handle, &desc);
    connUpdateStatus.store(event->conn_update.status);
    if (rc == 0) connIntervalUnits.store(desc.conn_itvl);
    LOG_INF("BLE", "Connection update: status=%d interval=%u latency=%u timeout=%u", event->conn_update.status,
            rc == 0 ? (unsigned)desc.conn_itvl : 0u, rc == 0 ? (unsigned)desc.conn_latency : 0u,
            rc == 0 ? (unsigned)desc.supervision_timeout : 0u);
  } else if (event->type == BLE_GAP_EVENT_PHY_UPDATE_COMPLETE) {
    LOG_INF("BLE", "PHY update: status=%d tx=%u rx=%u", event->phy_updated.status,
            (unsigned)event->phy_updated.tx_phy, (unsigned)event->phy_updated.rx_phy);
  } else if (event->type == BLE_GAP_EVENT_DATA_LEN_CHG) {
    LOG_INF("BLE", "Data length: tx=%u/%u us rx=%u/%u us", (unsigned)event->data_len_chg.max_tx_octets,
            (unsigned)event->data_len_chg.max_tx_time, (unsigned)event->data_len_chg.max_rx_octets,
            (unsigned)event->data_len_chg.max_rx_time);
  }
  return 0;
}
#endif
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
// Up to 16 slots of one MTU-sized write (~8 KB), matching the phone's 8 KB
// window (BLE-TRANSFER-V4 §7). The slots are allocated when a transfer is
// accepted (begin4) and freed when it ends, so the heap they take is only
// taken while a phone sends (2.7.2; 2.7.0 kept them static, which with the
// writer stack and the inflater cost 15 KB of the ~13 KB a 2.6.5 device had
// free with Wi-Fi and BLE up). By then ProjectStickHost has switched Wi-Fi off
// for the linked phone, so ~50 KB are free; fewer slots are accepted when the
// heap is short. DATA arrives as Write Without Response, so the host task
// never waits for a slot: a write that finds the queue full is dropped and the
// phone resends from the last PROGRESS `received`. PROGRESS notifications are
// held back while the queue is over half full, which is what paces the phone.
constexpr size_t QUEUE_SLOTS = 16, MIN_QUEUE_SLOTS = 4;
std::unique_ptr<Chunk> chunkQueue[QUEUE_SLOTS];  // under `mutex`
size_t queueSlots = 0;                           // allocated slots, 0 outside a transfer
size_t queueHead = 0, queueCount = 0;            // under `mutex`
size_t queuedEnd = 0, announcedOffset = 0;  // bytes accepted from the phone; offset promised at `begin4`
uint32_t sessionId = 0;                     // bumps on every resetSession; pump() drops stale results
enum class PendingOp : uint8_t { None, Start, Commit, FirmwareStart };
PendingOp pendingOp = PendingOp::None;
std::string pendingTask, pendingHash;
int64_t pendingExpires = 0;
size_t pendingSize = 0, pendingHeader = 0;
StudioReceiver::Resume pendingResume;
bool pendingAbort = false, pendingDiscard = false;
// A `commit` the phone sent before its link dropped (2.7.10). Every chunk had
// reached the queue (ATT keeps writes in order), so the writer still drains it
// and installs under the old session instead of aborting: the field report had
// a complete incoming.bin and an unchanged state.json. The phone's next begin4
// for the same task waits for that install (state 3, then 4/5).
bool detachedCommit = false;
std::string detachedTask, detachedHash;
// Firmware over BLE (op fw4, 2.7.4): the session streams an image instead of
// content. FirmwareReceiver writes it to firmware.tmp; once complete the
// pump installs it like an SD-card update and restarts.
bool firmwareTransfer = false;
std::string firmwareVersion, firmwareSha;
size_t firmwareSize = 0;
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
// The task that runs pump() (the background sync worker, idle while a phone
// is connected); woken for every queued chunk or op.
TaskHandle_t pumpTask = nullptr;
void wakePump() {
  if (pumpTask) xTaskNotifyGive(pumpTask);
}
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
// Forced release for a cloud job (2.7.3): how long `net_busy` is readable
// before the phone is dropped, and how long to wait for the disconnect.
constexpr uint32_t FORCED_RELEASE_NOTICE_MS = 400;
constexpr uint32_t FORCED_DISCONNECT_WAIT_MS = 1500;
// Setup mode (unbound): one-time key K shown in the QR. Each connection pins
// its key: a setup session keeps K until disconnect even after a bind.
std::string setupKey, sessionKey;
bool sessionSetup = false;
enum class BindState { None, Pending, Done, Failed };
BindState bindState = BindState::None;
std::optional<Binding> pendingBinding;
std::optional<WifiRequest> pendingWifi;
std::optional<SyncRequest> pendingSync;
std::optional<TokenRequest> pendingToken;
bool pendingUnbind = false;
// STATE characteristic value, built on the UI loop (setState) and copied by onRead.
std::string stateJson = "{}";
bool scanRequested = false;
WifiState wifiState = WifiState::Idle;
std::string wifiSsid, wifiError, scanState = "idle";
bool wifiSaved = false;  // at least one saved network (STATUS wifi.saved)
// STATUS `net_busy` (2.7.3): the cloud job that is about to disconnect the
// phone and release the stack ("firmware_check", "ota", "sync", ...); empty
// otherwise. Set just before a forced release so a phone polling STATUS sees
// why the link drops.
std::string netBusy;
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
// Caller holds `mutex`. Forgets queued chunks and ops and returns the slots'
// heap: after a drop the transfer is over or paused, and a resume starts
// with a new begin4 that allocates again.
void dropQueueLocked() {
  queueCount = 0;
  queueHead = 0;
  pendingOp = PendingOp::None;
  for (auto& slot : chunkQueue) slot.reset();
  queueSlots = 0;
}
// Caller holds `mutex`. Allocates the chunk slots for a transfer, as many as
// the heap allows up to QUEUE_SLOTS; false below MIN_QUEUE_SLOTS.
bool allocateQueueLocked() {
  for (auto& slot : chunkQueue) slot.reset();
  queueSlots = 0;
  for (auto& slot : chunkQueue) {
    slot.reset(new (std::nothrow) Chunk);
    if (!slot) break;
    ++queueSlots;
  }
  if (queueSlots >= MIN_QUEUE_SLOTS) return true;
  for (auto& slot : chunkQueue) slot.reset();
  queueSlots = 0;
  return false;
}
// Caller holds `mutex`. pump() closes (or discards) the partial transfer.
void requestAbortLocked(const bool discard) {
  pendingAbort = true;
  pendingDiscard = pendingDiscard || discard;
  // The pump may sit in its housekeeping wait for minutes; until it runs the
  // abort the next begin4 would find the old transfer still open (2.7.9:
  // the reconnecting phone got device_busy).
  wakePump();
}
void resetSession() {
  if (authenticated && !firmwareTransfer && pendingOp == PendingOp::Commit) {
    detachedCommit = true;  // the queue and the commit stay with the writer
    detachedTask = task;
    detachedHash = hash;
    wakePump();
  } else {
    if (authenticated && state == "receiving") awaitResume();  // the phone left mid-transfer; it reconnects and resumes
    if (authenticated) requestAbortLocked(false);              // keep the partial for a resume
    dropQueueLocked();
  }
  queuedEnd = announcedOffset = lastNotified = 0;
  packetsSinceNotify = 0;
  progressPending = framesPhase = gapReported = outcomeRead = false;
  outcomeAtMs = 0;
  needValue.clear();
  needCount = 0;
  rebuiltBytes = 0;
  transferHeader = 0;
  firmwareTransfer = false;
  firmwareVersion.clear();
  firmwareSha.clear();
  firmwareSize = 0;
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
// Caller holds `mutex`. The phase a running transfer is in, for the record.
const char* transferStageLocked() {
  if (firmwareTransfer) return "firmware";
  if (pendingOp == PendingOp::Commit || state == "refreshing") return "commit";
  return framesPhase ? "frames" : "header";
}
// Caller holds `mutex`.
void noteTransferLocked(const char* stage, const std::string& reason) {
  lastTransferStage = stage;
  lastTransferReason = reason;
  lastTransferAtMs = millis() ? millis() : 1;
}
void fail(const char* reason, const bool discard = true) {
  // The first cause is the one the phone must see; later chunks arriving
  // after a failure would otherwise overwrite it (the 2.6.0 reports all read
  // "unauthorized_or_invalid_chunk" whatever had actually gone wrong).
  if (state == "failed" && !error.empty()) return;
  if (authenticated) noteTransferLocked(transferStageLocked(), reason);
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
  wifi["saved"] = wifiSaved;
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
  if (update.phase == firmware_update::Phase::Idle) return;
  JsonObject ota = doc["ota"].to<JsonObject>();
  ota["state"] = otaStateName(update.phase);
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
  // A firmware transfer ends in a restart, not a displayed frame.
  if (authenticated && !firmwareTransfer && snapshot.task == task && state != "receiving" && state != "failed") {
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
  // A firmware transfer reports state 1 for its whole stream (no `need`).
  // `commit` accepted: state 3 at once, while the writer drains and installs.
  if (state == "receiving" && pendingOp == PendingOp::Commit) return studio_v4::State::Committing;
  if (state == "receiving")
    return framesPhase && !firmwareTransfer ? studio_v4::State::ReceivingFrames : studio_v4::State::ReceivingHeader;
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
  bool tune = false;
  {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (!isConnected || !progressChar || released) return;
    buildProgressLocked(value);
    if (beginAtMs) {
      firstProgressMs = millis() - beginAtMs;
      beginAtMs = 0;
      LOG_INF("BLE", "First PROGRESS %u ms after begin (state=%s)", (unsigned)firstProgressMs, state.c_str());
      // One conservative connection update per link, only once the phone has
      // its answer and only for a transfer that is running (BLE-TRANSFER-V4 §3).
      if (state == "receiving" && linkPolicy.shouldRequest(peerAddress)) {
        linkPolicy.requested(millis());
        tune = true;
      }
    }
    characteristic = progressChar;
    handle = connectionHandle;
  }
  characteristic->notify(value, sizeof(value), handle);
  if (tune) {
    auto* server = NimBLEDevice::getServer();
    if (server) {
      server->updateConnParams(handle, link_tuning::MIN_INTERVAL, link_tuning::MAX_INTERVAL, link_tuning::LATENCY,
                               link_tuning::SUPERVISION_TIMEOUT);
      LOG_INF("BLE", "Asked for a 15-30 ms interval (6 s timeout)");
    }
  }
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
// Link layer (BLE-TRANSFER-V4 §3, 2.7.10): only the data length is asked for,
// once per connection. 2.7.0-2.7.9 also requested a 2M-only PHY and a
// 7.5-15 ms interval with a 4 s supervision timeout on connect and again on
// begin4; on the field X3 (Android 16) the link died 4.2 s after every begin4,
// i.e. one supervision timeout after those procedures, before the first
// PROGRESS. PHY and interval are left to the phone; throughput comes from
// DLE plus write-without-response under the PROGRESS window.
// Host-callback context.
void requestDataLength(uint16_t handle) {
  auto* server = NimBLEDevice::getServer();
  if (!server || handle == 0xffff) return;
  server->setDataLen(handle, 251);
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
  if (!netBusy.empty()) doc["net_busy"] = netBusy;
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
  if (op == "token") {
    // A bound device that lost its cloud token (2.6.x store write under a
    // starved heap) gets a fresh one from the owner's phone; the BLE
    // authority stays as it is.
    if (sessionSetup) return reject("invalid_control");
    const std::string owner = doc["owner"] | "", ct = doc["ct"] | "";
    if (!equalProof(proof, mac(ble_setup::tokenMessage(nonce, owner, ct)))) return reject("authorization_failed");
    std::string token;
    // The token belongs to the owner this authority was issued for; the
    // cloud only hands a re-claim token to that owner.
    const bool ownerMatches = authorityOwner.empty() || owner == authorityOwner;
    if (!validOwner(owner) || !ownerMatches || pendingToken ||
        !ble_setup::unseal(sessionKey, "token3", nonce, ct, token) || !ble_setup::validDeviceToken(token))
      return reject("invalid_control");
    error.clear();
    pendingToken = TokenRequest{owner, token};
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
// Caller holds `mutex`. Positions the session's AES-256-CTR stream (key
// mac(K, "enc|N"), counter N) at stream offset `skip`, for a fresh transfer
// (0) or a resume.
bool seekCipherLocked(const size_t skip) {
  if (!unhex(nonce, counter, sizeof(counter))) return false;
  counterOffset = 0;
  memset(stream, 0, sizeof(stream));
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
  return true;
}
class ServerCallbacks final : public NimBLEServerCallbacks {
  // Host-task context: state under `mutex`, NimBLE calls after releasing it.
  void onConnect(NimBLEServer* server, NimBLEConnInfo& info) override {
    bool reject = false;
    {
      std::lock_guard<std::recursive_mutex> lock(mutex);
      reject = isConnected;
      if (reject) linkRejected.fetch_add(1);
      if (!reject) {
        isConnected = true;
        connectionHandle = info.getConnHandle();
        ++connections;
        lastConnectMs = millis();
        memcpy(peerAddress, info.getIdAddress().getVal(), sizeof(peerAddress));
        linkPolicy.linkUp();
        everConnected = true;
        resetSession();
      }
    }
    if (reject) {
      server->disconnect(info.getConnHandle());
      return;
    }
    requestDataLength(info.getConnHandle());
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo& info, int) override {
    bool advertise = false;
    {
      std::lock_guard<std::recursive_mutex> lock(mutex);
      if (connectionHandle != info.getConnHandle()) return;
      if (authenticated && state == "receiving") noteTransferLocked(transferStageLocked(), "link_lost");
      if (beginAtMs) LOG_ERR("BLE", "Link lost %u ms after begin, no PROGRESS sent", (unsigned)(millis() - beginAtMs));
      beginAtMs = 0;
      if (linkPolicy.linkDown(peerAddress, millis())) {
        connUpdateBlocked.store(true);
        LOG_ERR("BLE", "Link lost within %u ms of the connection update: not asking this phone again",
                (unsigned)link_tuning::DROP_WINDOW_MS);
      }
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
        } else if (queueCount >= queueSlots) {
          ++droppedWrites;
          progressPending = true;
        } else {
          Chunk& slot = *chunkQueue[(queueHead + queueCount) % queueSlots];
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
              if (queueCount <= queueSlots / 2)
                notify = true;
              else
                progressPending = true;  // reported once the writer drains the queue
            }
          }
        }
      }
    }
    wakePump();
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
    // A commit left by the dropped link is still installing this very task:
    // answer like a completed one (state 3, then 4/5 once it is shown).
    const bool installing = detachedCommit && detachedTask == incomingTask && detachedHash == incomingHash;
    const bool completed = installing || (active.task == incomingTask && active.hash == incomingHash);
    StudioReceiver::Resume resume;
    if (!completed) {
      // A pending abort of the previous session is not "busy": pump() applies
      // it before this start (abort, then start, in order).
      if (studio_v4::beginRefusedAsBusy(StudioFrame::instance().busy(), pendingAbort, pendingOp != PendingOp::None,
                                        incomingTask.size())) {
        fail("device_busy");
        return true;
      }
      // The RAM copy of the partial record: no SD access in the host task.
      resume = StudioReceiver::resumeFor(header, size,
                                         StudioFrame::instance().resumeOffset(incomingTask, incomingHash, size));
    }
    uint8_t encryptionKey[32];
    if (!unhex(mac("enc|" + nonce), encryptionKey, sizeof(encryptionKey))) {
      fail("authorization_failed");
      return true;
    }
    if (!completed && !allocateQueueLocked()) {
      LOG_ERR("BLE", "No heap for the transfer queue (heap=%u max=%u)", (unsigned)ESP.getFreeHeap(),
              (unsigned)ESP.getMaxAllocHeap());
      fail("insufficient_memory", false);
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
    state = completed ? (active.displayed && !installing ? "displayed" : "refreshing") : "receiving";
    if (state == "displayed") {
      outcomeAtMs = millis() ? millis() : 1;
      outcomeRead = false;
    }
    changed();
    seekCipherLocked(queuedEnd);  // the resume point
    // Announce at once (state 1 with the resume point): the phone has its
    // answer within milliseconds whatever the writer still has to do. A
    // resume past the header or a single frame then waits for state 2 and
    // `need`, which the writer reports once it has resolved them.
    (void)single;
    return true;
  }
  // fw4 (firmware over BLE, 2.7.4). Caller holds `mutex`. Checks the proof and
  // the request; the pump task (which may touch the card and the fuel gauge)
  // applies the remaining guards, allocates the queue once Wi-Fi is off,
  // opens or resumes firmware.tmp and then announces state 1 with the resume
  // offset. Returns whether to notify now (a refusal).
  bool beginFirmwareLocked(JsonDocument& doc) {
    const std::string version = doc["version"] | "", sha = doc["sha256"] | "", proof = doc["proof"] | "";
    const size_t size = doc["size"] | size_t(0);
    const int64_t phoneTime = doc["time"] | int64_t(0);
    if (!equalProof(proof, mac(firmware_v4::beginMessage(identity, nonce, epoch, version, sha, size, phoneTime)))) {
      fail("authorization_failed");
      return true;
    }
    studio_v4::Digest digest{};
    if (version.empty() || version.size() > 32 || stick_fw::compareVersions(version.c_str(), CROSSPOINT_VERSION) <= 0 ||
        size < firmware_v4::MIN_IMAGE_BYTES || size > firmware_v4::MAX_IMAGE_BYTES ||
        !studio_v4::parseDigest(sha.c_str(), digest)) {
      fail("invalid_target");
      return true;
    }
    if (StudioFrame::instance().busy() || pendingOp != PendingOp::None || pendingAbort ||
        firmware_update::snapshot().busy()) {
      fail("device_busy");
      return true;
    }
    uint8_t encryptionKey[32];
    if (!unhex(mac("enc|" + nonce), encryptionKey, sizeof(encryptionKey))) {
      fail("authorization_failed");
      return true;
    }
    if (phoneTime >= 1735689600 && phoneTime < 4102444800 && time(nullptr) < 1735689600) {
      timeval tv{static_cast<time_t>(phoneTime), 0};
      settimeofday(&tv, nullptr);
    }
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, encryptionKey, 256);
    authenticated = true;
    firmwareTransfer = true;
    firmwareVersion = version;
    firmwareSha = sha;
    firmwareSize = size;
    transferTotal = size;
    transferHeader = 0;
    framesPhase = false;
    needValue.clear();
    needCount = 0;
    packetsSinceNotify = 0;
    announcedOffset = queuedEnd = lastNotified = 0;
    state = "receiving";
    resumeWaitSinceMs = 0;
    pendingOp = PendingOp::FirmwareStart;
    changed();
    return false;
  }
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& info) override {
    if (characteristic->getUUID() == NimBLEUUID(DATA)) {
      onData(characteristic, info);
      return;
    }
    bool notify = false;
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
        beginAtMs = millis();
        notify = beginTransferLocked(doc);
        LOG_INF("BLE", "begin4 %s in %u ms (state=%s, offset=%u)", notify ? "answered" : "queued",
                (unsigned)(millis() - beginAtMs), state.c_str(), (unsigned)queuedEnd);
      } else if (op == "fw4" && !authenticated) {
        beginAtMs = millis();
        notify = beginFirmwareLocked(doc);
      } else if (op == "commit" && authenticated && !firmwareTransfer && state == "receiving" &&
                 pendingOp == PendingOp::None) {
        // The writer task checks, once every queued chunk is applied, that the
        // stream delivered every needed frame; STATUS stays "receiving" until then.
        pendingOp = PendingOp::Commit;
        notify = true;  // state 3: the phone knows the commit arrived
      } else {
        fail("unexpected_control");
        notify = true;
      }
      handle = connectionHandle;
    }
    wakePump();
    if (notify) notifyProgressUnguarded();
  }
};
ServerCallbacks serverCallbacks;
Callbacks callbacks;

// Caller holds `mutex`. Atomic (tmp, verify, rotate to .bak) like the device
// store: 2.6.5 truncated the file first and streamed into it, so a write that
// died halfway (power, SD) left the device without its BLE authority.
bool writeCredentials() {
  JsonDocument doc;
  doc["owner_id"] = authorityOwner;
  doc["device_id"] = identity;
  doc["secret"] = secret;
  doc["epoch"] = epoch;
  const bool ok = PersistableStoreBase::writeDocToFile(CREDENTIALS, doc);
  if (!ok) LOG_ERR("BLE", "BLE credential not saved");
  return ok;
}
// Caller holds `mutex`. Loads the stored authority (or its backup) when none
// is in memory. Returns true when a valid authority is held afterwards.
bool loadCredentialsLocked() {
  if (!secret.empty()) return true;
  JsonDocument doc;
  if (PersistableStoreBase::readDocFromFile(CREDENTIALS, doc)) {
    authorityOwner = doc["owner_id"] | "";
    identity = doc["device_id"] | "";
    secret = doc["secret"] | "";
    epoch = doc["epoch"] | 0;
  }
  uint8_t decoded[32];
  return identity.size() == 36 && unhex(secret, decoded, 32);
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
  // code is an esp_err_t for the nvs/controller stages (named here), the free
  // KB for low_memory.
  LOG_ERR("BLE", "Start failed at %s (code=%d 0x%x %s heap=%u max=%u)", stage, code, static_cast<unsigned>(code),
          code > 0 && strcmp(stage, "low_memory") != 0 ? esp_err_to_name(static_cast<esp_err_t>(code)) : "-",
          (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
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
// NimBLEDevice::init() only returns false. Its first steps are
// nvs_flash_init(), esp_bt_controller_init() and esp_bt_controller_enable()
// (NimBLE-Arduino NimBLEDevice.cpp); repeat them to record which one failed
// and its esp_err_t (2.7.5 logged only "init (code=0)", the controller
// status). Anything this probe brings up is torn down again, so the next
// start begins from the same state.
struct InitFailure {
  const char* stage;
  int code;
};
InitFailure diagnoseInitFailure() {
  const esp_err_t nvs = nvs_flash_init();
  if (nvs != ESP_OK) return {"nvs_flash_init", static_cast<int>(nvs)};
  switch (esp_bt_controller_get_status()) {
    case ESP_BT_CONTROLLER_STATUS_IDLE: {
      esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
      const esp_err_t init = esp_bt_controller_init(&cfg);
      if (init != ESP_OK) return {"controller_init", static_cast<int>(init)};
      const esp_err_t enable = esp_bt_controller_enable(ESP_BT_MODE_BLE);
      if (enable == ESP_OK) esp_bt_controller_disable();
      esp_bt_controller_deinit();
      if (enable != ESP_OK) return {"controller_enable", static_cast<int>(enable)};
      return {"host_init", 0};  // the controller comes up on its own: NimBLE's host side failed
    }
    case ESP_BT_CONTROLLER_STATUS_INITED: {
      const esp_err_t enable = esp_bt_controller_enable(ESP_BT_MODE_BLE);
      if (enable == ESP_OK) esp_bt_controller_disable();
      return {"controller_enable", static_cast<int>(enable)};
    }
    case ESP_BT_CONTROLLER_STATUS_ENABLED:
      return {"host_init", 0};
    default:
      return {"controller_status", static_cast<int>(esp_bt_controller_get_status())};
  }
}
// Starts NimBLE for the current identity. Caller holds radioMutex. Returns
// false when the start failed (recorded for diagnostics; tick() retries).
bool startRadioLocked() {
  std::string name;
  {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (started || released || !userEnabled) return true;
    loadCredentialsLocked();
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
    const InitFailure why = diagnoseInitFailure();
    failStartLocked(why.stage, why.code);
    return false;
  }
  // ble_gap_init() clears the listener list on every start: register again.
  ble_gap_event_listener_register(&gapListener, onGapEvent, nullptr);
  NimBLEDevice::setMTU(ATT_MTU);
  // Allow 1M and 2M by default; the phone decides the PHY (BLE-TRANSFER-V4
  // §3). 2.7.x preferred 2M only, which leaves no PHY a 1M-only peer can
  // agree to; keeping 1M in the mask is the IDF default and costs nothing.
  NimBLEDevice::setDefaultPhy(BLE_GAP_LE_PHY_1M_MASK | BLE_GAP_LE_PHY_2M_MASK,
                              BLE_GAP_LE_PHY_1M_MASK | BLE_GAP_LE_PHY_2M_MASK);
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
bool releaseRadio(const char* forJob) {
  std::lock_guard<std::mutex> radio(radioMutex);
  uint16_t handle = 0xffff;
  {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (!started || released) return false;
    if (isConnected) {
      if (!forJob) return false;
      // A cloud job wins over the phone (2.7.3): say why, then drop it.
      netBusy = forJob;
      handle = connectionHandle;
      changed();
    }
    released = true;  // from here tick() neither restarts nor advertises
  }
  if (handle != 0xffff) {
    LOG_INF("BLE", "Disconnecting the phone for %s", forJob);
    delay(FORCED_RELEASE_NOTICE_MS);  // a phone polling STATUS reads net_busy
    disconnectLocked(handle);
    for (uint32_t waited = 0; waited < FORCED_DISCONNECT_WAIT_MS && connected(); waited += 50) delay(50);
  }
  {
    std::lock_guard<std::recursive_mutex> lock(mutex);
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
  LOG_INF("BLE", "NimBLE released for %s (heap %u -> %u, max=%u)", forJob ? forJob : "network/flash",
          (unsigned)before, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  return true;
}
void restoreRadio() {
  std::lock_guard<std::mutex> radio(radioMutex);
  {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (!released) return;
    released = false;
    netBusy.clear();
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
    // The atomic writer keeps a .bak; a revoked authority must not come back
    // from it at the next boot (ProjectStickService::loadStore heals a store
    // from this file).
    Storage.remove(CREDENTIALS);
    Storage.remove((std::string(CREDENTIALS) + ".bak").c_str());
    Storage.remove((std::string(CREDENTIALS) + ".tmp").c_str());
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
void setPumpTask(TaskHandle_t task) {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  pumpTask = task;
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
  result.linkAttempts = linkAttempts.load();
  result.linkFailures = linkFailures.load();
  result.linkRejected = linkRejected.load();
  result.lastLinkFailure = lastLinkFailure.load();
  result.lastLinkFailureHeap = lastLinkFailureHeap.load();
  result.lastDisconnectReason = lastDisconnectReason.load();
  // Stack headroom of the NimBLE host task (its overflow was the 2.4.3 crash).
  if (started) {
    result.hostStackFree = nimble_port_freertos_get_hs_hwm();  // by handle; never xTaskGetHandle()
  }
  const uint32_t now = millis();
  if (everConnected) result.sinceConnect = static_cast<int32_t>((now - lastConnectMs) / 1000);
  if (everDisconnected) result.sinceDisconnect = static_cast<int32_t>((now - lastDisconnectMs) / 1000);
  if (lastTransferAtMs) {
    result.transferStage = lastTransferStage;
    result.transferReason = lastTransferReason;
    result.sinceTransfer = static_cast<int32_t>((now - lastTransferAtMs) / 1000);
  }
  result.firstProgressMs = firstProgressMs;
  result.connUpdateStatus = connUpdateStatus.load();
  result.connIntervalUnits = connIntervalUnits.load();
  result.connUpdateBlocked = connUpdateBlocked.load();
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
// Runs on the background sync worker (woken by the callbacks): applies the
// transfer work they queued, in order (abort, start, chunks, commit), through
// the protocol 4 receiver. The SD card is never touched with `mutex` held, so
// the callbacks keep answering the phone meanwhile.
// While it applies transfer work the worker runs above the UI loop and the
// render task (priority 1), as 2.7.0's writer task did, so an e-paper refresh
// does not stall the chunk queue; cloud jobs keep the normal priority.
class PumpPriority {
 public:
  void raise() {
    if (raised) return;
    previous = uxTaskPriorityGet(nullptr);
    if (previous < 2) vTaskPrioritySet(nullptr, 2);
    raised = true;
  }
  ~PumpPriority() {
    if (raised && previous < 2) vTaskPrioritySet(nullptr, previous);
  }

 private:
  UBaseType_t previous = 0;
  bool raised = false;
};
// fw4 guards that need the card or the fuel gauge (pump task). Returns the
// STATUS error name, or nullptr when the image may be received.
const char* firmwareRefusal(const size_t size) {
  if (ota_trial::active()) return "trial_active";
  if (powerManager.getBatteryPercentage() < 30 && !firmware_update::externalPower()) return "low_battery";
  uint64_t total = 0, used = 0;
  if (StudioFrame::storageUsage(total, used) && (total <= used || total - used < uint64_t(size) + 65536))
    return "insufficient_storage";
  return nullptr;
}
// A complete, verified image is on the card (pump task, no locks held): the
// phone has been told (PROGRESS state 3); STATUS `ota` shows verifying, then
// the radio is released and the image installed by the same routine as the
// SD-card update (identify, flash, trial boot). A success restarts.
void installReceivedFirmware(const std::string& version, const size_t size) {
  HalPowerManager::Lock powerLock;
  firmware_update::begin(version.c_str(), static_cast<uint32_t>(size));
  firmware_update::setPhase(firmware_update::Phase::Verifying);
  delay(800);  // a phone polling STATUS sees `verifying` before the link drops
  const bool lent = releaseRadio("firmware_install");
  firmware_update::setPhase(firmware_update::Phase::Installing);
  auto onProgress = +[](size_t written, size_t, void*) { firmware_update::setProgress(written); };
  const auto result = firmware_install::installFromSd(FirmwareReceiver::path(), version.c_str(), false, onProgress,
                                                      nullptr);
  if (!result.ok) {
    LOG_ERR("FWBLE", "Install of %s failed: %s", version.c_str(), result.error);
    if (!result.flashFailed) {  // the image itself is wrong: no resume from it
      Storage.remove(FirmwareReceiver::path());
      Storage.remove("/.crosspoint/studio/firmware.meta");
    }
    firmware_update::fail(result.error);
    if (lent) restoreRadio();
    return;
  }
  Storage.remove(FirmwareReceiver::path());
  Storage.remove("/.crosspoint/studio/firmware.meta");
  firmware_update::setPhase(firmware_update::Phase::Restarting);
  LOG_INF("FWBLE", "Firmware %s installed over BLE, restarting", version.c_str());
  delay(1500);
  ESP.restart();
}
void pump() {
  static Chunk chunk;  // off the worker's stack
  auto& receiver = StudioReceiver::instance();
  auto& firmware = FirmwareReceiver::instance();
  bool notify = false, install = false;
  std::string installVersion;
  size_t installSize = 0;
  PumpPriority priority;
  for (size_t guard = 0; guard < QUEUE_SLOTS + 2 && !install; ++guard) {
    enum class Job : uint8_t { None, Abort, Start, FirmwareStart, Chunk, Commit } job = Job::None;
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
      } else if (pendingOp == PendingOp::FirmwareStart) {
        job = Job::FirmwareStart;
        startHash = firmwareSha;
        startSize = firmwareSize;
        pendingOp = PendingOp::None;
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
        chunk = *chunkQueue[queueHead];
        queueHead = (queueHead + 1) % queueSlots;
        --queueCount;
      } else if (pendingOp == PendingOp::Commit) {
        job = Job::Commit;
        pendingOp = PendingOp::None;
      }
    }
    if (job == Job::None) break;
    priority.raise();
    if (job == Job::Abort) {
      receiver.abort(discard);
      firmware.abort();  // a firmware partial always stays for a resume
      continue;
    }
    if (job == Job::FirmwareStart) {
      const char* refusal = firmwareRefusal(startSize);
      // A linked phone keeps Wi-Fi off (ProjectStickHost); the queue is only
      // allocated once its driver's heap is back.
      for (uint32_t waited = 0; !refusal && WiFi.getMode() != WIFI_MODE_NULL; waited += 100) {
        if (waited >= 10000) refusal = "device_busy";
        delay(100);
      }
      {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        if (session != sessionId) continue;
        if (!refusal && !allocateQueueLocked()) {
          LOG_ERR("BLE", "No heap for the firmware queue (heap=%u max=%u)", (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getMaxAllocHeap());
          refusal = "insufficient_memory";
        }
        if (refusal) {
          fail(refusal, false);
          notify = true;
          continue;
        }
      }
      const bool ok = firmware.start(startHash, startSize);
      std::lock_guard<std::recursive_mutex> lock(mutex);
      if (session != sessionId) {
        firmware.abort();
        continue;
      }
      if (!ok) {
        fail(studio_v4::errorName(firmware.error()), false);
        notify = true;
        continue;
      }
      announcedOffset = queuedEnd = lastNotified = firmware.streamOffset();
      seekCipherLocked(queuedEnd);
      rebuiltBytes = firmware.rawDone();
      notify = true;  // state 1 with the resume offset: the phone starts sending
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
      notify = true;
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
    if (job == Job::Chunk && firmwareTransfer) {
      const bool ok = firmware.feed(chunk.bytes, chunk.length);
      std::lock_guard<std::recursive_mutex> lock(mutex);
      if (session != sessionId) continue;
      if (!ok) {
        const auto error = firmware.error();
        fail(error == studio_v4::Error::None ? "storage_or_cipher_error" : studio_v4::errorName(error), false);
        notify = true;
        continue;
      }
      rebuiltBytes = firmware.rawDone();
      if (firmware.complete()) {
        // PROGRESS state 3 goes out before the radio is released to install.
        state = "refreshing";
        dropQueueLocked();
        install = notify = true;
        installVersion = firmwareVersion;
        installSize = firmwareSize;
        changed();
        continue;
      }
      if (progressPending && queueCount <= queueSlots / 2) notify = true;
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
      if (progressPending && queueCount <= queueSlots / 2) notify = true;
      continue;
    }
    const bool ok = receiver.commit();
    if (ok) StudioFrame::instance().tick(time(nullptr));
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (detachedCommit) {
      if (!ok) receiver.abort(false);  // the partial stays for the phone's retry
      detachedCommit = false;
      dropQueueLocked();  // the old session's slots; no begin4 allocated new ones meanwhile
      noteTransferLocked("commit", ok ? "ok_after_link_loss" : "failed_after_link_loss");
      LOG_INF("BLE", "Commit after the link loss %s", ok ? "installed" : "failed");
      if (ok) ++completedTransfers;
      // A phone that reconnected for this task waits in state 3.
      if (!ok && authenticated && task == detachedTask && state == "refreshing") {
        fail("storage_or_cipher_error", false);
        notify = true;
      }
      continue;
    }
    if (session != sessionId) continue;
    notify = true;
    if (!ok) {
      receiverFailureLocked();
      continue;
    }
    state = "refreshing";
    resumeWaitSinceMs = 0;
    ++completedTransfers;
    noteTransferLocked("commit", "ok");
    dropQueueLocked();  // the slots' heap goes back until the next begin4
    changed();
    LOG_INF("BLE", "Transfer installed (%u bytes received, %u writes dropped, heap=%u, worker stack free %u)",
            (unsigned)queuedEnd, (unsigned)droppedWrites, (unsigned)ESP.getFreeHeap(),
            (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  }
  if (notify) notifyProgress();
  if (install) installReceivedFirmware(installVersion, installSize);
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
    bool notify = false;
    {
      std::lock_guard<std::recursive_mutex> lock(mutex);
      if (isConnected && authenticated && state == "refreshing") {
        refreshOutcomeLocked();
        notify = state != "refreshing";
      }
      notify = notify || (isConnected && progressPending && queueCount <= queueSlots / 2);
    }
    if (notify) notifyProgress();
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
    // A link the stack holds but our onConnect never adopted (2.7.7): the
    // phone sees a connection whose reads and writes are all ignored, and the
    // advertising check below would tear the stack down under it. Adopt it.
    auto* server = NimBLEDevice::getServer();
    const std::vector<uint16_t> peers = server ? server->getPeerDevices() : std::vector<uint16_t>{};
    if (!peers.empty()) {
      {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        if (!isConnected) {
          isConnected = true;
          connectionHandle = peers.front();
          ++connections;
          ++untrackedLinks;
          lastConnectMs = now;
          everConnected = true;
          resetSession();
          changed();
        }
      }
      LOG_ERR("BLE", "Adopted a link onConnect missed (handle %u)", static_cast<unsigned>(peers.front()));
      advertisingFaults = 0;
    } else if (NimBLEDevice::getAdvertising()->isAdvertising()) {
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
bool finishBinding(bool ok, const Binding& binding) {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  if (!ok) {
    bindState = BindState::Failed;
    error = "bind_failed";
    return false;
  }
  // The live setup session keeps K (sessionKey) until the phone disconnects;
  // the next connection resets into bound mode with this secret.
  authorityOwner = binding.owner;
  secret = binding.secret;
  epoch = binding.epoch;
  if (!writeCredentials()) {
    // All or nothing with the device store: without the file the device would
    // be bound in the cloud and unreachable over BLE after a reboot.
    authorityOwner.clear();
    secret.clear();
    epoch = 0;
    bindState = BindState::Failed;
    error = "bind_failed";
    return false;
  }
  setupKey.clear();
  bindState = BindState::Done;
  return true;
}
bool storedAuthority(std::string& deviceId, std::string& owner) {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  if (!loadCredentialsLocked()) return false;
  deviceId = identity;
  owner = authorityOwner;
  return true;
}
bool takeTokenRequest(TokenRequest& out) {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  if (!pendingToken) return false;
  out = std::move(*pendingToken);
  pendingToken.reset();
  return true;
}
void finishToken(bool ok) {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  if (!ok) error = "bind_failed";
  changed();
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
void setWifiSaved(bool saved) {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  wifiSaved = saved;
}
bool transferActive() {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  return isConnected && authenticated && (state == "receiving" || state == "refreshing");
}
}  // namespace studio_ble
#else
#include <Arduino.h>

#include <cstdlib>
#include <random>

namespace studio_ble {
void revoke() {}
bool adoptAuthority(const std::string&, const std::string&, uint32_t, const std::string&) { return false; }
bool releaseRadio(const char*) { return false; }
void restoreRadio() {}
void begin() {}
void setPumpTask(TaskHandle_t) {}
void pump() {}
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
bool finishBinding(bool ok, const Binding&) { return ok; }
bool storedAuthority(std::string&, std::string&) { return false; }
bool takeTokenRequest(TokenRequest&) { return false; }
void finishToken(bool) {}
void setWifiSaved(bool) {}
bool transferActive() { return false; }
bool takeWifiRequest(WifiRequest&) { return false; }
bool takeScanRequest() { return false; }
void setState(std::string) {}
bool takeSyncRequest(SyncRequest&) { return false; }
bool takeUnbindRequest() { return false; }
void reportWifi(WifiState, const std::string&, const char*) {}
void reportScan(bool, std::vector<ble_setup::Network>) {}
}  // namespace studio_ble
#endif
