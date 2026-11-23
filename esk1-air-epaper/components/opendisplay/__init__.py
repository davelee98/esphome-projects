"""OpenDisplay e-paper ESL, driven directly over BLE.

The display platform lives in display.py. This package only declares the
namespace and its dependencies.
"""

import esphome.codegen as cg

CODEOWNERS = ["@davelee98"]
DEPENDENCIES = ["ble_client", "esp32_ble_tracker"]

opendisplay_ns = cg.esphome_ns.namespace("opendisplay")
