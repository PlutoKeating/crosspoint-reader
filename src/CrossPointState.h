#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstdint>

class CrossPointState : public PersistableStore<CrossPointState> {
  CrossPointState() = default;

  friend class PersistableStore<CrossPointState>;

 public:
  // False after a power-key sleep: the card stayed on screen, so the next boot
  // resumes without the splash.
  bool showBootScreen = true;

  static const char* getFilePath() { return "/.crosspoint/state.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);
};

// Helper macro to access state
#define APP_STATE CrossPointState::getInstance()
