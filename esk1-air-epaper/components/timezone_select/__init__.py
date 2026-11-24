"""Timezone chosen at run time from a list fixed in the YAML.

ESPHome parses timezones only at compile time (the device has no POSIX TZ
parser), so every zone offered is parsed here and stored ready to apply. The
select platform lives in select.py.
"""

import esphome.codegen as cg

CODEOWNERS = ["@davelee98"]
DEPENDENCIES = ["time"]

timezone_select_ns = cg.esphome_ns.namespace("timezone_select")
