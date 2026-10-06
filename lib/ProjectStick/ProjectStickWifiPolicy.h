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
  bool cloudJob = false;        // a background cloud job is queued or running on the worker
  bool userJob = false;         // an on-device firmware check or install (Settings) runs
  bool heartbeatDue = false;    // register due, no fresh phone sync, credential held
  bool alertWindow = false;     // credential held and inside trading hours
  bool trialPending = false;    // a trial boot or an unreported OTA outcome
};

// A running transfer loses (its heap). Work the user asked for wins: a BLE
// Wi-Fi join (the phone waits for `connected`), the Wi-Fi and firmware pages
// and an on-device firmware check or install (which disconnect the phone
// first). Otherwise a linked phone keeps the radio down (2.7.4): it relays
// what the cloud would tell us and delivers firmware itself (op fw4), so every
// background job waits until it leaves. Alone, the device needs the radio
// only for cloud jobs.
inline bool wifiWanted(const WifiNeeds& n) {
  if (n.transferActive) return false;
  if (n.bleJoinActive || n.joinHold || n.pageHold || n.userJob) return true;
  if (n.phoneLinked) return false;
  return n.cloudJob || n.heartbeatDue || n.alertWindow || n.trialPending;
}

// Whether a cloud job may run while a phone holds the BLE link. A job that
// needs TLS cannot share the heap with a connected NimBLE stack on the C3.
// A job the user started on the device (Settings: firmware check or install)
// disconnects the phone and keeps the radio released (no advertising) until it
// ends. Background jobs (register heartbeat, alert poll, event flush) wait for
// as long as a phone is linked (2.7.4; 2.7.3 forced an active phone off after
// two minutes): the phone relays the heartbeat's information and delivers
// firmware over BLE, so nothing it holds back is lost.
enum class PhoneLinkAction : uint8_t {
  Proceed,     // no phone linked
  Defer,       // background job: try again later
  Disconnect,  // run now: disconnect the phone and release NimBLE first
};

struct PhoneLinkInputs {
  bool phoneLinked = false;
  bool userInitiated = false;  // started on the device itself
};

inline PhoneLinkAction phoneLinkAction(const PhoneLinkInputs& in) {
  if (!in.phoneLinked) return PhoneLinkAction::Proceed;
  return in.userInitiated ? PhoneLinkAction::Disconnect : PhoneLinkAction::Defer;
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
