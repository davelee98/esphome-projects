import esphome.codegen as cg
from esphome.components import ble_client, display
import esphome.config_validation as cv
from esphome.const import CONF_HEIGHT, CONF_ID, CONF_LAMBDA, CONF_PAGES, CONF_WIDTH

from . import opendisplay_ns

DEPENDENCIES = ["ble_client"]

CONF_COLOR_SCHEME = "color_scheme"
CONF_REFRESH_MODE = "refresh_mode"
CONF_SKIP_UNCHANGED = "skip_unchanged"
CONF_COMPRESSION = "compression"

OpenDisplay = opendisplay_ns.class_(
    "OpenDisplay",
    display.DisplayBuffer,
    cg.PollingComponent,
    ble_client.BLEClientNode,
)

ColorScheme = opendisplay_ns.enum("ColorScheme")
# Wire values of the firmware's color_scheme field. Only the schemes this
# component can encode are offered.
COLOR_SCHEMES = {
    "MONO": ColorScheme.COLOR_SCHEME_MONO,
    "BWR": ColorScheme.COLOR_SCHEME_BWR,
    "BWY": ColorScheme.COLOR_SCHEME_BWY,
}

RefreshMode = opendisplay_ns.enum("RefreshMode")
REFRESH_MODES = {
    "FULL": RefreshMode.REFRESH_MODE_FULL,
    "FAST": RefreshMode.REFRESH_MODE_FAST,
}

CONFIG_SCHEMA = cv.All(
    display.FULL_DISPLAY_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(OpenDisplay),
            # The panel's native size, before any rotation. The component
            # checks it against the device's own config on every connection.
            cv.Required(CONF_WIDTH): cv.int_range(min=8, max=2048),
            cv.Required(CONF_HEIGHT): cv.int_range(min=8, max=2048),
            cv.Optional(CONF_COLOR_SCHEME, default="MONO"): cv.enum(
                COLOR_SCHEMES, upper=True
            ),
            cv.Optional(CONF_REFRESH_MODE, default="FULL"): cv.enum(
                REFRESH_MODES, upper=True
            ),
            # An e-paper refresh costs the label battery; by default an
            # unchanged frame is not sent again.
            cv.Optional(CONF_SKIP_UNCHANGED, default=True): cv.boolean,
            # zlib-compress uploads when the label advertises a decompressor;
            # falls back to uncompressed if the label refuses.
            cv.Optional(CONF_COMPRESSION, default=True): cv.boolean,
        }
    )
    .extend(cv.polling_component_schema("10min"))
    .extend(ble_client.BLE_CLIENT_SCHEMA),
    cv.has_at_most_one_key(CONF_PAGES, CONF_LAMBDA),
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await display.register_display(var, config)
    await ble_client.register_ble_node(var, config)

    cg.add(var.set_panel_size(config[CONF_WIDTH], config[CONF_HEIGHT]))
    cg.add(var.set_color_scheme(config[CONF_COLOR_SCHEME]))
    cg.add(var.set_refresh_mode(config[CONF_REFRESH_MODE]))
    cg.add(var.set_skip_unchanged(config[CONF_SKIP_UNCHANGED]))
    cg.add(var.set_compression(config[CONF_COMPRESSION]))

    if CONF_LAMBDA in config:
        lambda_ = await cg.process_lambda(
            config[CONF_LAMBDA], [(display.DisplayRef, "it")], return_type=cg.void
        )
        cg.add(var.set_writer(lambda_))
