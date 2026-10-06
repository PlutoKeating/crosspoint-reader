#include "WifiAutoConnect.h"

#include <Logging.h>
#include <WiFi.h>

#include <algorithm>

#include "WifiCredentialStore.h"

void WifiAutoConnect::startAttempt(uint32_t nowMs) {
  const auto& saved = WIFI_STORE.getCredentials();
  if (saved.empty()) {
#ifdef SIMULATOR
    // The simulator's fake network needs no credentials; it comes back when
    // the on-demand policy wants Wi-Fi again.
    WiFi.mode(WIFI_STA);
    WiFi.begin("Simulator WiFi (fake)");
    connecting = true;
    attemptStartedMs = nowMs;
    return;
#endif
    nextAttemptMs = nowMs + MAX_BACKOFF_MS;  // nothing to try until the user adds a network
    return;
  }
  // Each round tries every saved network once, starting with the last one
  // that worked; step 0 is that network, later steps walk the others in order.
  const WifiCredential* last = WIFI_STORE.findCredential(WIFI_STORE.getLastConnectedSsid());
  const WifiCredential* credential = nullptr;
  if (step == 0 && last) {
    credential = last;
  } else {
    const size_t index = last ? step - 1 : step;
    size_t seen = 0;
    for (const auto& savedNetwork : saved) {
      if (&savedNetwork == last) continue;
      if (seen++ == index) {
        credential = &savedNetwork;
        break;
      }
    }
  }
  step = (step + 1) % saved.size();
  if (!credential) credential = &saved.front();  // list changed mid-round

  WiFi.persistent(false);  // credentials live in WifiCredentialStore, not SDK NVS
  WiFi.mode(WIFI_STA);
  if (credential->password.empty()) {
    WiFi.begin(credential->ssid.c_str());
  } else {
    WiFi.begin(credential->ssid.c_str(), credential->password.c_str());
  }
  connecting = true;
  attemptStartedMs = nowMs;
  LOG_INF("WIFI", "Auto-connecting to saved network %s", credential->ssid.c_str());
}

bool WifiAutoConnect::tick(uint32_t nowMs) {
  const bool connected = WiFi.status() == WL_CONNECTED;
  const bool cameUp = connected && !wasConnected;
  wasConnected = connected;
  if (connected) {
    if (connecting) LOG_INF("WIFI", "Auto-connect succeeded");
    connecting = false;
    failures = 0;
    step = 0;
    return cameUp;
  }
  if (!loaded) {
    WIFI_STORE.loadFromFile();
    loaded = true;
  }
  if (connecting) {
    if (nowMs - attemptStartedMs < CONNECT_TIMEOUT_MS) return false;
    connecting = false;
    // Power the radio down between attempts: an unreachable saved network
    // otherwise keeps the Wi-Fi driver (~50 KB) resident for nothing, heap
    // that BLE transfers and rendering need. startAttempt() brings it back.
    WiFi.disconnect(true);
    if (failures < 16) ++failures;
    // One backoff step per full round over the saved networks.
    const size_t networks = std::max<size_t>(1, WIFI_STORE.getCredentials().size());
    const uint32_t rounds = failures / networks;
    const uint32_t backoff =
        rounds == 0 ? 0 : std::min<uint32_t>(MAX_BACKOFF_MS, FIRST_BACKOFF_MS << std::min<uint32_t>(rounds - 1, 5));
    nextAttemptMs = nowMs + backoff;
    return false;
  }
  if (static_cast<int32_t>(nowMs - nextAttemptMs) >= 0) startAttempt(nowMs);
  return false;
}
