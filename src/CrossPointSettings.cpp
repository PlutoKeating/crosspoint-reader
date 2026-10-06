#include "CrossPointSettings.h"

#include <Logging.h>

#include <cstring>

void CrossPointSettings::toJson(JsonDocument& doc) const {
  doc["clockHasBeenSynced"] = clockHasBeenSynced;
  doc["bluetoothEnabled"] = bluetoothEnabled;
  // A catalogue code (built-in "ZH" or an SD-card language pack).
  doc["language"] = language;
}

bool CrossPointSettings::fromJson(JsonVariantConst doc) {
  const uint8_t synced = doc["clockHasBeenSynced"] | (uint8_t)0;
  clockHasBeenSynced = synced ? 1 : 0;
  const uint8_t bluetooth = doc["bluetoothEnabled"] | (uint8_t)1;
  bluetoothEnabled = bluetooth ? 1 : 0;
  // Older firmware stored upstream codes such as "EN"; they now select the
  // matching SD pack, or the built-in catalogue if that pack is not installed.
  strncpy(language, doc["language"] | "ZH", sizeof(language) - 1);
  language[sizeof(language) - 1] = '\0';
  // Settings removed in 2.7.4 (sleepScreen*, uiTheme, hideBatteryPercentage,
  // fadingFix, shortPwrBtn, frontButton*) may still be in older files; they
  // are dropped at the next save.
  if (doc["uiTheme"].is<uint8_t>() || doc["sleepScreen"].is<uint8_t>() || doc["frontButtonBack"].is<uint8_t>())
    requestResave();
  LOG_DBG("CPS", "Settings loaded from file");
  return true;
}
