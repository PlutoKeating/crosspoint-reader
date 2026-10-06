#pragma once
#include <BleSetupProtocol.h>

#include <cstdint>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Studio BLE service, the only content channel (Project.StockStick
// docs/product/BLE-ONLY-DELIVERY.md). Bound devices use the secret delivered
// at bind time: protocol 4 frame/program transfers (docs/product/BLE-TRANSFER-V4.md),
// firmware transfers (op fw4), Wi-Fi setup and the phone-relayed sync.
// Unbound devices run setup mode (protocol 3, docs/product/BLE-SETUP.md): the
// phone scans the setup QR (device id + one-time key K), binds the device to
// its account and pushes Wi-Fi credentials. The device cloud bearer is never
// exposed.
namespace studio_ble {
// Drops the bound credential (unbound in the cloud); no-op in setup mode.
void revoke();
// Adopts a rotated authority from the register response (ownership transfer,
// collaborator revoke, account deletion). Persisted like a bind; clears Studio
// content when the owner changes and drops a live session so the phone
// reconnects with the new key. Returns true when anything changed.
bool adoptAuthority(const std::string& deviceId, const std::string& secret, uint32_t epoch,
                    const std::string& owner);
// Deinitialises NimBLE (host and controller) so a TLS request or the flash
// writer can use its heap; on the C3, Wi-Fi + NimBLE + a TLS session may not
// fit together. Returns false (nothing to do) when NimBLE is not running.
// A connected phone blocks the release unless `forJob` names the cloud job:
// then STATUS shows `net_busy` = forJob briefly, the phone is disconnected and
// nothing advertises until restoreRadio() brings the stack back with the same
// identity and key (2.7.3: a phone that keeps reconnecting used to block
// every TLS job).
bool releaseRadio(const char* forJob = nullptr);
void restoreRadio();
void begin();
// The task that applies queued transfer work (pump()): the background sync
// worker, which is idle while a phone is connected. Set once at boot.
void setPumpTask(TaskHandle_t task);
// Applies queued transfer work (start, chunks, commit, abort) on the pump
// task; cheap when nothing is queued.
void pump();
void tick();
bool connected();
// A content transfer is receiving or being installed (ProjectStickHost keeps
// Wi-Fi off meanwhile so the heap goes to the transfer).
bool transferActive();

// Radio state as the user sees it (Settings > Bluetooth, status tags).
enum class Radio : uint8_t {
  Off,          // switched off in Settings
  Idle,         // on, but nothing to advertise yet (no identity or key)
  Advertising,  // discoverable, waiting for a phone
  Connected,    // a phone holds the link
  Paused,       // stack released for a cloud request or the flash writer
  Failed,       // the last start failed; tick() retries with backoff
};
enum class Transfer : uint8_t { None, Receiving, Refreshing };
// Allocation-free snapshot for the UI loop. `generation` changes whenever the
// radio or session state does; `completed`/`failed` count transfers since boot.
struct Link {
  Radio radio = Radio::Idle;
  Transfer transfer = Transfer::None;
  bool setupMode = false;
  size_t received = 0, total = 0;
  uint32_t completed = 0, failed = 0;
  // A stopped transfer the phone is expected to resume (shown as waiting, not failed).
  bool resuming = false;
  uint32_t generation = 0;
};
Link link();
// Everything the Bluetooth settings screen shows.
struct Diagnostics {
  Link link;
  std::string name, address, error;
  int errorCode = 0;
  uint32_t starts = 0, startFailures = 0, connections = 0, advertisingRestarts = 0;
  // GAP-level (2.7.7): every connect completion the host saw, the ones that
  // failed at link level (HCI status of the last, and the free heap then),
  // the ones refused because a phone was already linked, and the reason of
  // the last disconnect (NimBLE code: 0x200 + HCI reason; -1 none yet).
  uint32_t linkAttempts = 0, linkFailures = 0, linkRejected = 0, lastLinkFailureHeap = 0;
  int lastLinkFailure = 0, lastDisconnectReason = -1;
  // Unused stack of the NimBLE host task in bytes (0 when the stack is down).
  uint32_t hostStackFree = 0;
  // Seconds since the last connect / disconnect; -1 when there was none.
  int32_t sinceConnect = -1, sinceDisconnect = -1;
};
Diagnostics diagnostics();
// The user's switch (Settings > Bluetooth). Off deinitialises the stack and
// keeps it off; content cannot be delivered until it is switched on again.
void setEnabled(bool enabled);
bool enabled();
// Tears the stack down and starts it again (drops a connected phone).
void restart();

// Enters setup mode for an unbound device (no-op while a secret exists).
void setup(const std::string& deviceId);
// Setup QR payload, or empty when not in setup mode.
std::string setupPayload();

// Work queued by BLE callbacks, applied from the StockStick activity loop.
struct Binding {
  std::string token, owner, secret;
  uint32_t epoch = 0;
};
bool takeBinding(Binding& out);
// Persists the BLE credential on success (atomically); the current setup
// session keeps its key until the phone disconnects. Returns false when the
// credential could not be written: the caller must undo the store binding.
bool finishBinding(bool ok, const Binding& binding);
// The persisted BLE authority (identity and owner), loading it if needed.
// False when the device holds none (setup mode).
bool storedAuthority(std::string& deviceId, std::string& owner);
// Op `token` (bound mode, 2.7.2): the owner's phone hands a bound device
// that lost its cloud token a fresh one; applied by the host, which reports
// the outcome back.
struct TokenRequest {
  std::string owner, token;
};
bool takeTokenRequest(TokenRequest& out);
void finishToken(bool ok);

struct WifiRequest {
  std::string ssid, password;
};
bool takeWifiRequest(WifiRequest& out);
bool takeScanRequest();

// Firmware over BLE (op fw4, bound mode, 2.7.4; replaces op `ota`): the phone
// streams the image like a content transfer; the pump task writes it to
// firmware.tmp, then installs it like an SD-card update and restarts. STATUS
// `ota` reports the firmware_update phases (verifying, installing, ...).

// BLE-first sync (2.6.0, Project.StockStick docs/product/BLE-ONLY-DELIVERY.md):
// the STATE characteristic carries what the cloud heartbeat used to (firmware,
// metrics, pending events, an OTA outcome); the phone uploads it and answers
// with op `sync`. The activity builds the JSON (it may read the SD card and
// the store) and hands it over here; GATT reads only copy it.
void setState(std::string json);
struct SyncRequest {
  int64_t time = 0;  // phone/server clock, Unix seconds, 0 when absent
  int trading = -1;  // -1 unknown, 0 not a trading day, 1 trading day
  std::vector<std::string> ack;  // event ids the cloud accepted
  bool otaAck = false;           // the OTA outcome was uploaded
};
bool takeSyncRequest(SyncRequest& out);
// Op `unbind` (bound mode): the owner unbound the device in the mini program
// while next to it; the activity drops the binding like a `bound:false`.
bool takeUnbindRequest();

enum class WifiState { Idle, Connecting, Connected, Failed };
void reportWifi(WifiState state, const std::string& ssid, const char* error = "");
void reportScan(bool scanning, std::vector<ble_setup::Network> networks = {});
// STATUS `wifi.saved`: the device has at least one saved network to join on demand.
void setWifiSaved(bool saved);
}  // namespace studio_ble
