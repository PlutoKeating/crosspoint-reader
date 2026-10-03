// Arduino-ESP32 3.3.x defines `_btLibraryInUse` only when one of its built-in
// Bluetooth hosts is enabled, but NimBLE-Arduino's static constructor
// (esp32-hal-bt-mem.h) still sets it in this controller-only build.
// scripts/patch_arduino_bt_controller.py moves the core's definition out of
// that #if (the framework-library scaffold link needs it there); this weak
// fallback covers the application link when the core is unpatched.
#include <stdbool.h>

__attribute__((weak)) bool _btLibraryInUse = false;
