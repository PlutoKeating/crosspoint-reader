#pragma once

#include <cstdint>
#include <string>

namespace project_stick {

struct ShanghaiTime {
  int64_t day = 0;
  uint32_t secondOfDay = 0;
  bool valid = false;

  uint16_t minuteOfDay() const { return static_cast<uint16_t>(secondOfDay / 60); }
};

bool parseIso8601ToShanghai(const char* value, ShanghaiTime& result);
ShanghaiTime advanceTime(const ShanghaiTime& base, uint32_t elapsedSeconds);
std::string formatIso8601Shanghai(const ShanghaiTime& time);

}  // namespace project_stick
