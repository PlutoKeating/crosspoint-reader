#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>

// Request cadence for the cloud API (docs/studio-protocol.md "Poll cadence").
// Every device request costs database queries on the server, whose daily
// budget is shared by all devices and the website simulator, so the device
// polls on a server-provided interval and backs off on overload instead of
// retrying hot.
namespace project_stick {

constexpr uint32_t DEFAULT_STUDIO_POLL_SECONDS = 60;
constexpr uint32_t MIN_STUDIO_POLL_SECONDS = 15;
constexpr uint32_t MAX_STUDIO_POLL_SECONDS = 3600;

// `studio_poll_seconds` from /register or /studio; absent or invalid values
// fall back to the default.
inline uint32_t clampStudioPollSeconds(int64_t raw) {
  if (raw <= 0) return DEFAULT_STUDIO_POLL_SECONDS;
  return static_cast<uint32_t>(
      std::clamp<int64_t>(raw, MIN_STUDIO_POLL_SECONDS, MAX_STUDIO_POLL_SECONDS));
}

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

// When the Studio target is polled: every `interval` seconds, right away after
// pollNow() (coming online, a finished BLE session, a card to report), and a
// short 10 s burst after a manual sync so a publish made moments ago is picked
// up without polling hot all the time.
class StudioPollSchedule {
 public:
  static constexpr uint32_t BURST_INTERVAL_MS = 10000;
  static constexpr uint8_t BURST_POLLS = 3;

  void setIntervalSeconds(uint32_t seconds) { intervalMs_ = clampStudioPollSeconds(seconds) * 1000UL; }
  uint32_t intervalMs() const { return intervalMs_; }
  void pollNow() { immediate_ = true; }
  void startBurst() { burstLeft_ = BURST_POLLS; }
  bool due(uint32_t nowMs) const {
    if (immediate_ || !polledOnce_) return true;
    const uint32_t interval = burstLeft_ > 0 ? BURST_INTERVAL_MS : intervalMs_;
    return nowMs - lastMs_ >= interval;
  }
  // A Studio GET ran (either a poll or a full sync, which includes one).
  void polled(uint32_t nowMs) {
    if (!immediate_ && polledOnce_ && burstLeft_ > 0) --burstLeft_;
    immediate_ = false;
    polledOnce_ = true;
    lastMs_ = nowMs;
  }

 private:
  uint32_t intervalMs_ = DEFAULT_STUDIO_POLL_SECONDS * 1000UL;
  uint32_t lastMs_ = 0;
  uint8_t burstLeft_ = 0;
  bool immediate_ = false;
  bool polledOnce_ = false;
};

}  // namespace project_stick
