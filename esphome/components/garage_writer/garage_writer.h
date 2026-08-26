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
 *
 * It starts dormant and only connects when a state is requested, so the
 * thermometer is not kept awake. On a command it connects, writes the state,
 * then disconnects after a short delay.
 */
class GarageWriter : public esp32_ble_client::BLEClientBase {
 public:
  /// 0=closed 1=open 2=opening 3=closing 4=error, 0xFF = off (normal display)
  void set_state(uint8_t state);
  void setup() override;

 protected:
  void set_state(esp32_ble_tracker::ClientState st) override;
  void on_disconnect_complete(esp_err_t reason) override;
  void write_pending_();

  esp32_ble_client::BLECharacteristic *rx_char_{nullptr};
  uint8_t pending_state_{0xFF};
  bool pending_write_{false};
};

}  // namespace garage_writer
}  // namespace esphome

#endif  // USE_ESP32
