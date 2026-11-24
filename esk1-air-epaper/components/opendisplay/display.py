import esphome.codegen as cg
from esphome.components import ble_client, display, esp32_ble_tracker
import esphome.config_validation as cv
from esphome.const import CONF_HEIGHT, CONF_ID, CONF_LAMBDA, CONF_PAGES, CONF_WIDTH

from . import opendisplay_ns

DEPENDENCIES = ["ble_client", "esp32_ble_tracker"]

CONF_COLOR_SCHEME = "color_scheme"
CONF_REFRESH_MODE = "refresh_mode"
CONF_SKIP_UNCHANGED = "skip_unchanged"
CONF_COMPRESSION = "compression"
CONF_BLE_NAME = "ble_name"

OpenDisplay = opendisplay_ns.class_(
    "OpenDisplay",
    display.DisplayBuffer,
    cg.PollingComponent,
    ble_client.BLEClientNode,
    esp32_ble_tracker.ESPBTDeviceListener,
)

ColorScheme = opendisplay_ns.enum("ColorScheme")
# enum ColorScheme of opendisplay-protocol (the firmware's color_scheme wire
# value), named as there without the OD_COLOR_SCHEME_ prefix. Only the schemes
# this component can encode are offered.
COLOR_SCHEMES = {
    "MONO": ColorScheme.COLOR_SCHEME_MONO,
    "BWR": ColorScheme.COLOR_SCHEME_BWR,
    "BWY": ColorScheme.COLOR_SCHEME_BWY,
    "BWRY": ColorScheme.COLOR_SCHEME_BWRY,
}

# Bits per pixel and plane count of each scheme's frame buffer.
SCHEME_LAYOUT = {
    "MONO": (1, 1),
    "BWR": (1, 2),
    "BWY": (1, 2),
    "BWRY": (2, 1),
}

# The frame buffer reserved at boot: a 400x300 BWRY frame (2 bpp), the largest
# supported. The ESP32-C6 has no PSRAM, and the buffer must fit in internal RAM
# next to Wi-Fi, BLE and the API, with room left for compressing the upload.
# Keep in step with MAX_FRAME_BYTES in opendisplay.h.
MAX_FRAME_BYTES = 400 * 300 * 2 // 8  # 30,000


PANEL_KEYS_MSG = "width, height and color_scheme must be given together, or not at all"


def frame_bytes(width, height, scheme):
    bpp, planes = SCHEME_LAYOUT[scheme]
    return (width * bpp + 7) // 8 * height * planes


def validate_frame_size(config):
    if CONF_WIDTH not in config:
        return config  # the panel is read from the label
    scheme = str(config[CONF_COLOR_SCHEME])
    size = frame_bytes(config[CONF_WIDTH], config[CONF_HEIGHT], scheme)
    if size > MAX_FRAME_BYTES:
        raise cv.Invalid(
            f"A {config[CONF_WIDTH]}x{config[CONF_HEIGHT]} {scheme} frame needs "
            f"{size} bytes; the most this component supports is {MAX_FRAME_BYTES} "
            f"bytes (a 400x300 BWRY frame)"
        )
    return config


RefreshMode = opendisplay_ns.enum("RefreshMode")
REFRESH_MODES = {
    "FULL": RefreshMode.REFRESH_MODE_FULL,
    "FAST": RefreshMode.REFRESH_MODE_FAST,
}

CONFIG_SCHEMA = cv.All(
    display.FULL_DISPLAY_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(OpenDisplay),
            # Optional. The panel's size (native, before any rotation) and
            # colour scheme are read from the label on every connection, and
            # the label's values win. These are only needed for a label with
            # no stored config; given, they must come together.
            cv.Inclusive(CONF_WIDTH, "panel", msg=PANEL_KEYS_MSG): cv.int_range(
                min=8, max=2048
            ),
            cv.Inclusive(CONF_HEIGHT, "panel", msg=PANEL_KEYS_MSG): cv.int_range(
                min=8, max=2048
            ),
            cv.Inclusive(CONF_COLOR_SCHEME, "panel", msg=PANEL_KEYS_MSG): cv.enum(
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
            # Find the label by its advertised BLE name when ble_client's
            # mac_address is 00:00:00:00:00:00 (any other MAC overrides this).
            # A trailing '*' matches a prefix. Can be changed at run time with
            # set_ble_name(), e.g. from a template text entity.
            cv.Optional(CONF_BLE_NAME): cv.All(cv.string_strict, cv.Length(min=1, max=29)),
        }
    )
    .extend(cv.polling_component_schema("10min"))
    .extend(ble_client.BLE_CLIENT_SCHEMA)
    .extend(esp32_ble_tracker.ESP_BLE_DEVICE_SCHEMA),
    cv.has_at_most_one_key(CONF_PAGES, CONF_LAMBDA),
    validate_frame_size,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await display.register_display(var, config)
    await ble_client.register_ble_node(var, config)
    await esp32_ble_tracker.register_ble_device(var, config)

    if CONF_WIDTH in config:
        cg.add(var.set_panel_size(config[CONF_WIDTH], config[CONF_HEIGHT]))
        cg.add(var.set_color_scheme(config[CONF_COLOR_SCHEME]))
    cg.add(var.set_refresh_mode(config[CONF_REFRESH_MODE]))
    cg.add(var.set_skip_unchanged(config[CONF_SKIP_UNCHANGED]))
    cg.add(var.set_compression(config[CONF_COMPRESSION]))
    if CONF_BLE_NAME in config:
        cg.add(var.set_ble_name(config[CONF_BLE_NAME]))

    if CONF_LAMBDA in config:
        lambda_ = await cg.process_lambda(
            config[CONF_LAMBDA], [(display.DisplayRef, "it")], return_type=cg.void
        )
        cg.add(var.set_writer(lambda_))
