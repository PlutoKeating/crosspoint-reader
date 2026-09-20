#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace studio {
struct Window {
  std::string id, scene, start, end, repeat;
  int priority = 0;
  bool enabled = true;
  std::vector<int> weekdays;
  std::vector<std::string> dates;
};
struct Scene {
  std::string id;
  std::vector<int> cards;
};
struct Program {
  std::string defaultScene, alertScene, mode, calendarFrom, calendarUntil;
  bool random = false, alertInterrupts = false;
  int interval = 300, keyguard = 20;
  std::vector<Window> windows;
  std::vector<std::string> tradingDays, cardIds;
  std::vector<Scene> scenes;
};
struct Playback {
  int signal = 0;
  int64_t lastKey = 0, lastTime = 0, lastChange = 0, manualUntil = -1;
  std::string scene, lastCard;
  int index = 0;
  bool manual = false;
};
int minute(const std::string& value);
bool active(const Window&, int64_t, const Program&);
int64_t boundary(const Program&, int64_t);
std::vector<int> order(const Scene&, int64_t, bool);
// Epoch seconds; unlike the UI's milliseconds this fits the embedded clock directly.
int step(const Program&, Playback&, int64_t, int event = 0, int64_t alertUntil = 0);
}  // namespace studio
