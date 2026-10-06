#pragma once
// Connection-parameter policy for content transfers (2.7.10, BLE-TRANSFER-V4
// §3). After the first PROGRESS of a transfer the device asks ONE conservative
// connection update (15-30 ms, latency 0, 6 s supervision). A peer whose link
// drops within DROP_WINDOW_MS of that request is not asked again until reboot:
// some phones tolerate the default interval but not a change mid-burst.
// Pure and allocation-free, so host tests cover it.
#include <cstdint>
#include <cstring>

namespace link_tuning {

// Units of 1.25 ms / 10 ms (Bluetooth Core): 15-30 ms, 6 s.
constexpr uint16_t MIN_INTERVAL = 12, MAX_INTERVAL = 24, LATENCY = 0, SUPERVISION_TIMEOUT = 600;
constexpr uint32_t DROP_WINDOW_MS = 10000;

class Policy {
 public:
  static constexpr int BLOCKED_PEERS = 4;

  // Whether this connection may still ask for the update.
  bool shouldRequest(const uint8_t peer[6]) const {
    if (askedThisLink_) return false;
    for (int i = 0; i < blockedCount_; ++i)
      if (memcmp(blocked_[i], peer, 6) == 0) return false;
    return true;
  }
  void linkUp() {
    askedThisLink_ = false;
    requestedAtMs_ = 0;
  }
  void requested(uint32_t nowMs) {
    askedThisLink_ = true;
    requestedAtMs_ = nowMs ? nowMs : 1;
  }
  // Returns true when the drop counts against the peer (it is now blocked).
  bool linkDown(const uint8_t peer[6], uint32_t nowMs) {
    const bool blame = requestedAtMs_ && nowMs - requestedAtMs_ < DROP_WINDOW_MS;
    requestedAtMs_ = 0;
    askedThisLink_ = false;
    if (!blame || isBlocked(peer)) return blame;
    memcpy(blocked_[next_], peer, 6);
    next_ = (next_ + 1) % BLOCKED_PEERS;
    if (blockedCount_ < BLOCKED_PEERS) ++blockedCount_;
    return true;
  }
  bool isBlocked(const uint8_t peer[6]) const {
    for (int i = 0; i < blockedCount_; ++i)
      if (memcmp(blocked_[i], peer, 6) == 0) return true;
    return false;
  }

 private:
  uint8_t blocked_[BLOCKED_PEERS][6]{};
  int blockedCount_ = 0, next_ = 0;
  bool askedThisLink_ = false;
  uint32_t requestedAtMs_ = 0;
};

}  // namespace link_tuning
