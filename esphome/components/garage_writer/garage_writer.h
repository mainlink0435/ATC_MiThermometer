#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include "esphome/components/esp32_ble_client/ble_client_base.h"
#include "esphome/core/component.h"

namespace esphome {
namespace garage_writer {

/**
 * BLE client that writes garage-door state to an ATC_MiThermometer
 * (Xiaomi LYWSD03MMC). See the component README in __init__.py for the
 * protocol: writes [0x4A][state] to char 0x1F1F (service 0x1F10).
 */
class GarageWriter : public esp32_ble_client::BLEClientBase {
 public:
  /// 0=closed 1=open 2=opening 3=closing, 0xFF = off (normal display)
  void set_state(uint8_t state);

 protected:
  void on_disconnect_complete(esp_err_t reason) override;

  esp32_ble_client::BLECharacteristic *rx_char_{nullptr};
};

}  // namespace garage_writer
}  // namespace esphome

#endif  // USE_ESP32
