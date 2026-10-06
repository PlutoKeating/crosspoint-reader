#pragma once

#include <BoardConfig.h>
#include <I18n.h>

#include <algorithm>
#include <vector>

#include "CrossPointSettings.h"
#include "activities/settings/SettingsActivity.h"

// Picker over a subset of a persisted enum. The option index shown on device
// maps to an explicit stored value, so values retired with the ebook reader are
// never offered while existing numbers keep their meaning. Mapped entries have
// no valuePtr: CrossPointSettings::toJson/fromJson persist them by name.
inline SettingInfo mappedEnumSetting(StrId nameId, uint8_t CrossPointSettings::* field, std::vector<uint8_t> values,
                                     std::vector<StrId> labels, StrId category) {
  return SettingInfo::DynamicEnum(
      nameId, std::move(labels),
      [field, values]() -> uint8_t {
        const auto it = std::find(values.begin(), values.end(), SETTINGS.*field);
        return it == values.end() ? 0 : static_cast<uint8_t>(it - values.begin());
      },
      [field, values](uint8_t index) {
        if (index < values.size()) SETTINGS.*field = values[index];
      },
      nullptr, category);
}

// Settings shown on the device, grouped by category. Built once; every call
// returns a copy so callers may filter it freely.
inline std::vector<SettingInfo> getSettingsList() {
  static const std::vector<SettingInfo> baseList = [] {
    std::vector<SettingInfo> v = {
        // --- Display ---
        mappedEnumSetting(
            StrId::STR_SLEEP_SCREEN, &CrossPointSettings::sleepScreen,
            {CrossPointSettings::DARK, CrossPointSettings::LIGHT, CrossPointSettings::CUSTOM, CrossPointSettings::BLANK,
             CrossPointSettings::QUICK_RESUME},
            {StrId::STR_DARK, StrId::STR_LIGHT, StrId::STR_CUSTOM, StrId::STR_NONE_OPT, StrId::STR_QUICK_RESUME},
            StrId::STR_CAT_DISPLAY),
        SettingInfo::Enum(StrId::STR_SLEEP_COVER_MODE, &CrossPointSettings::sleepScreenCoverMode,
                          {StrId::STR_FIT, StrId::STR_CROP}, "sleepScreenCoverMode", StrId::STR_CAT_DISPLAY),
        SettingInfo::Enum(StrId::STR_SLEEP_COVER_FILTER, &CrossPointSettings::sleepScreenCoverFilter,
                          {StrId::STR_NONE_OPT, StrId::STR_FILTER_CONTRAST, StrId::STR_INVERTED},
                          "sleepScreenCoverFilter", StrId::STR_CAT_DISPLAY),
        mappedEnumSetting(StrId::STR_HIDE_BATTERY, &CrossPointSettings::hideBatteryPercentage,
                          {CrossPointSettings::HIDE_NEVER, CrossPointSettings::HIDE_ALWAYS},
                          {StrId::STR_NEVER, StrId::STR_ALWAYS}, StrId::STR_CAT_DISPLAY),
        mappedEnumSetting(StrId::STR_UI_THEME, &CrossPointSettings::uiTheme,
                          {CrossPointSettings::CLASSIC, CrossPointSettings::LYRA, CrossPointSettings::ROUNDEDRAFF},
                          {StrId::STR_THEME_CLASSIC, StrId::STR_THEME_LYRA, StrId::STR_THEME_ROUNDEDRAFF},
                          StrId::STR_CAT_DISPLAY),
        SettingInfo::Toggle(StrId::STR_SUNLIGHT_FADING_FIX, &CrossPointSettings::fadingFix, "fadingFix",
                            StrId::STR_CAT_DISPLAY),

        // --- Controls ---
        mappedEnumSetting(StrId::STR_SHORT_PWR_BTN, &CrossPointSettings::shortPwrBtn,
                          {CrossPointSettings::IGNORE, CrossPointSettings::SLEEP, CrossPointSettings::FORCE_REFRESH},
                          {StrId::STR_IGNORE, StrId::STR_SLEEP, StrId::STR_FORCE_REFRESH}, StrId::STR_CAT_CONTROLS),

        // --- System ---
        // Persistence-only flag for the one-time NTP seed on first Wi-Fi connect.
        SettingInfo::Toggle(StrId::STR_CLOCK_SYNCED, &CrossPointSettings::clockHasBeenSynced, "clockHasBeenSynced"),
        // Persistence-only: switched on the Bluetooth screen (Settings > System).
        SettingInfo::Toggle(StrId::STR_BLUETOOTH, &CrossPointSettings::bluetoothEnabled, "bluetoothEnabled"),
    };
    return v;
  }();

  std::vector<SettingInfo> v = baseList;
  if (BoardConfig::hasTouch()) {
    v.erase(std::remove_if(v.begin(), v.end(),
                           [](const SettingInfo& s) { return s.nameId == StrId::STR_SUNLIGHT_FADING_FIX; }),
            v.end());
  }
  return v;
}
