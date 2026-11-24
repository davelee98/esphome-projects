"""OpenDisplay e-paper ESL, driven directly over BLE.

The display platform lives in display.py: BLE transport (od_session),
config parsing (od_config), the frame buffer and the four inks (ink.h). A page
component such as aqi_dashboard draws through its public hooks.
"""

import esphome.codegen as cg

CODEOWNERS = ["@davelee98"]
DEPENDENCIES = ["ble_client", "esp32_ble_tracker"]

opendisplay_ns = cg.esphome_ns.namespace("opendisplay")
