#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstdint>

// Device-level settings for the StockStick firmware. The ebook reader and its
// settings were removed; enum values that are persisted keep their historical
// numbers so settings.json written by older firmware is never misread. Keys the
// firmware no longer knows are ignored on load and dropped on the next save.
class CrossPointSettings : public PersistableStore<CrossPointSettings> {
 private:
  // Private constructor for singleton
  CrossPointSettings() = default;

  friend class PersistableStore<CrossPointSettings>;

 public:
  // Persisted by value. COVER and COVER_CUSTOM belonged to the removed reader;
  // loading them folds to DARK and CUSTOM respectively.
  enum SLEEP_SCREEN_MODE {
    DARK = 0,
    LIGHT = 1,
    CUSTOM = 2,
    COVER = 3,
    COVER_CUSTOM = 4,
    BLANK = 5,
    QUICK_RESUME = 6,
    SLEEP_SCREEN_MODE_COUNT
  };
  // Applies to custom sleep images from /sleep or /.sleep.
  enum SLEEP_SCREEN_COVER_MODE { FIT = 0, CROP = 1, SLEEP_SCREEN_COVER_MODE_COUNT };
  enum SLEEP_SCREEN_COVER_FILTER {
    NO_FILTER = 0,
    BLACK_AND_WHITE = 1,
    INVERTED_BLACK_AND_WHITE = 2,
    SLEEP_SCREEN_COVER_FILTER_COUNT
  };

  // Front button hardware identifiers (for remapping)
  enum FRONT_BUTTON_HARDWARE {
    FRONT_HW_BACK = 0,
    FRONT_HW_CONFIRM = 1,
    FRONT_HW_LEFT = 2,
    FRONT_HW_RIGHT = 3,
    FRONT_BUTTON_HARDWARE_COUNT
  };

  // Short power button press actions. PAGE_TURN (2) and FOOTNOTES (4) were
  // reader actions; loading them folds to IGNORE.
  enum SHORT_PWRBTN { IGNORE = 0, SLEEP = 1, PAGE_TURN = 2, FORCE_REFRESH = 3, FOOTNOTES = 4, SHORT_PWRBTN_COUNT };

  enum HIDE_BATTERY_PERCENTAGE { HIDE_NEVER = 0, HIDE_READER = 1, HIDE_ALWAYS = 2, HIDE_BATTERY_PERCENTAGE_COUNT };

  // LYRA_3_COVERS (2) showed book covers on Home; loading it folds to LYRA.
  enum UI_THEME { CLASSIC = 0, LYRA = 1, LYRA_3_COVERS = 2, ROUNDEDRAFF = 3 };

  // Sleep screen settings
  uint8_t sleepScreen = DARK;
  uint8_t sleepScreenCoverMode = FIT;
  uint8_t sleepScreenCoverFilter = NO_FILTER;
  // Set once NTP has seeded the RTC after the first Wi-Fi connection.
  uint8_t clockHasBeenSynced = 0;
  uint8_t shortPwrBtn = IGNORE;
  // Front button remap
  uint8_t frontButtonBack = FRONT_HW_BACK;
  uint8_t frontButtonConfirm = FRONT_HW_CONFIRM;
  uint8_t frontButtonLeft = FRONT_HW_LEFT;
  uint8_t frontButtonRight = FRONT_HW_RIGHT;
  uint8_t hideBatteryPercentage = HIDE_NEVER;
  uint8_t uiTheme = LYRA;
  // Sunlight fading compensation
  uint8_t fadingFix = 0;
  // Bluetooth radio switch (Settings > Bluetooth). Content only arrives over
  // BLE, so this stays on unless the user turns it off for diagnosis.
  uint8_t bluetoothEnabled = 1;
  // UI language code: the built-in catalogue ("ZH") or an SD-card pack in
  // /.crosspoint/lang/<code>.lang. Persisted as the "language" string.
  char language[8] = "ZH";
  // The device never powers itself off: the card stays on screen with idle
  // power saving between loop iterations, and only the power key sleeps it
  // (2.7.3; stale "sleepTimeout*" / "quickResumeSleepScreen" keys in older
  // settings files are ignored).

  uint16_t getPowerButtonDuration() const {
    return (shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP) ? 10 : 400;
  }

  static const char* getFilePath() { return "/.crosspoint/settings.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  static void validateFrontButtonMapping(CrossPointSettings& settings);
};

// Helper macro to access settings
#define SETTINGS CrossPointSettings::getInstance()
