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

// Times before MIN_TRUSTED_YEAR (an RTC reset to 2000-01-01) are rejected.
constexpr int MIN_TRUSTED_YEAR = 2025;
constexpr int MAX_TRUSTED_YEAR = 2099;

bool parseIso8601ToShanghai(const char* value, ShanghaiTime& result);
// UTC calendar fields to Shanghai time without allocating; false for untrusted years.
bool shanghaiFromUtc(int year, int month, int day, int hour, int minute, int second, ShanghaiTime& result);
ShanghaiTime advanceTime(const ShanghaiTime& base, uint32_t elapsedSeconds);
std::string formatIso8601Shanghai(const ShanghaiTime& time);

}  // namespace project_stick
