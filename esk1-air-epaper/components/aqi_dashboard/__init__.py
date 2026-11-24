"""AQI dashboard: the fixed eight-cell air-quality page of
docs/aqi-dashboard-specification.md, drawn through an opendisplay display.

    aqi_dashboard:
      display_id: epaper        # an opendisplay display, 400x300 BWRY,
                                # with update_interval: never and no lambda
      time_id: ha_time
      sen6x_id: sen65           # optional: shows a missing / failing sensor
      sensors: {pm_2_5: ..., pm_10_0: ..., pm_1_0: ..., voc_index: ...,
                nox_index: ..., temperature: ..., humidity: ..., co2: ...}

The dashboard owns the display's refresh schedule; the display is only the
BLE transport. timing / deadbands / bands default to the specification.
"""

import esphome.codegen as cg
from esphome.components import sensor
from esphome.components import time as time_
from esphome.components.opendisplay.display import OpenDisplay
import esphome.config_validation as cv
from esphome.const import (
    CONF_DISPLAY_ID,
    CONF_HEIGHT,
    CONF_ID,
    CONF_LAMBDA,
    CONF_PAGES,
    CONF_PLATFORM,
    CONF_SENSORS,
    CONF_TIME_ID,
    CONF_UPDATE_INTERVAL,
    CONF_WIDTH,
    SCHEDULER_DONT_RUN,
)
import esphome.final_validate as fv

CODEOWNERS = ["@davelee98"]
DEPENDENCIES = ["time", "sensor"]

CONF_TITLE = "title"
CONF_SEN6X_ID = "sen6x_id"
CONF_TEMPERATURE_UNIT = "temperature_unit"
CONF_TIMING = "timing"
CONF_DEADBANDS = "deadbands"
CONF_BANDS = "bands"
CONF_COLOR_SCHEME = "color_scheme"

aqi_dashboard_ns = cg.esphome_ns.namespace("aqi_dashboard")
AqiDashboard = aqi_dashboard_ns.class_("AqiDashboard", cg.Component)
Metric = aqi_dashboard_ns.enum("Metric", is_class=True)

# YAML key -> Metric.
DASHBOARD_SENSORS = {
    "pm_2_5": Metric.PM2_5,
    "pm_10_0": Metric.PM10,
    "pm_1_0": Metric.PM1,
    "co2": Metric.CO2,
    "voc_index": Metric.VOC,
    "nox_index": Metric.NOX,
    "temperature": Metric.TEMP,
    "humidity": Metric.HUM,
}
REQUIRED_SENSORS = [k for k in DASHBOARD_SENSORS if k != "co2"]

# Glyphs of the 14 px text font: ASCII 0x20-0x7E plus degree, micro, superscript 3.
TEXT_GLYPHS = {chr(c) for c in range(0x20, 0x7F)} | {"°", "µ", "³"}
TITLE_MAX_GLYPHS = 24

# Event bands (spec section 4.3): (classifier, enabled, enter_yellow,
# enter_red, leave_yellow, leave_red). CO2 is off by default even when bound.
BAND_DEFAULTS = {
    "pm_2_5": (Metric.PM2_5, True, 15, 35, 12, 30),
    "voc": (Metric.VOC, True, 150, 400, 130, 350),
    "nox": (Metric.NOX, True, 20, 300, 15, 250),
    "co2": (Metric.CO2, False, 1000, 1500, 900, 1300),
}


def validate_title(value):
    value = cv.string_strict(value)
    bad = sorted({c for c in value if c not in TEXT_GLYPHS})
    if bad:
        raise cv.Invalid(
            f"title has characters the 14 px font lacks: {''.join(bad)!r} "
            "(use ASCII, plus degree, micro and superscript 3)"
        )
    if len(value) > TITLE_MAX_GLYPHS:
        raise cv.Invalid(f"title is {len(value)} characters; at most {TITLE_MAX_GLYPHS} fit")
    return value


def validate_bands(config):
    ly, ey = config["leave_yellow"], config["enter_yellow"]
    lr, er = config["leave_red"], config["enter_red"]
    if not ly < ey < lr < er:
        raise cv.Invalid(
            "bands need leave_yellow < enter_yellow < leave_red < enter_red, "
            f"got {ly} / {ey} / {lr} / {er}"
        )
    return config


def band_schema(name):
    _, enabled, ey, er, ly, lr = BAND_DEFAULTS[name]
    return cv.All(
        cv.Schema(
            {
                cv.Optional("enabled", default=enabled): cv.boolean,
                cv.Optional("enter_yellow", default=ey): cv.float_,
                cv.Optional("enter_red", default=er): cv.float_,
                cv.Optional("leave_yellow", default=ly): cv.float_,
                cv.Optional("leave_red", default=lr): cv.float_,
            }
        ),
        validate_bands,
    )


# Spec section 5.1 defaults.
TIMING_SCHEMA = cv.Schema(
    {
        cv.Optional("routine", default="30min"): cv.positive_time_period_milliseconds,
        cv.Optional("heartbeat", default="60min"): cv.positive_time_period_milliseconds,
        cv.Optional("guard", default="15min"): cv.positive_time_period_milliseconds,
        cv.Optional("persistence", default="3min"): cv.positive_time_period_milliseconds,
        cv.Optional("stale_after", default="180s"): cv.positive_time_period_milliseconds,
        cv.Optional("startup_wait", default="180s"): cv.positive_time_period_milliseconds,
    }
)

# Spec section 5.2 defaults.
DEADBANDS_SCHEMA = cv.Schema(
    {
        cv.Optional("pm_absolute", default=2): cv.positive_float,
        cv.Optional("pm_relative", default="20%"): cv.percentage,
        cv.Optional("co2", default=100): cv.positive_float,
        cv.Optional("voc", default=25): cv.positive_float,
        cv.Optional("nox", default=10): cv.positive_float,
        cv.Optional("temperature", default=0.5): cv.positive_float,
        cv.Optional("humidity", default=3): cv.positive_float,
    }
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(AqiDashboard),
        cv.Required(CONF_DISPLAY_ID): cv.use_id(OpenDisplay),
        cv.Required(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),
        # The sen6x component itself, so a missing or failing sensor shows as
        # SENSOR MISSING / SENSOR ERROR in the header instead of a page that
        # looks like it is still starting.
        cv.Optional(CONF_SEN6X_ID): cv.use_id(cg.Component),
        cv.Optional(CONF_TITLE, default="INDOOR AIR"): validate_title,
        cv.Optional(CONF_TEMPERATURE_UNIT, default="C"): cv.one_of("C", "F", upper=True),
        cv.Required(CONF_SENSORS): cv.Schema(
            {
                **{cv.Required(k): cv.use_id(sensor.Sensor) for k in REQUIRED_SENSORS},
                cv.Optional("co2"): cv.use_id(sensor.Sensor),
            }
        ),
        cv.Optional(CONF_TIMING, default={}): TIMING_SCHEMA,
        cv.Optional(CONF_DEADBANDS, default={}): DEADBANDS_SCHEMA,
        cv.Optional(CONF_BANDS, default={}): cv.Schema(
            {cv.Optional(name, default={}): band_schema(name) for name in BAND_DEFAULTS}
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


def _final_validate(config):
    """The display this draws on must suit the fixed page and leave the
    refresh schedule to the dashboard."""
    target = config[CONF_DISPLAY_ID]
    displays = fv.full_config.get().get("display", [])
    for index, disp in enumerate(displays):
        if disp.get(CONF_ID) != target:
            continue
        path = ["display", index]
        if disp.get(CONF_PLATFORM) != "opendisplay":
            raise cv.Invalid(f"display '{target}' must be an opendisplay display", path=path)
        if CONF_LAMBDA in disp or CONF_PAGES in disp:
            raise cv.Invalid(
                f"display '{target}' is drawn by aqi_dashboard; remove its lambda / pages", path=path
            )
        if CONF_WIDTH in disp and (
            disp[CONF_WIDTH] != 400 or disp[CONF_HEIGHT] != 300 or str(disp[CONF_COLOR_SCHEME]) != "BWRY"
        ):
            raise cv.Invalid(
                "aqi_dashboard needs a 400x300 BWRY panel; fix width / height / color_scheme "
                f"of display '{target}' or remove them",
                path=path,
            )
        interval = disp.get(CONF_UPDATE_INTERVAL)
        if interval is None or interval.total_milliseconds != SCHEDULER_DONT_RUN:
            raise cv.Invalid(
                f"set update_interval: never on display '{target}'; aqi_dashboard decides when "
                "the label refreshes",
                path=path + [CONF_UPDATE_INTERVAL],
            )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    disp = await cg.get_variable(config[CONF_DISPLAY_ID])
    cg.add(var.set_display(disp))
    clock = await cg.get_variable(config[CONF_TIME_ID])
    cg.add(var.set_time(clock))
    if CONF_SEN6X_ID in config:
        source = await cg.get_variable(config[CONF_SEN6X_ID])
        cg.add(var.set_sensor_component(source))
    cg.add(var.set_title(config[CONF_TITLE]))
    cg.add(var.set_fahrenheit(config[CONF_TEMPERATURE_UNIT] == "F"))
    for key, metric in DASHBOARD_SENSORS.items():
        if key in config[CONF_SENSORS]:
            sens = await cg.get_variable(config[CONF_SENSORS][key])
            cg.add(var.set_sensor(metric, sens))
    t = config[CONF_TIMING]
    cg.add(
        var.set_timing(
            t["routine"].total_milliseconds,
            t["heartbeat"].total_milliseconds,
            t["guard"].total_milliseconds,
            t["persistence"].total_milliseconds,
            t["stale_after"].total_milliseconds,
            t["startup_wait"].total_milliseconds,
        )
    )
    d = config[CONF_DEADBANDS]
    cg.add(
        var.set_deadbands(
            d["pm_absolute"],
            d["pm_relative"],
            d["co2"],
            d["voc"],
            d["nox"],
            d["temperature"],
            d["humidity"],
        )
    )
    for name, (metric, *_unused) in BAND_DEFAULTS.items():
        b = config[CONF_BANDS][name]
        cg.add(
            var.set_bands(
                metric,
                b["enabled"],
                b["enter_yellow"],
                b["enter_red"],
                b["leave_yellow"],
                b["leave_red"],
            )
        )
