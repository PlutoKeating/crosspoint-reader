#pragma once

#include <Arduino.h>
#include <Imu.h>

class HalTiltSensor;
extern HalTiltSensor halTiltSensor;  // Singleton

// The X3's IMU has no StockStick use (the reader-era tilt page turn is gone);
// the HAL only finds it at boot and keeps it in standby so it does not drain
// the battery, and powers it down for deep sleep.
class HalTiltSensor {
  bool _available = false;
  mutable Imu _sdkImu;

 public:
  // Call after BoardConfig has selected the active device.
  void begin();

  // Puts the IMU into its lowest-power state.
  bool deepSleep();

  // True if an IMU is present on this device
  bool isAvailable() const { return _available; }
};
