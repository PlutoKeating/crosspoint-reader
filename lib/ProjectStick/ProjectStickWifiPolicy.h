#pragma once

#include <cstdint>

// When the Wi-Fi radio is up (docs/memory-budget.md "2.7.2"). BLE-first: the
// phone relays state and delivers content, so the device rarely needs the
// cloud, and on the C3 the Wi-Fi driver (~50 KB of heap) cannot stay resident
// next to NimBLE and a content transfer. Wi-Fi is on demand: brought up for
// the few cloud jobs and the pages that need it, and powered down otherwise.
// Pure, host-tested; ProjectStickHost gathers the inputs and acts.
namespace project_stick {

struct WifiNeeds {
  bool phoneLinked = false;     // a phone holds the BLE link
  bool transferActive = false;  // a content transfer is receiving or installing
  bool bleJoinActive = false;   // a Wi-Fi join or scan asked over BLE is running
  bool joinHold = false;        // within WIFI_JOIN_HOLD_MS of a BLE join that succeeded
  bool pageHold = false;        // the Wi-Fi settings or the firmware update page is open
  bool cloudJob = false;        // a cloud job is queued or running on the worker
  bool otaRequested = false;    // a BLE `ota` request waits for the worker
  bool heartbeatDue = false;    // register due, no fresh phone sync, credential held
  bool alertWindow = false;     // credential held and inside trading hours
  bool trialPending = false;    // a trial boot or an unreported OTA outcome
};

// Pages and BLE-driven Wi-Fi work win; a running transfer loses (its heap);
// a linked phone otherwise keeps the radio down (it relays what the cloud
// would tell us); alone, the device needs the radio only for cloud jobs.
inline bool wifiWanted(const WifiNeeds& n) {
  if (n.transferActive) return false;
  if (n.bleJoinActive || n.joinHold || n.pageHold) return true;
  if (n.cloudJob || n.otaRequested) return true;
  if (n.phoneLinked) return false;
  return n.heartbeatDue || n.alertWindow || n.trialPending;
}

// Whether a cloud job may run while a phone holds the BLE link (2.7.3). A job
// that needs TLS cannot share the heap with a connected NimBLE stack on the
// C3, so a running job disconnects the phone and keeps the radio released
// (no advertising) until it ends. User-initiated jobs (the on-device firmware
// check or install, a BLE `ota` request) go at once. Background jobs (register
// heartbeat, alert poll, event flush) leave an active phone alone: never during
// a content transfer, and otherwise for at most PHONE_DEFER_MAX_MS of
// continuous link; an idle phone (no ops for PHONE_ACTIVE_WINDOW_MS) yields
// right away. Without this a phone that keeps reconnecting blocked every job.
enum class PhoneLinkAction : uint8_t {
  Proceed,     // no phone linked
  Defer,       // background job: try again later
  Disconnect,  // run now: disconnect the phone and release NimBLE first
};

struct PhoneLinkInputs {
  bool phoneLinked = false;
  bool userInitiated = false;
  bool transferActive = false;  // a content transfer is receiving or installing
  uint32_t linkedForMs = 0;     // since the current link was made
  uint32_t idleForMs = 0;       // since the phone's last read or write
};

constexpr uint32_t PHONE_DEFER_MAX_MS = 120000;
constexpr uint32_t PHONE_ACTIVE_WINDOW_MS = 30000;

inline PhoneLinkAction phoneLinkAction(const PhoneLinkInputs& in) {
  if (!in.phoneLinked) return PhoneLinkAction::Proceed;
  if (in.userInitiated) return PhoneLinkAction::Disconnect;
  if (in.transferActive) return PhoneLinkAction::Defer;
  const bool active = in.idleForMs < PHONE_ACTIVE_WINDOW_MS;
  if (active && in.linkedForMs < PHONE_DEFER_MAX_MS) return PhoneLinkAction::Defer;
  return PhoneLinkAction::Disconnect;
}

constexpr uint32_t WIFI_IDLE_GRACE_MS = 20000;  // unwanted this long -> power down
constexpr uint32_t WIFI_JOIN_HOLD_MS = 60000;   // the phone reads `connected` after a BLE join
constexpr uint32_t WIFI_JOB_WAIT_MS = 20000;    // a worker job waits this long for the link

// Whether to power the radio down now. Immediately when the heap is needed
// (a phone is linked or a transfer runs); after the grace otherwise, so a
// burst of short jobs does not cycle the radio.
inline bool wifiShouldPowerOff(const WifiNeeds& n, uint32_t nowMs, uint32_t lastWantedMs) {
  if (wifiWanted(n)) return false;
  if (n.transferActive || n.phoneLinked) return true;
  return nowMs - lastWantedMs >= WIFI_IDLE_GRACE_MS;
}

}  // namespace project_stick
