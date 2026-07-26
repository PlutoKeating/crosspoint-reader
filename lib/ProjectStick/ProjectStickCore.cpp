#include "ProjectStickCore.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

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

bool isUsed(int64_t id, const std::vector<int64_t>& usedIds) {
  return std::find(usedIds.begin(), usedIds.end(), id) != usedIds.end();
}

bool matches(const ScheduleWindow& window, uint16_t minute, bool tradingDay) {
  if (!window.enabled || window.allDay || (window.tradingDayOnly && !tradingDay)) return false;
  if (window.startMinute == window.endMinute) return false;
  if (window.startMinute < window.endMinute) {
    return minute >= window.startMinute && minute < window.endMinute;
  }
  return minute >= window.startMinute || minute < window.endMinute;
}

}  // namespace

bool parseClockMinute(const char* value, uint16_t& minute) {
  if (!value) return false;
  int hour = 0;
  int min = 0;
  if (!parseDigits(value, 0, 2, hour) || value[2] != ':' || !parseDigits(value, 3, 2, min)) return false;
  if (hour > 23 || min > 59) return false;
  minute = static_cast<uint16_t>(hour * 60 + min);
  return true;
}

bool parseIso8601ToShanghai(const char* value, ShanghaiTime& result) {
  result = {};
  if (!value) return false;
  const std::string input(value);
  if (input.size() < 20 || input[4] != '-' || input[7] != '-' || (input[10] != 'T' && input[10] != ' ') ||
      input[13] != ':' || input[16] != ':') {
    return false;
  }

  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (!parseDigits(value, 0, 4, year) || !parseDigits(value, 5, 2, month) || !parseDigits(value, 8, 2, day) ||
      !parseDigits(value, 11, 2, hour) || !parseDigits(value, 14, 2, minute) ||
      !parseDigits(value, 17, 2, second)) {
    return false;
  }
  if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) return false;

  size_t zone = 19;
  if (zone < input.size() && input[zone] == '.') {
    ++zone;
    while (zone < input.size() && std::isdigit(static_cast<unsigned char>(input[zone]))) ++zone;
  }

  int offsetSeconds = 0;
  if (zone < input.size() && (input[zone] == 'Z' || input[zone] == 'z')) {
    ++zone;
  } else if (zone + 5 < input.size() && (input[zone] == '+' || input[zone] == '-')) {
    int offsetHour = 0;
    int offsetMinute = 0;
    if (!parseDigits(value, zone + 1, 2, offsetHour) || input[zone + 3] != ':' ||
        !parseDigits(value, zone + 4, 2, offsetMinute) || offsetHour > 23 || offsetMinute > 59) {
      return false;
    }
    offsetSeconds = (offsetHour * 60 + offsetMinute) * 60;
    if (input[zone] == '-') offsetSeconds = -offsetSeconds;
    zone += 6;
  } else {
    return false;
  }
  if (zone != input.size()) return false;

  int64_t epoch = daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * 86400 +
                  hour * 3600 + minute * 60 + std::min(second, 59) - offsetSeconds;
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

const ScheduleWindow* selectSchedule(const std::vector<ScheduleWindow>& windows, uint16_t minute, bool tradingDay) {
  for (const auto& window : windows) {
    if (matches(window, minute, tradingDay)) return &window;
  }
  for (const auto& window : windows) {
    if (window.enabled && window.allDay) return &window;
  }
  for (const auto& window : windows) {
    if (window.enabled) return &window;
  }
  return nullptr;
}

const ContentCopy* selectCopy(const std::vector<ContentCopy>& copies, const std::vector<int64_t>& usedIds,
                              uint32_t randomValue) {
  uint32_t total = 0;
  for (const auto& copy : copies) {
    if (!isUsed(copy.id, usedIds)) total += std::max<uint16_t>(copy.weight, 1);
  }
  if (total == 0) {
    for (const auto& copy : copies) total += std::max<uint16_t>(copy.weight, 1);
  }
  if (total == 0) return nullptr;

  uint32_t target = randomValue % total;
  const bool allowUsed = std::all_of(copies.begin(), copies.end(), [&usedIds](const ContentCopy& copy) {
    return isUsed(copy.id, usedIds);
  });
  for (const auto& copy : copies) {
    if (!allowUsed && isUsed(copy.id, usedIds)) continue;
    const uint32_t weight = std::max<uint16_t>(copy.weight, 1);
    if (target < weight) return &copy;
    target -= weight;
  }
  return copies.empty() ? nullptr : &copies.front();
}

bool isSafeReleasePath(const std::string& path) {
  if (path.empty() || path.front() == '/' || path.back() == '/' || path.size() > 160) return false;
  size_t segmentStart = 0;
  for (size_t i = 0; i <= path.size(); ++i) {
    if (i == path.size() || path[i] == '/') {
      const std::string segment = path.substr(segmentStart, i - segmentStart);
      if (segment.empty() || segment == "." || segment == "..") return false;
      segmentStart = i + 1;
      continue;
    }
    const unsigned char c = static_cast<unsigned char>(path[i]);
    if (!(std::isalnum(c) || c == '_' || c == '-' || c == '.')) return false;
  }
  return true;
}

std::string stripWrappingQuotes(const std::string& text) {
  const size_t first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const size_t last = text.find_last_not_of(" \t\r\n");
  std::string result = text.substr(first, last - first + 1);

  struct QuotePair {
    const char* opening;
    const char* closing;
  };
  constexpr QuotePair PAIRS[] = {
      {"\"", "\""},
      {"\xE2\x80\x9C", "\xE2\x80\x9D"},  // “ ”
  };
  for (const auto& pair : PAIRS) {
    const size_t openingLength = std::char_traits<char>::length(pair.opening);
    const size_t closingLength = std::char_traits<char>::length(pair.closing);
    if (result.size() >= openingLength + closingLength && result.compare(0, openingLength, pair.opening) == 0 &&
        result.compare(result.size() - closingLength, closingLength, pair.closing) == 0) {
      return result.substr(openingLength, result.size() - openingLength - closingLength);
    }
  }
  return result;
}

}  // namespace project_stick
