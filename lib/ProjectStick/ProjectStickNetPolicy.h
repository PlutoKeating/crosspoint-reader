#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>

// Pure decisions behind the device's few HTTPS requests (docs/studio-protocol.md
// "Cloud requests"): whether the heap can afford a TLS handshake, why a request
// failed (shown to the user instead of a spinner), and UI deadlines.
namespace project_stick {

// What a wolfSSL TLS 1.3 handshake plus record buffers needs on the C3. The
// handshake is known to work at the ~45-50 KB free a busy session leaves when
// NimBLE is not initialised; below these marks the request is sent with the
// BLE stack released (deinitialised) to lend its heap.
constexpr uint32_t TLS_MIN_FREE_HEAP = 56 * 1024;
constexpr uint32_t TLS_MIN_MAX_ALLOC = 24 * 1024;

inline bool tlsHeapSufficient(uint32_t freeHeap, uint32_t maxAlloc) {
  return freeHeap >= TLS_MIN_FREE_HEAP && maxAlloc >= TLS_MIN_MAX_ALLOC;
}

// Hard floor, checked after NimBLE has been released: below it a handshake
// cannot complete (the 2.2.2 crash log shows three 40 s attempts failing at
// 23-26 KB free / 17-23 KB largest block) and each attempt only fragments the
// heap further. Such a request is skipped and reported as a memory failure.
constexpr uint32_t TLS_HARD_MIN_FREE_HEAP = 32 * 1024;
constexpr uint32_t TLS_HARD_MIN_MAX_ALLOC = 16 * 1024;

inline bool tlsHeapAffordable(uint32_t freeHeap, uint32_t maxAlloc) {
  return freeHeap >= TLS_HARD_MIN_FREE_HEAP && maxAlloc >= TLS_HARD_MIN_MAX_ALLOC;
}

enum class NetFailure : uint8_t {
  None,
  Clock,        // no trusted time (NTP unreachable): certificates cannot be checked
  Network,      // DNS / TCP / TLS / read failed or timed out
  Memory,       // the heap stayed below the TLS floor with NimBLE released
  RateLimited,  // the server answered 429 / Retry-After and its window is open
  Backoff,      // the device's own error backoff held a background request (never shown as "busy")
  Server,       // any other HTTP error status
  Timeout,      // the UI deadline passed before the worker answered
};

// Classifies one request outcome. `status` is the HTTP status, <= 0 for a
// transport failure; `heapLow` is true only when the transport failed with the
// heap below the TLS marks although NimBLE had been released (with the stack
// still up a failure is a network failure: the caller retries without it).
inline NetFailure classifyRequest(bool clockReady, int status, bool heapLow) {
  if (!clockReady) return NetFailure::Clock;
  if (status <= 0) return heapLow ? NetFailure::Memory : NetFailure::Network;
  if (status == 429) return NetFailure::RateLimited;
  if (status >= 200 && status < 300) return NetFailure::None;
  return NetFailure::Server;
}

// Where a request stopped (2.7.9): shown on the firmware screen, written to
// /.crosspoint/net_last.txt and relayed in STATE (`net_stage`, `net_code`).
enum class NetStage : uint8_t {
  None = 0,
  Wifi = 1,        // not associated / no IP
  Clock = 2,       // no trusted time, so certificates cannot be checked
  Dns = 3,         // host name did not resolve (router and fallback resolvers)
  Tcp = 4,         // TCP connect failed (code: errno)
  Tls = 5,         // TLS setup or handshake failed (code: wolfSSL error)
  TlsTimeout = 6,  // handshake stalled until the deadline (code: last wolfSSL error)
  Read = 7,        // connected, but the response did not arrive or was cut
  Http = 8,        // the server answered with an error status (code: HTTP status)
  Memory = 9,      // skipped: the heap stayed below the TLS floor
  Ok = 10,
};

// Stages reported by freeink::SecureClient::lastStage().
enum class TlsClientStage : uint8_t { None = 0, Dns = 1, Tcp = 2, TlsSetup = 3, Tls = 4, TlsTimeout = 5, Ok = 6 };

struct NetDiag {
  NetStage stage = NetStage::None;
  int code = 0;
  uint32_t freeHeap = 0;
  uint32_t maxAlloc = 0;
  bool clock = false;
  int8_t rssi = 0;
  uint32_t ipv4 = 0;  // address used (network byte order as IPAddress stores it), 0 if none
};

// Folds one request outcome into a stage. `status` is the HTTP status, <= 0
// when no status line arrived.
inline NetDiag diagnoseRequest(bool wifiUp, bool clockReady, TlsClientStage client, int clientError, int status) {
  NetDiag d;
  d.clock = clockReady;
  if (status >= 200 && status < 300) {
    d.stage = NetStage::Ok;
  } else if (status > 0) {
    d.stage = NetStage::Http;
    d.code = status;
  } else if (!wifiUp) {
    d.stage = NetStage::Wifi;
  } else if (!clockReady) {
    d.stage = NetStage::Clock;
  } else {
    switch (client) {
      case TlsClientStage::Dns:
        d.stage = NetStage::Dns;
        break;
      case TlsClientStage::Tcp:
        d.stage = NetStage::Tcp;
        d.code = clientError;
        break;
      case TlsClientStage::TlsSetup:
      case TlsClientStage::Tls:
        d.stage = NetStage::Tls;
        d.code = clientError;
        break;
      case TlsClientStage::TlsTimeout:
        d.stage = NetStage::TlsTimeout;
        d.code = clientError;
        break;
      case TlsClientStage::Ok:
        d.stage = NetStage::Read;
        break;
      case TlsClientStage::None:
      default:
        d.stage = NetStage::Tcp;
        break;
    }
  }
  return d;
}

inline const char* netStageName(NetStage stage) {
  switch (stage) {
    case NetStage::Wifi: return "wifi";
    case NetStage::Clock: return "clock";
    case NetStage::Dns: return "dns";
    case NetStage::Tcp: return "tcp";
    case NetStage::Tls: return "tls";
    case NetStage::TlsTimeout: return "tls_timeout";
    case NetStage::Read: return "read";
    case NetStage::Http: return "http";
    case NetStage::Memory: return "memory";
    case NetStage::Ok: return "ok";
    case NetStage::None: default: return "none";
  }
}

// One line for /.crosspoint/net_last.txt.
inline int formatNetDiagLine(char* out, size_t size, const NetDiag& d, const char* what, const char* firmware,
                             long long utc, uint32_t uptimeMs) {
  const uint8_t* ip = reinterpret_cast<const uint8_t*>(&d.ipv4);
  return snprintf(out, size,
                  "fw=%s what=%s stage=%s code=%d heap=%u max=%u clock=%d rssi=%d ip=%u.%u.%u.%u utc=%lld "
                  "uptime_ms=%u\n",
                  firmware, what, netStageName(d.stage), d.code, static_cast<unsigned>(d.freeHeap),
                  static_cast<unsigned>(d.maxAlloc), d.clock ? 1 : 0, static_cast<int>(d.rssi), ip[0], ip[1], ip[2],
                  ip[3], utc, static_cast<unsigned>(uptimeMs));
}

// Reads {"v":"x.y.z"} (the device update check's whole answer) without a JSON
// document: finds "v", then the quoted value. False when absent, empty, too
// long or containing anything but digits, dots, letters and '-'.
inline bool parseVersionAnswer(const char* body, char* out, size_t size) {
  if (!body || size < 2) return false;
  const char* key = body;
  for (;; ++key) {
    key = strstr(key, "\"v\"");
    if (!key) return false;
    const char* p = key + 3;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p != ':') continue;
    ++p;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p != '"') return false;
    ++p;
    size_t n = 0;
    for (; p[n] && p[n] != '"'; ++n) {
      const char c = p[n];
      const bool ok = (c >= '0' && c <= '9') || c == '.' || c == '-' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
      if (!ok || n + 1 >= size) return false;
      out[n] = c;
    }
    if (p[n] != '"' || n == 0) return false;
    out[n] = 0;
    return true;
  }
}

// A one-shot deadline on the millis() clock (wrap-safe).
class Deadline {
 public:
  void start(uint32_t nowMs, uint32_t durationMs) {
    startedMs_ = nowMs;
    durationMs_ = durationMs;
    active_ = true;
  }
  void stop() { active_ = false; }
  bool active() const { return active_; }
  bool expired(uint32_t nowMs) const { return active_ && nowMs - startedMs_ >= durationMs_; }

 private:
  uint32_t startedMs_ = 0;
  uint32_t durationMs_ = 0;
  bool active_ = false;
};

// How long the UI waits for the worker: a firmware check (queueing behind a
// running job included).
constexpr uint32_t FIRMWARE_CHECK_DEADLINE_MS = 60000;

}  // namespace project_stick
