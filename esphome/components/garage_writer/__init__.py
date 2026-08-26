# ESPHome custom component for writing garage-door state to an ATC_MiThermometer
# (Xiaomi LYWSD03MMC) over BLE.
#
# It connects to the thermometer as a BLE client only when a command is
# requested, writes [0x4A][state] to the RxTx characteristic (service 0x1F10,
# char 0x1F1F), then disconnects so the thermometer can sleep. The firmware
# then runs the matching baked-in animation.
#
#   state 0   = closed
#   state 1   = open
#   state 2   = opening
#   state 3   = closing
#   state 4   = error
#   state 0xFF = off (return to normal temp/hum display)
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import ble_client, esp32_ble_tracker
from esphome.const import CONF_ID

DEPENDENCIES = ["ble_client", "esp32_ble_tracker"]

garage_ns = cg.esphome_ns.namespace("garage_writer")
GarageWriter = garage_ns.class_("GarageWriter", ble_client.BLEClientBase, cg.Component)

CONFIG_SCHEMA = (
    ble_client.BLE_CLIENT_SCHEMA.extend({cv.GenerateID(): cv.declare_id(GarageWriter)}).extend(
        cv.COMPONENT_SCHEMA
    )
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await ble_client.register_ble_client(var, config)
