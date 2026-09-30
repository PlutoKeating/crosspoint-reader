#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>

// Shared backoff for the few cloud requests a device still makes (register
// heartbeat, trading-hours alerts, events, firmware check/download; see
// docs/studio-protocol.md "Cloud requests"). Every request costs database
// queries on the server, so an overloaded or rate-limiting server is left
// alone instead of being retried hot.
namespace project_stick {

// Retry-After in delta-seconds form; HTTP-date and malformed values yield 0
// (the caller then uses its own backoff).
inline uint32_t parseRetryAfterSeconds(const char* value) {
  if (value == nullptr) return 0;
  while (*value == ' ') ++value;
  if (*value < '0' || *value > '9') return 0;
  char* end = nullptr;
  const unsigned long seconds = std::strtoul(value, &end, 10);
  while (end && *end == ' ') ++end;
  if (end == nullptr || *end != '\0') return 0;
  return static_cast<uint32_t>(std::min<unsigned long>(seconds, 86400UL));
}

// True for outcomes that mean "the server is overloaded or unreachable":
// transport failures, 429 and 5xx. Other 4xx are real answers.
inline bool isBackoffStatus(int status) { return status <= 0 || status == 429 || status >= 500; }

// Shared backoff gate for every cloud request. Exponential 30 s, 60 s, ...
// capped at 10 min; a 429 honours Retry-After (capped the same way).
class ApiBackoff {
 public:
  static constexpr uint32_t BASE_SECONDS = 30;
  static constexpr uint32_t MAX_SECONDS = 600;

  void success() {
    failures_ = 0;
    active_ = false;
    rateLimited_ = false;
  }

  // Records a failed request; returns the delay in seconds.
  uint32_t failure(uint32_t nowMs, int status, uint32_t retryAfterSeconds = 0) {
    uint32_t seconds;
    if (status == 429 && retryAfterSeconds > 0) {
      seconds = std::min(retryAfterSeconds, MAX_SECONDS);
    } else {
      const uint8_t shift = std::min<uint8_t>(failures_, 5);
      seconds = std::min<uint32_t>(BASE_SECONDS << shift, MAX_SECONDS);
    }
    if (failures_ < 255) ++failures_;
    rateLimited_ = status == 429;
    active_ = true;
    untilMs_ = nowMs + seconds * 1000UL;
    return seconds;
  }

  void record(uint32_t nowMs, int status, uint32_t retryAfterSeconds = 0) {
    if (isBackoffStatus(status))
      failure(nowMs, status, retryAfterSeconds);
    else
      success();
  }

  bool blocked(uint32_t nowMs) const { return active_ && static_cast<int32_t>(nowMs - untilMs_) < 0; }
  // The server asked the device to slow down (429) and the window is open.
  bool rateLimited(uint32_t nowMs) const { return rateLimited_ && blocked(nowMs); }
  // A manual sync may skip an error backoff, never a server rate limit.
  bool clearForManual(uint32_t nowMs) {
    if (rateLimited(nowMs)) return false;
    active_ = false;
    return true;
  }
  uint8_t failures() const { return failures_; }

 private:
  uint8_t failures_ = 0;
  bool active_ = false;
  bool rateLimited_ = false;
  uint32_t untilMs_ = 0;
};

}  // namespace project_stick
