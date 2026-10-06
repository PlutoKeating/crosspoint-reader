#include "HalTiltSensor.h"

#include <Logging.h>

HalTiltSensor halTiltSensor;  // Singleton instance

void HalTiltSensor::begin() {
  _available = _sdkImu.begin();
  if (!_available) {
    LOG_ERR("GYR", "SDK IMU not found");
    return;
  }
  // begin() leaves the sensors sampling; nothing reads them, so stand them by.
  if (!_sdkImu.sleep()) LOG_ERR("GYR", "IMU standby failed");
  LOG_INF("GYR", "SDK IMU initialized (standby)");
}

bool HalTiltSensor::deepSleep() {
  if (!_available) return false;
  if (!_sdkImu.sleep()) {
    LOG_ERR("GYR", "IMU sleep failed");
    return false;
  }
  return true;
}
