"""PumpSaver sensor platform.

Two forms:

1. Named telemetry sensors (any subset):

    sensor:
      - platform: pumpsaver
        voltage:
          name: "Pump Voltage"
        current:
          name: "Pump Current"

2. Advanced: expose any raw 16-bit register value:

    sensor:
      - platform: pumpsaver
        register: 0x02
        name: "Calibration Voltage x10"
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    CONF_CURRENT,
    ENTITY_CATEGORY_DIAGNOSTIC,
    CONF_POWER,
    CONF_POWER_FACTOR,
    CONF_VOLTAGE,
    DEVICE_CLASS_CURRENT,
    DEVICE_CLASS_DURATION,
    DEVICE_CLASS_POWER,
    DEVICE_CLASS_POWER_FACTOR,
    DEVICE_CLASS_VOLTAGE,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_AMPERE,
    UNIT_MINUTE,
    UNIT_VOLT,
    UNIT_WATT,
)

from . import CONF_PUMPSAVER_ID, PumpSaver

DEPENDENCIES = ["pumpsaver"]

CONF_PUMP_STARTS = "pump_starts"
CONF_RUN_MINUTES = "run_minutes"
CONF_LAST_FAULT_AT = "last_fault_at"
CONF_SIGNAL_RATE = "signal_rate"
CONF_DECODE_ERRORS = "decode_errors"
CONF_REGISTER = "register"

# name -> (register, multiplier applied to the raw 16-bit value)
TYPES = {
    CONF_VOLTAGE: (0x11, 0.1),
    CONF_CURRENT: (0x12, 0.01),
    CONF_POWER: (0x10, 1.0),
    CONF_POWER_FACTOR: (0x13, 0.001),
    CONF_PUMP_STARTS: (0x0F, 1.0),
    CONF_RUN_MINUTES: (0x17, 1.0),
}

NAMED_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(CONF_PUMPSAVER_ID): cv.use_id(PumpSaver),
            cv.Optional(CONF_VOLTAGE): sensor.sensor_schema(
                unit_of_measurement=UNIT_VOLT,
                accuracy_decimals=1,
                device_class=DEVICE_CLASS_VOLTAGE,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_CURRENT): sensor.sensor_schema(
                unit_of_measurement=UNIT_AMPERE,
                accuracy_decimals=2,
                device_class=DEVICE_CLASS_CURRENT,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_POWER): sensor.sensor_schema(
                unit_of_measurement=UNIT_WATT,
                accuracy_decimals=0,
                device_class=DEVICE_CLASS_POWER,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_POWER_FACTOR): sensor.sensor_schema(
                accuracy_decimals=3,
                device_class=DEVICE_CLASS_POWER_FACTOR,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_PUMP_STARTS): sensor.sensor_schema(
                accuracy_decimals=0,
                state_class=STATE_CLASS_TOTAL_INCREASING,
                icon="mdi:counter",
            ),
            cv.Optional(CONF_RUN_MINUTES): sensor.sensor_schema(
                unit_of_measurement=UNIT_MINUTE,
                accuracy_decimals=0,
                device_class=DEVICE_CLASS_DURATION,
                state_class=STATE_CLASS_TOTAL_INCREASING,
                icon="mdi:timer-outline",
            ),
            # Run-clock minutes (same unit as run_minutes) at which the newest
            # fault-history record was logged; changes only when a new fault lands.
            cv.Optional(CONF_LAST_FAULT_AT): sensor.sensor_schema(
                unit_of_measurement=UNIT_MINUTE,
                accuracy_decimals=0,
                icon="mdi:alert-circle-check-outline",
            ),
            # Diagnostics: decoded words/second (a healthy full view of the
            # broadcast is ~40/s) and bad bursts per minute. Published every ~30 s.
            cv.Optional(CONF_SIGNAL_RATE): sensor.sensor_schema(
                unit_of_measurement="words/s",
                accuracy_decimals=1,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:signal",
            ),
            cv.Optional(CONF_DECODE_ERRORS): sensor.sensor_schema(
                unit_of_measurement="/min",
                accuracy_decimals=1,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:alert-decagram-outline",
            ),
        }
    ),
    cv.has_at_least_one_key(*TYPES, CONF_LAST_FAULT_AT, CONF_SIGNAL_RATE, CONF_DECODE_ERRORS),
)

REGISTER_SCHEMA = sensor.sensor_schema(accuracy_decimals=0).extend(
    {
        cv.GenerateID(CONF_PUMPSAVER_ID): cv.use_id(PumpSaver),
        # Known registers are 0x01-0x75; allow the full addressable range for
        # protocol exploration on other models (0xFF is the sync word).
        cv.Required(CONF_REGISTER): cv.All(cv.hex_int, cv.int_range(min=0x01, max=0xFE)),
    }
)


def _dispatch_schema(config):
    if isinstance(config, dict) and CONF_REGISTER in config:
        return REGISTER_SCHEMA(config)
    return NAMED_SCHEMA(config)


CONFIG_SCHEMA = _dispatch_schema


async def to_code(config):
    hub = await cg.get_variable(config[CONF_PUMPSAVER_ID])
    if CONF_REGISTER in config:
        var = await sensor.new_sensor(config)
        cg.add(hub.register_sensor(config[CONF_REGISTER], 1.0, var))
        return
    if CONF_LAST_FAULT_AT in config:
        var = await sensor.new_sensor(config[CONF_LAST_FAULT_AT])
        cg.add(hub.set_last_fault_at_sensor(var))
    if CONF_SIGNAL_RATE in config:
        var = await sensor.new_sensor(config[CONF_SIGNAL_RATE])
        cg.add(hub.set_signal_rate_sensor(var))
    if CONF_DECODE_ERRORS in config:
        var = await sensor.new_sensor(config[CONF_DECODE_ERRORS])
        cg.add(hub.set_decode_errors_sensor(var))
    for key, (reg, multiplier) in TYPES.items():
        if key in config:
            var = await sensor.new_sensor(config[key])
            cg.add(hub.register_sensor(reg, multiplier, var))
