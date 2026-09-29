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
bool everOnline = false;
uint8_t transportFailures = 0;
uint32_t firstFailureMs = 0;
char trialCommand[40] = {};
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
  prefs.putString("out_cmd", prefs.getString("cmd", ""));
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
      copyString(trialCommand, sizeof(trialCommand), prefs.getString("cmd", "").c_str());
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

bool arm(const esp_partition_t* target, const char* commandId, const char* targetVersion) {
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (!target || !running) return false;
  Preferences prefs;
  if (!prefs.begin(NS, false)) return false;
  const bool ok =
      prefs.putString("prev", running->label) > 0 && prefs.putString("target", target->label) > 0 &&
      prefs.putString("cmd", commandId ? commandId : "") == strlen(commandId ? commandId : "") &&
      prefs.putString("ver", targetVersion ? targetVersion : "") == strlen(targetVersion ? targetVersion : "") &&
      prefs.putUChar("tries", 0) == 1 && prefs.putBool("armed", true) == 1;
  prefs.end();
  LOG_INF("OTA", "Trial armed: %s -> %s (%s)", running->label, target->label, targetVersion ? targetVersion : "");
  return ok;
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
      if (transportFailures++ == 0) firstFailureMs = millis();
    }
  }
  taskEXIT_CRITICAL(&healthMux);
}

void tick() {
  stick_fw::HealthInputs inputs;
  taskENTER_CRITICAL(&healthMux);
  const bool running = trialActive;
  inputs.uptimeMs = millis();
  inputs.apiResponded = apiResponded;
  inputs.everOnline = everOnline;
  inputs.transportFailures = transportFailures;
  inputs.onlineSinceFirstFailureMs = transportFailures ? millis() - firstFailureMs : 0;
  taskEXIT_CRITICAL(&healthMux);
  if (!running) return;

  const auto decision = stick_fw::evaluateHealth(inputs);
  if (decision == stick_fw::HealthDecision::Wait) return;
  if (decision == stick_fw::HealthDecision::Rollback) {
    rollBackNow("cloud_unreachable");
    return;
  }
  Preferences prefs;
  if (prefs.begin(NS, false)) {
    saveOutcome(prefs, false, inputs.apiResponded ? "api_ok" : "offline_uptime");
    finishTrial(prefs);
    prefs.end();
  }
  markRunningValid();
  taskENTER_CRITICAL(&healthMux);
  trialActive = false;
  taskEXIT_CRITICAL(&healthMux);
  LOG_INF("OTA", "Firmware %s confirmed (%s)", trialVersion, inputs.apiResponded ? "API reachable" : "offline");
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
    copyString(outcome.commandId, sizeof(outcome.commandId), prefs.getString("out_cmd", "").c_str());
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
