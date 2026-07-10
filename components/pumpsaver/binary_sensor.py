"""PumpSaver binary sensor platform.

Exposes either a "running" state derived from active power, or a diagnostic
``link_ok`` state which becomes false when valid IR words stop arriving.

    binary_sensor:
      - platform: pumpsaver
        name: "Pump Running"
        threshold: 200
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import (
    CONF_THRESHOLD,
    DEVICE_CLASS_CONNECTIVITY,
    DEVICE_CLASS_RUNNING,
    ENTITY_CATEGORY_DIAGNOSTIC,
)

from . import CONF_PUMPSAVER_ID, PumpSaver

DEPENDENCIES = ["pumpsaver"]

CONF_LINK_OK = "link_ok"

RUNNING_SCHEMA = binary_sensor.binary_sensor_schema(
    device_class=DEVICE_CLASS_RUNNING,
).extend(
    {
        cv.GenerateID(CONF_PUMPSAVER_ID): cv.use_id(PumpSaver),
        # Watts of reported power above which the pump counts as running.
        cv.Optional(CONF_THRESHOLD, default=200): cv.int_range(min=1, max=65535),
    }
)

LINK_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_PUMPSAVER_ID): cv.use_id(PumpSaver),
        cv.Required(CONF_LINK_OK): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_CONNECTIVITY,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
    }
)


def _dispatch_schema(config):
    if isinstance(config, dict) and CONF_LINK_OK in config:
        return LINK_SCHEMA(config)
    return RUNNING_SCHEMA(config)


CONFIG_SCHEMA = _dispatch_schema


async def to_code(config):
    hub = await cg.get_variable(config[CONF_PUMPSAVER_ID])
    if CONF_LINK_OK in config:
        var = await binary_sensor.new_binary_sensor(config[CONF_LINK_OK])
        cg.add(hub.set_link_ok_binary_sensor(var))
        return
    var = await binary_sensor.new_binary_sensor(config)
    cg.add(hub.register_running_sensor(var, config[CONF_THRESHOLD]))
