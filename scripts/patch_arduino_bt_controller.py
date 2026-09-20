"""Arduino 3.3.7: expose the BT-use flag to external hosts in controller-only builds.

The upstream header always references this symbol for BLE-capable SoCs, but the
definition is incorrectly nested under the built-in host selection. Move only
the flag, leaving Arduino's host/controller routines unchanged.
"""
from pathlib import Path

Import("env")
package = env.PioPlatform().get_package_dir("framework-arduinoespressif32")
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
