import esphome.codegen as cg
from esphome.components import select
from esphome.components import time as time_
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_INITIAL_OPTION,
    CONF_RESTORE_VALUE,
    CONF_TIME_ID,
    ENTITY_CATEGORY_CONFIG,
)

from . import timezone_select_ns

DEPENDENCIES = ["time"]

CONF_TIMEZONES = "timezones"

TimezoneSelect = timezone_select_ns.class_("TimezoneSelect", select.Select, cg.Component)


def _zone(value):
    """An IANA name (America/New_York) or a POSIX TZ string (EST5EDT,M3.2.0,M11.1.0)."""
    value = cv.string_strict(value)
    time_.validate_tz(value)  # raises cv.Invalid with ESPHome's own message
    return value


def _validate(config):
    # Keep the first of any repeats, so a substitution that is also in the
    # list (see the device YAML) does not need special handling.
    zones = list(dict.fromkeys(config[CONF_TIMEZONES]))
    config[CONF_TIMEZONES] = zones
    initial = config.setdefault(CONF_INITIAL_OPTION, zones[0])
    if initial not in zones:
        raise cv.Invalid(
            f"initial_option '{initial}' is not one of timezones: {', '.join(zones)}",
            path=[CONF_INITIAL_OPTION],
        )
    return config


CONFIG_SCHEMA = cv.All(
    select.select_schema(
        TimezoneSelect, icon="mdi:map-clock", entity_category=ENTITY_CATEGORY_CONFIG
    )
    .extend(
        {
            cv.GenerateID(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),
            cv.Required(CONF_TIMEZONES): cv.All(
                cv.ensure_list(_zone), cv.Length(min=1, max=48)
            ),
            cv.Optional(CONF_INITIAL_OPTION): cv.string_strict,
            cv.Optional(CONF_RESTORE_VALUE, default=True): cv.boolean,
        }
    )
    .extend(cv.COMPONENT_SCHEMA),
    _validate,
)


def _rule_args(rule, types):
    return [
        rule.time_seconds,
        rule.day,
        types[rule.type],
        rule.month,
        rule.week,
        rule.day_of_week,
    ]


async def to_code(config):
    from aioesphomeapi.posix_tz import DSTRuleType, parse_posix_tz

    # Wire values of esphome::time::DSTRuleType.
    types = {
        DSTRuleType.NONE: 0,
        DSTRuleType.MONTH_WEEK_DAY: 1,
        DSTRuleType.JULIAN_NO_LEAP: 2,
        DSTRuleType.DAY_OF_YEAR: 3,
    }

    zones = config[CONF_TIMEZONES]
    var = cg.new_Pvariable(config[CONF_ID])
    await select.register_select(var, config, options=zones)
    await cg.register_component(var, config)

    clock = await cg.get_variable(config[CONF_TIME_ID])
    cg.add(var.set_time(clock))
    cg.add(var.set_initial_index(zones.index(config[CONF_INITIAL_OPTION])))
    cg.add(var.set_restore_value(config[CONF_RESTORE_VALUE]))

    for zone in zones:
        posix = time_.validate_tz(zone)  # IANA name -> POSIX TZ string
        parsed = parse_posix_tz(posix)
        cg.add(
            var.add_zone(
                posix,
                parsed.std_offset_seconds,
                parsed.dst_offset_seconds,
                *_rule_args(parsed.dst_start, types),
                *_rule_args(parsed.dst_end, types),
            )
        )

    # time::set_global_tz() is only compiled with a timezone in use.
    cg.add_define("USE_TIME_TIMEZONE")
