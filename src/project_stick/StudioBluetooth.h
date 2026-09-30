#pragma once
#include <BleSetupProtocol.h>

#include <cstdint>
#include <string>
#include <vector>

// Studio BLE service. Bound devices use the cloud-issued secret (protocol 2:
// offline frames/programs, plus Wi-Fi setup). Unbound devices run setup mode
// (protocol 3, Project.StockStick docs/product/BLE-SETUP.md): the phone scans
// the setup QR (device id + one-time key K), binds the device to its account
// and pushes Wi-Fi credentials. The device cloud bearer is never exposed.
namespace studio_ble {
void configure(const std::string& deviceId, const std::string& secret, uint32_t epoch, const std::string& owner = "");
// Drops the bound credential (no-op in setup mode, so an unbound register
// heartbeat never tears down a setup session).
void revoke();
void pause(bool paused);
void begin();
void tick();
bool connected();

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

enum class WifiState { Idle, Connecting, Connected, Failed };
void reportWifi(WifiState state, const std::string& ssid, const char* error = "");
void reportScan(bool scanning, std::vector<ble_setup::Network> networks = {});
}  // namespace studio_ble
