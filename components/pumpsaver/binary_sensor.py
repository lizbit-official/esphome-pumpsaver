"""PumpSaver binary sensor platform.

Exposes a "running" state derived from the device's reported active power
(register 0x10). The PumpSaver itself draws ~26 W; a running pump reports
several hundred watts, so the default 200 W threshold separates the two
cleanly.

    binary_sensor:
      - platform: pumpsaver
        name: "Pump Running"
        threshold: 200
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import CONF_THRESHOLD, DEVICE_CLASS_RUNNING

from . import CONF_PUMPSAVER_ID, PumpSaver

DEPENDENCIES = ["pumpsaver"]

CONFIG_SCHEMA = binary_sensor.binary_sensor_schema(
    device_class=DEVICE_CLASS_RUNNING,
).extend(
    {
        cv.GenerateID(CONF_PUMPSAVER_ID): cv.use_id(PumpSaver),
        # Watts of reported power above which the pump counts as running.
        cv.Optional(CONF_THRESHOLD, default=200): cv.int_range(min=1, max=65535),
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_PUMPSAVER_ID])
    var = await binary_sensor.new_binary_sensor(config)
    cg.add(hub.register_running_sensor(var, config[CONF_THRESHOLD]))
