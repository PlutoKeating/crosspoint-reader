#include "ProjectStickCore.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace project_stick {
namespace {

// Howard Hinnant's civil calendar conversion, shifted to the Unix epoch.
int64_t daysFromCivil(int year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int>(doe) - 719468;
}

void civilFromDays(int64_t days, int& year, unsigned& month, unsigned& day) {
  days += 719468;
  const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(days - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  year = static_cast<int>(yoe) + static_cast<int>(era) * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  day = doy - (153 * mp + 2) / 5 + 1;
  month = mp + (mp < 10 ? 3 : -9);
  year += month <= 2;
}

bool parseDigits(const char* value, size_t offset, size_t count, int& out) {
  out = 0;
  for (size_t i = 0; i < count; ++i) {
    const unsigned char c = static_cast<unsigned char>(value[offset + i]);
    if (!std::isdigit(c)) return false;
    out = out * 10 + (c - '0');
  }
  return true;
}

}  // namespace

bool shanghaiFromUtc(int year, int month, int day, int hour, int minute, int second, ShanghaiTime& result) {
  result = {};
  // An RTC that lost power reads 2000-01-01; nothing before 2025 is a real
  // time for this device, so it must never drive a schedule.
  if (year < MIN_TRUSTED_YEAR || year > MAX_TRUSTED_YEAR) return false;
  if (month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
      second < 0 || second > 60) {
    return false;
  }
  int64_t epoch = daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * 86400 + hour * 3600 +
                  minute * 60 + std::min(second, 59);
  epoch += 8 * 3600;  // Protocol scheduling is authoritative in Asia/Shanghai.
  result.day = epoch / 86400;
  int64_t seconds = epoch % 86400;
  if (seconds < 0) {
    seconds += 86400;
    --result.day;
  }
  result.secondOfDay = static_cast<uint32_t>(seconds);
  result.valid = true;
  return true;
}

// Allocation-free: called from the UI loop, where a failed heap allocation
// used to abort the firmware (std::bad_alloc is not caught anywhere).
bool parseIso8601ToShanghai(const char* value, ShanghaiTime& result) {
  result = {};
  if (!value) return false;
  const size_t length = std::strlen(value);
  if (length < 20 || value[4] != '-' || value[7] != '-' || (value[10] != 'T' && value[10] != ' ') ||
      value[13] != ':' || value[16] != ':') {
    return false;
  }

  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (!parseDigits(value, 0, 4, year) || !parseDigits(value, 5, 2, month) || !parseDigits(value, 8, 2, day) ||
      !parseDigits(value, 11, 2, hour) || !parseDigits(value, 14, 2, minute) || !parseDigits(value, 17, 2, second)) {
    return false;
  }

  size_t zone = 19;
  if (zone < length && value[zone] == '.') {
    ++zone;
    while (zone < length && std::isdigit(static_cast<unsigned char>(value[zone]))) ++zone;
  }

  int offsetSeconds = 0;
  if (zone < length && (value[zone] == 'Z' || value[zone] == 'z')) {
    ++zone;
  } else if (zone + 5 < length && (value[zone] == '+' || value[zone] == '-')) {
    int offsetHour = 0;
    int offsetMinute = 0;
    if (!parseDigits(value, zone + 1, 2, offsetHour) || value[zone + 3] != ':' ||
        !parseDigits(value, zone + 4, 2, offsetMinute) || offsetHour > 23 || offsetMinute > 59) {
      return false;
    }
    offsetSeconds = (offsetHour * 60 + offsetMinute) * 60;
    if (value[zone] == '-') offsetSeconds = -offsetSeconds;
    zone += 6;
  } else {
    return false;
  }
  if (zone != length) return false;

  if (!shanghaiFromUtc(year, month, day, hour, minute, second, result)) return false;
  // Apply the zone offset on the already normalised Shanghai time.
  const int64_t total = result.day * 86400 + static_cast<int64_t>(result.secondOfDay) - offsetSeconds;
  result.day = total >= 0 ? total / 86400 : (total - 86399) / 86400;
  result.secondOfDay = static_cast<uint32_t>(total - result.day * 86400);
  return true;
}

ShanghaiTime advanceTime(const ShanghaiTime& base, uint32_t elapsedSeconds) {
  if (!base.valid) return {};
  ShanghaiTime result = base;
  const uint64_t total = static_cast<uint64_t>(base.secondOfDay) + elapsedSeconds;
  result.day += static_cast<int64_t>(total / 86400);
  result.secondOfDay = static_cast<uint32_t>(total % 86400);
  return result;
}

std::string formatIso8601Shanghai(const ShanghaiTime& time) {
  if (!time.valid) return {};
  int year = 0;
  unsigned month = 0;
  unsigned day = 0;
  civilFromDays(time.day, year, month, day);
  const unsigned hour = time.secondOfDay / 3600;
  const unsigned minute = (time.secondOfDay / 60) % 60;
  const unsigned second = time.secondOfDay % 60;
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "%04d-%02u-%02uT%02u:%02u:%02u+08:00", year, month, day, hour, minute, second);
  return buffer;
}

}  // namespace project_stick
