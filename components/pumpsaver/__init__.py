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
CONF_LINK_TIMEOUT = "link_timeout"

pumpsaver_ns = cg.esphome_ns.namespace("pumpsaver")
PumpSaver = pumpsaver_ns.class_(
    "PumpSaver", cg.Component, remote_base.RemoteReceiverListener
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(PumpSaver),
            # Normal broadcasts arrive about twice a second. Ten seconds gives
            # ample margin for a few damaged frames without hiding a dead link.
            cv.Optional(CONF_LINK_TIMEOUT, default="10s"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(min=cv.TimePeriod(milliseconds=3000)),
            ),
        }
    )
    .extend(remote_base.REMOTE_LISTENER_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await remote_base.register_listener(var, config)
    cg.add(
        var.set_link_timeout_ms(config[CONF_LINK_TIMEOUT].total_milliseconds)
    )
