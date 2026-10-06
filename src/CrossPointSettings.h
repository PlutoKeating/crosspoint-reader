#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstdint>

// Device-level settings for the StockStick firmware. Only what the device
// itself needs is persisted; display, theme, sleep-screen and button-layout
// choices were removed in 2.7.4 (fixed behaviour: the card stays on screen
// with a sleep marker on power-key sleep, a short power press refreshes the
// screen, the default front-button layout). Keys the firmware no longer knows
// are ignored on load and dropped on the next save.
class CrossPointSettings : public PersistableStore<CrossPointSettings> {
 private:
  // Private constructor for singleton
  CrossPointSettings() = default;

  friend class PersistableStore<CrossPointSettings>;

 public:
  // Set once NTP has seeded the RTC after the first Wi-Fi connection.
  uint8_t clockHasBeenSynced = 0;
  // Bluetooth radio switch (Settings > Bluetooth). Content only arrives over
  // BLE, so this stays on unless the user turns it off for diagnosis.
  uint8_t bluetoothEnabled = 1;
  // UI language code: the built-in catalogue ("ZH") or an SD-card pack in
  // /.crosspoint/lang/<code>.lang. Persisted as the "language" string.
  char language[8] = "ZH";
  // The device never powers itself off: the card stays on screen with idle
  // power saving between loop iterations, and only the power key sleeps it.

  // Power-key hold time (ms) that puts the device to sleep; a shorter press
  // refreshes the screen.
  static constexpr uint16_t POWER_BUTTON_SLEEP_MS = 400;

  static const char* getFilePath() { return "/.crosspoint/settings.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);
};

// Helper macro to access settings
#define SETTINGS CrossPointSettings::getInstance()
