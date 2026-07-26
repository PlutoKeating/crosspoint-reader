#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace project_stick {

struct ScheduleWindow {
  std::string scenario;
  uint16_t startMinute = 0;
  uint16_t endMinute = 0;
  bool tradingDayOnly = false;
  bool allDay = false;
  bool enabled = true;
};

struct ContentCopy {
  int64_t id = 0;
  std::string text;
  std::string tone;
  uint16_t weight = 1;
};

struct ShanghaiTime {
  int64_t day = 0;
  uint32_t secondOfDay = 0;
  bool valid = false;

  uint16_t minuteOfDay() const { return static_cast<uint16_t>(secondOfDay / 60); }
};

bool parseClockMinute(const char* value, uint16_t& minute);
bool parseIso8601ToShanghai(const char* value, ShanghaiTime& result);
ShanghaiTime advanceTime(const ShanghaiTime& base, uint32_t elapsedSeconds);
std::string formatIso8601Shanghai(const ShanghaiTime& time);
const ScheduleWindow* selectSchedule(const std::vector<ScheduleWindow>& windows, uint16_t minute, bool tradingDay);
const ContentCopy* selectCopy(const std::vector<ContentCopy>& copies, const std::vector<int64_t>& usedIds,
                              uint32_t randomValue);
bool isSafeReleasePath(const std::string& path);

}  // namespace project_stick
