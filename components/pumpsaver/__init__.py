"""PumpSaver hub component.

Registers itself as a listener on an existing remote_receiver and decodes
SymCom / Littelfuse PumpSaver Plus baseband-IR broadcasts into register
values. See https://github.com/lizbit-official/pumpsaver-ir-protocol for the
protocol specification.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import remote_base
from esphome.const import CONF_ID

CODEOWNERS = ["@lizbit-official"]
DEPENDENCIES = ["remote_receiver"]
MULTI_CONF = True

CONF_PUMPSAVER_ID = "pumpsaver_id"

pumpsaver_ns = cg.esphome_ns.namespace("pumpsaver")
PumpSaver = pumpsaver_ns.class_(
    "PumpSaver", cg.Component, remote_base.RemoteReceiverListener
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(PumpSaver),
        }
    )
    .extend(remote_base.REMOTE_LISTENER_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await remote_base.register_listener(var, config)
