#pragma once
#include <cstdint>
#include <string>

namespace studio_ble {
// No Wi-Fi credentials or device cloud bearer is exposed through the service.
void configure(const std::string& deviceId, const std::string& secret, uint32_t epoch, const std::string& owner = "");
void revoke();
void pause(bool paused);
void begin();
void tick();
bool connected();
}  // namespace studio_ble
