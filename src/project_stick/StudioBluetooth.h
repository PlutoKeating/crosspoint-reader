#pragma once
#include <BleSetupProtocol.h>

#include <cstdint>
#include <string>
#include <vector>

// Studio BLE service, the only content channel (Project.StockStick
// docs/product/BLE-ONLY-DELIVERY.md). Bound devices use the secret delivered
// at bind time: protocol 2 frames/programs, Wi-Fi setup and OTA triggers.
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
// fit together. Returns false (nothing to do) when NimBLE is not running or a phone is
// connected. restoreRadio() brings it back with the same identity and key.
bool releaseRadio();
void restoreRadio();
void begin();
void tick();
bool connected();

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
  uint32_t generation = 0;
};
Link link();
// Everything the Bluetooth settings screen shows.
struct Diagnostics {
  Link link;
  std::string name, address, error;
  int errorCode = 0;
  uint32_t starts = 0, startFailures = 0, connections = 0, advertisingRestarts = 0;
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
// Persists the BLE credential on success; the current setup session keeps its
// key until the phone disconnects.
void finishBinding(bool ok, const Binding& binding);

struct WifiRequest {
  std::string ssid, password;
};
bool takeWifiRequest(WifiRequest& out);
bool takeScanRequest();

// Op `ota` (bound mode): the phone names a catalogue image; the activity hands
// it to the background worker. STATUS reports firmware_update progress.
struct OtaRequest {
  std::string version, url, sha256;
  size_t bytes = 0;
};
bool takeOtaRequest(OtaRequest& out);

enum class WifiState { Idle, Connecting, Connected, Failed };
void reportWifi(WifiState state, const std::string& ssid, const char* error = "");
void reportScan(bool scanning, std::vector<ble_setup::Network> networks = {});
}  // namespace studio_ble
