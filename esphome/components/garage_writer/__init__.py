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
#
# This is registered as a direct BLE client (like ESPHome's own `ble_client`
# component): the C++ class derives from esp32_ble_client::BLEClientBase and is
# registered with esp32_ble_tracker, so it needs a mac_address.
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import esp32_ble, esp32_ble_client, esp32_ble_tracker
from esphome.const import CONF_ID, CONF_MAC_ADDRESS

AUTO_LOAD = ["esp32_ble_tracker"]
DEPENDENCIES = ["esp32_ble_client", "esp32_ble_tracker"]

CONF_AUTO_CONNECT = "auto_connect"

garage_ns = cg.esphome_ns.namespace("garage_writer")
GarageWriter = garage_ns.class_("GarageWriter", esp32_ble_client.BLEClientBase)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(GarageWriter),
            cv.Required(CONF_MAC_ADDRESS): cv.mac_address,
            cv.Optional(CONF_AUTO_CONNECT, default=False): cv.boolean,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(esp32_ble_tracker.ESP_BLE_DEVICE_SCHEMA),
    esp32_ble.consume_connection_slots(1, "garage_writer"),
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await esp32_ble_tracker.register_client(var, config)
    cg.add(var.set_address(config[CONF_MAC_ADDRESS].as_hex))
    cg.add(var.set_auto_connect(config[CONF_AUTO_CONNECT]))
