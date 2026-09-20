#pragma once
#include <string>
#include <cstdint>

namespace studio_ble {
// No Wi-Fi credentials or device cloud bearer is exposed through the service.
void configure(const std::string& deviceId, const std::string& secret, uint32_t epoch);
void revoke();
void begin();
void tick();
bool connected();
}
