"""Arduino 3.3.7: expose the BT-use flag to external hosts in controller-only builds.

The upstream header always references this symbol for BLE-capable SoCs, but the
definition is incorrectly nested under the built-in host selection. Move only
the flag, leaving Arduino's host/controller routines unchanged.

The patch must be in place before pioarduino's framework-library rebuild (the
custom_sdkconfig scaffold links NimBLE-Arduino against the core without this
project's sources), so the core package has to exist when this pre: script
runs; a missing package is installed here rather than skipped.
"""
from pathlib import Path

Import("env")
platform = env.PioPlatform()
package = platform.get_package_dir("framework-arduinoespressif32")
if not package:
    # Frameworks are installed lazily during the build, i.e. after this pre:
    # script on a clean machine or CI runner; fetch it now so the patch is in
    # place for the scaffold link.
    print("patch_arduino_bt_controller: installing framework-arduinoespressif32 before patching")
    platform.install_package("framework-arduinoespressif32")
    package = platform.get_package_dir("framework-arduinoespressif32")
if not package:
    raise RuntimeError("framework-arduinoespressif32 could not be installed; the BLE host would fail to link")
if package:
    path = Path(package) / "cores/esp32/esp32-hal-bt.c"
    source = path.read_text()
    marker = "// StockStick: external BLE host also needs the Arduino usage flag."
    if marker not in source:
        if source.count("bool _btLibraryInUse = false;") != 1:
            raise RuntimeError("Arduino BT usage flag changed upstream; review the compatibility patch")
        source = source.replace("bool _btLibraryInUse = false;", "")
        source = source.replace("#if SOC_BT_SUPPORTED\n", "#if SOC_BT_SUPPORTED\n" + marker + "\nbool _btLibraryInUse = false;\n", 1)
        path.write_text(source)
