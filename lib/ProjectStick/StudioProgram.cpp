#include "StudioProgram.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
namespace studio {
namespace {
std::tm local(int64_t now) {
  time_t t = now + 28800;
  std::tm result{};
  gmtime_r(&t, &result);
  return result;
}
std::string date(int64_t now) {
  auto t = local(now);
  char s[11];
  strftime(s, sizeof(s), "%Y-%m-%d", &t);
  return s;
}
template <class T>
bool has(const std::vector<T>& items, const T& value) {
  return std::find(items.begin(), items.end(), value) != items.end();
}
}  // namespace
int minute(const std::string& v) {
  if (v.size() != 5 || v[2] != ':' || v[0] < '0' || v[0] > '2' || v[1] < '0' || v[1] > '9' || v[3] < '0' ||
      v[3] > '5' || v[4] < '0' || v[4] > '9')
    return -1;
  const int n = (v[0] - '0') * 600 + (v[1] - '0') * 60 + (v[3] - '0') * 10 + v[4] - '0';
  return n <= 1440 ? n : -1;
}
bool active(const Window& w, int64_t now, const Program& p) {
  if (!w.enabled) return false;
  auto t = local(now);
  const int start = minute(w.start), end = minute(w.end), m = t.tm_hour * 60 + t.tm_min;
  if (start < 0 || end < 0 || start == end) return false;
  if (end < start) {
    if (m >= end && m < start) return false;
    if (m < end) now -= 86400;
  } else if (m < start || m >= end)
    return false;
  t = local(now);
  if (w.repeat == "weekdays") return t.tm_wday >= 1 && t.tm_wday <= 5;
  if (w.repeat == "weekdays_selected") return has(w.weekdays, t.tm_wday);
  if (w.repeat == "dates") return has(w.dates, date(now));
  if (w.repeat == "trading") {
    const auto d = date(now);
    return d >= p.calendarFrom && d <= p.calendarUntil && has(p.tradingDays, d);
  }
  return w.repeat == "daily";
}
int64_t boundary(const Program& p, int64_t now) {
  const int64_t start = (now + 28800) / 86400 * 86400 - 28800;
  int64_t result = -1;
  for (int day = 0; day <= 367; ++day)
    for (const auto& w : p.windows)
      if (w.enabled)
        for (int m : {minute(w.start), minute(w.end)}) {
          const int64_t at = start + day * 86400LL + m * 60;
          if (m < 0 || at <= now || (result >= 0 && at >= result)) continue;
          if (active(w, at - 1, p) != active(w, at, p)) result = at;
        }
  return result;
}
std::vector<int> order(const Scene& s, int64_t now, bool random) {
  auto a = s.cards;
  if (!random) return a;
  uint32_t seed = 2166136261u;
  for (char c : s.id + "|" + date(now)) {
    seed ^= static_cast<uint8_t>(c);
    seed *= 16777619u;
  }
  for (size_t i = a.size(); i > 1; --i) {
    seed = seed * 1664525u + 1013904223u;
    std::swap(a[i - 1], a[seed % i]);
  }
  return a;
}
int step(const Program& p, Playback& s, int64_t timestamp, int event, int64_t alertUntil) {
  const int64_t now = std::max(timestamp, s.lastTime);
  s.lastTime = now;
  s.signal = 0;
  if (event) {
    const bool locked = p.mode == "portable" && p.keyguard > 0 && now - s.lastKey > p.keyguard;
    s.lastKey = now;
    s.signal = locked ? 1 : event == 1 ? 2 : 3;
    if (locked) {
      auto it = std::find(p.cardIds.begin(), p.cardIds.end(), s.lastCard);
      return it == p.cardIds.end() ? -1 : static_cast<int>(it - p.cardIds.begin());
    }
  }
  const bool alert = alertUntil > now && !p.alertScene.empty(),
             manual = s.manual && !s.lastCard.empty() && (s.manualUntil < 0 || s.manualUntil > now);
  const Scene* selected = nullptr;
  std::string id;
  if (alert && (p.alertInterrupts || !manual)) {
    for (const auto& scene : p.scenes)
      if (scene.id == p.alertScene) {
        auto a = order(scene, now, p.random);
        return a.empty() ? -1 : a[(now / p.interval) % a.size()];
      }
    return -1;
  }
  if (manual && event != 1) {
    auto it = std::find(p.cardIds.begin(), p.cardIds.end(), s.lastCard);
    return it == p.cardIds.end() ? -1 : static_cast<int>(it - p.cardIds.begin());
  }
  if (manual)
    id = s.scene;
  else {
    int priority = -1;
    bool conflict = false;
    for (const auto& w : p.windows)
      if (active(w, now, p)) {
        if (w.priority > priority) {
          id = w.scene;
          priority = w.priority;
          conflict = false;
        } else if (w.priority == priority)
          conflict = true;
      }
    if (conflict) return -1;
    if (id.empty()) id = p.defaultScene;
  }
  for (const auto& scene : p.scenes)
    if (scene.id == id) {
      selected = &scene;
      break;
    }
  if (!selected || selected->cards.empty()) return -1;
  const auto a = order(*selected, now, p.random);
  if (s.scene != id) {
    s.index = 0;
    s.lastChange = now;
    s.manual = false;
  } else if (event == 1) {
    s.index = (s.index + 1) % a.size();
    s.lastChange = now;
    s.manual = true;
    s.manualUntil = boundary(p, now);
  } else if (event == 2) {
    s.lastChange = now;
    s.manual = true;
    s.manualUntil = boundary(p, now);
  } else if (!s.manual || (s.manualUntil >= 0 && now >= s.manualUntil)) {
    const int64_t ticks = (now - s.lastChange) / p.interval;
    if (ticks > 0) {
      s.index = (s.index + ticks) % a.size();
      s.lastChange += ticks * p.interval;
    }
    s.manual = false;
  }
  s.scene = id;
  const int frame = a[s.index % a.size()];
  s.lastCard = p.cardIds[frame];
  return frame;
}
}  // namespace studio
