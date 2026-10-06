#include "CrossPointState.h"

void CrossPointState::toJson(JsonDocument& doc) const { doc["showBootScreen"] = showBootScreen; }

bool CrossPointState::fromJson(JsonVariantConst doc) {
  // The recent-wallpaper keys of the removed custom sleep screens are ignored.
  showBootScreen = doc["showBootScreen"] | true;
  return true;
}
