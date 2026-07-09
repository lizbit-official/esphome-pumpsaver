"""PumpSaver text sensor platform.

    text_sensor:
      - platform: pumpsaver
        last_fault:
          name: "Pump Last Fault"

Publishes the newest fault-history record, e.g.
"dry well / underload - 774 W, 241.7 V, 5.70 A @ 22d 14h 52m".
Publishes once after boot and again whenever the device logs a new fault.
Code names: 1 (dry-well/underload) is proven; 2-4 follow the family's
documented fault-class ordering and carry a '?' until confirmed.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor

from . import CONF_PUMPSAVER_ID, PumpSaver

DEPENDENCIES = ["pumpsaver"]

CONF_LAST_FAULT = "last_fault"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_PUMPSAVER_ID): cv.use_id(PumpSaver),
        cv.Required(CONF_LAST_FAULT): text_sensor.text_sensor_schema(
            icon="mdi:alert-circle-outline",
        ),
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_PUMPSAVER_ID])
    var = await text_sensor.new_text_sensor(config[CONF_LAST_FAULT])
    cg.add(hub.set_last_fault_text_sensor(var))
