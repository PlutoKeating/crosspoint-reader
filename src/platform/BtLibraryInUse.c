// Arduino-ESP32 3.3.x defines `_btLibraryInUse` only when one of its built-in
// Bluetooth hosts is enabled, but NimBLE-Arduino's static constructor
// (esp32-hal-bt-mem.h) still sets it in this controller-only build. Weak, so
// a core that does define the flag keeps its own.
#include <stdbool.h>

__attribute__((weak)) bool _btLibraryInUse = false;
