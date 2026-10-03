#include "OtaTrial.h"

#include <Arduino.h>
#include <Logging.h>
#include <Preferences.h>
#include <StickFirmware.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>

#include <atomic>
#include <cstring>

#include "FirmwareFlasher.h"
#include "OtaBootSwitch.h"

// Arduino's initArduino() marks a PENDING_VERIFY image valid immediately unless
// this weak hook returns true. The trial below decides instead.
extern "C" bool verifyRollbackLater() { return true; }

namespace ota_trial {

namespace {
constexpr const char* NS = "stick_ota";

portMUX_TYPE healthMux = portMUX_INITIALIZER_UNLOCKED;
bool trialActive = false;
bool apiResponded = false;
bool phoneSynced = false;
bool everOnline = false;
uint8_t transportFailures = 0;
uint32_t firstFailureMs = 0;
uint32_t lastFailureMs = 0;
char trialVersion[33] = {};
std::atomic<bool> outcomePending{false};  // read by the sync task, written by the main loop

bool abnormalReset(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
      return true;  // brownouts are a power problem, not evidence against the image
    default:
      return false;
  }
}

void copyString(char* dest, size_t size, const char* src) {
  strncpy(dest, src ? src : "", size - 1);
  dest[size - 1] = '\0';
}

void markRunningValid() {
  esp_ota_img_states_t state;
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (running && esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
    esp_ota_mark_app_valid_cancel_rollback();
  }
}

void saveOutcome(Preferences& prefs, bool rolledBack, const char* reason) {
  outcomePending = true;
  prefs.putBool("out", true);
  prefs.putBool("out_rb", rolledBack);
  prefs.putString("out_ver", prefs.getString("ver", ""));
  prefs.putString("out_why", reason);
}

void finishTrial(Preferences& prefs) {
  prefs.putBool("armed", false);
  prefs.putUChar("tries", 0);
}

void rollBackNow(const char* reason) {
  Preferences prefs;
  if (!prefs.begin(NS, false)) return;
  const String previous = prefs.getString("prev", "");
  const esp_partition_t* target = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY,
                                                           previous.length() ? previous.c_str() : nullptr);
  if (!target || previous.length() == 0) {
    // Nothing to return to: keep running the new image rather than bricking.
    LOG_ERR("OTA", "Rollback requested (%s) but previous slot '%s' is missing", reason, previous.c_str());
    saveOutcome(prefs, false, "rollback_unavailable");
    finishTrial(prefs);
    prefs.end();
    markRunningValid();
    return;
  }
  saveOutcome(prefs, true, reason);
  finishTrial(prefs);
  prefs.end();
  LOG_ERR("OTA", "Rolling back to %s: %s", target->label, reason);
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (running && esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
    esp_ota_mark_app_invalid_rollback_and_reboot();  // bootloader-managed rollback; does not return on success
  }
  if (ota_boot::switchTo(target)) {
    delay(100);
    ESP.restart();
  }
  LOG_ERR("OTA", "otadata switch for rollback failed; staying on the new image");
}
}  // namespace

void onBoot() {
  Preferences prefs;
  if (!prefs.begin(NS, false)) {
    markRunningValid();
    return;
  }
  outcomePending = prefs.getBool("out", false);
  stick_fw::TrialRecord record;
  record.armed = prefs.getBool("armed", false);
  record.attempts = prefs.getUChar("tries", 0);
  copyString(record.previousSlot, sizeof(record.previousSlot), prefs.getString("prev", "").c_str());
  copyString(record.targetSlot, sizeof(record.targetSlot), prefs.getString("target", "").c_str());
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_reset_reason_t reason = esp_reset_reason();
  const auto action = stick_fw::decideOnBoot(record, running ? running->label : "", abnormalReset(reason));

  switch (action) {
    case stick_fw::BootAction::None:
      prefs.end();
      // Images installed without a trial (USB, web flasher) are trusted.
      markRunningValid();
      return;
    case stick_fw::BootAction::RolledBack:
      LOG_ERR("OTA", "Trial image did not stay booted; running previous slot %s", record.previousSlot);
      saveOutcome(prefs, true, "bootloader_rollback");
      finishTrial(prefs);
      prefs.end();
      markRunningValid();
      return;
    case stick_fw::BootAction::RollbackNow:
      prefs.putUChar("tries", record.attempts);
      prefs.end();
      rollBackNow("repeated_crash");
      return;
    case stick_fw::BootAction::Continue:
      prefs.putUChar("tries", record.attempts);
      // The app-level counter now owns the trial. A bootloader with rollback
      // enabled would otherwise abort a PENDING_VERIFY image on *any* reset
      // (power loss, brownout, one panic), overriding the 3-failure rule; it
      // still protects the window before this point, i.e. images that crash
      // before setup() reaches onBoot().
      markRunningValid();
      copyString(trialVersion, sizeof(trialVersion), prefs.getString("ver", "").c_str());
      prefs.end();
      taskENTER_CRITICAL(&healthMux);
      trialActive = true;
      taskEXIT_CRITICAL(&healthMux);
      LOG_INF("OTA", "Trial boot of %s (failed attempts %u/%u, reset reason %d)", trialVersion,
              static_cast<unsigned>(record.attempts), static_cast<unsigned>(stick_fw::MAX_TRIAL_FAILURES),
              static_cast<int>(reason));
      return;
  }
}

bool arm(const esp_partition_t* target, const char* targetVersion) {
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (!target || !running) return false;
  Preferences prefs;
  if (!prefs.begin(NS, false)) return false;
  const char* version = targetVersion ? targetVersion : "";
  prefs.putString("prev", running->label);
  prefs.putString("target", target->label);
  prefs.putString("ver", version);
  prefs.putUChar("tries", 0);
  prefs.putBool("armed", true);
  // Read back: put*() cannot distinguish an empty value from a failed write.
  const bool ok = prefs.getString("prev", "") == running->label && prefs.getString("target", "") == target->label &&
                  prefs.getString("ver", "\x01") == version &&
                  prefs.getUChar("tries", 0xFF) == 0 && prefs.getBool("armed", false);
  if (!ok) prefs.putBool("armed", false);
  prefs.end();
  LOG_INF("OTA", "Trial armed: %s -> %s (%s)", running->label, target->label, targetVersion ? targetVersion : "");
  return ok;
}

void disarm() {
  if (active()) return;  // never cancel the trial of the image that is running
  Preferences prefs;
  if (!prefs.begin(NS, false)) return;
  finishTrial(prefs);
  prefs.end();
  LOG_INF("OTA", "Trial disarmed: install did not switch boot slots");
}

bool active() {
  taskENTER_CRITICAL(&healthMux);
  const bool value = trialActive;
  taskEXIT_CRITICAL(&healthMux);
  return value;
}

void noteApiResult(int httpStatus, bool wifiConnected) {
  taskENTER_CRITICAL(&healthMux);
  if (trialActive) {
    if (wifiConnected) everOnline = true;
    if (httpStatus > 0) {
      apiResponded = true;
    } else if (wifiConnected && transportFailures < UINT8_MAX) {
      // Sample: one request cycle retries several times, which is one failure.
      const uint32_t now = millis();
      if (transportFailures == 0) {
        firstFailureMs = lastFailureMs = now;
        transportFailures = 1;
      } else if (now - lastFailureMs >= stick_fw::FAILURE_SAMPLE_MS) {
        lastFailureMs = now;
        ++transportFailures;
      }
    }
  }
  taskEXIT_CRITICAL(&healthMux);
}

void notePhoneSync() {
  taskENTER_CRITICAL(&healthMux);
  if (trialActive) phoneSynced = true;
  taskEXIT_CRITICAL(&healthMux);
}

void tick(bool wifiConnected) {
  stick_fw::HealthInputs inputs;
  taskENTER_CRITICAL(&healthMux);
  // Failures only count while one Wi-Fi association lasts; losing the link
  // points at the network, not at the image.
  if (!wifiConnected) transportFailures = 0;
  const bool running = trialActive;
  const bool viaPhone = phoneSynced && !apiResponded;
  inputs.uptimeMs = millis();
  inputs.apiResponded = apiResponded || phoneSynced;
  inputs.everOnline = everOnline;
  inputs.transportFailures = transportFailures;
  inputs.failureSpanMs = transportFailures ? lastFailureMs - firstFailureMs : 0;
  taskEXIT_CRITICAL(&healthMux);
  // Never switch slots while an install is erasing the update slot.
  if (!running || firmware_flash::installInProgress()) return;

  const auto decision = stick_fw::evaluateHealth(inputs);
  if (decision == stick_fw::HealthDecision::Wait) return;
  if (decision == stick_fw::HealthDecision::Rollback) {
    rollBackNow("cloud_unreachable");
    return;
  }
  Preferences prefs;
  if (prefs.begin(NS, false)) {
    saveOutcome(prefs, false, viaPhone ? "phone_ok" : inputs.apiResponded ? "api_ok" : "offline_uptime");
    finishTrial(prefs);
    prefs.end();
  }
  markRunningValid();
  taskENTER_CRITICAL(&healthMux);
  trialActive = false;
  taskEXIT_CRITICAL(&healthMux);
  LOG_INF("OTA", "Firmware %s confirmed (%s)", trialVersion,
          viaPhone ? "phone sync" : inputs.apiResponded ? "API reachable" : "offline");
}

void onCleanShutdown() {
  if (!active()) return;
  Preferences prefs;
  if (prefs.begin(NS, false)) {
    saveOutcome(prefs, false, "clean_shutdown");
    finishTrial(prefs);
    prefs.end();
  }
  markRunningValid();
  taskENTER_CRITICAL(&healthMux);
  trialActive = false;
  taskEXIT_CRITICAL(&healthMux);
}

Outcome pendingOutcome() {
  Outcome outcome;
  Preferences prefs;
  if (!prefs.begin(NS, true)) return outcome;
  outcome.pending = prefs.getBool("out", false);
  if (outcome.pending) {
    outcome.rolledBack = prefs.getBool("out_rb", false);
    copyString(outcome.version, sizeof(outcome.version), prefs.getString("out_ver", "").c_str());
    copyString(outcome.reason, sizeof(outcome.reason), prefs.getString("out_why", "").c_str());
  }
  prefs.end();
  return outcome;
}

bool hasPendingOutcome() { return outcomePending; }

void clearOutcome() {
  outcomePending = false;
  Preferences prefs;
  if (!prefs.begin(NS, false)) return;
  prefs.putBool("out", false);
  prefs.end();
}

}  // namespace ota_trial
