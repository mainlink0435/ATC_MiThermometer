#include "garage_writer.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"

namespace esphome {
namespace garage_writer {

static const char *const TAG = "garage_writer";

// ATC firmware RxTx characteristic
static const uint16_t SERVICE_UUID = 0x1F10;
static const uint16_t CHAR_UUID = 0x1F1F;
static const uint8_t CMD_GARAGE = 0x4A;

void GarageWriter::set_state(uint8_t state) {
  if (!this->connected()) {
    ESP_LOGW(TAG, "Not connected, cannot write garage state %u", state);
    return;
  }
  if (this->rx_char_ == nullptr) {
    this->rx_char_ = this->get_characteristic(SERVICE_UUID, CHAR_UUID);
  }
  if (this->rx_char_ == nullptr) {
    ESP_LOGW(TAG, "Characteristic 0x%04X not found on device", CHAR_UUID);
    return;
  }
  uint8_t data[2] = {CMD_GARAGE, state};
  esp_err_t err = this->rx_char_->write_value(data, 2, ESP_GATT_WRITE_TYPE_NO_RSP);
  if (err == ESP_OK) {
    ESP_LOGD(TAG, "Sent garage state %u", state);
  } else {
    ESP_LOGW(TAG, "Write failed, err=%d", err);
  }
}

void GarageWriter::on_disconnect_complete(esp_err_t reason) {
  this->rx_char_ = nullptr;
  BLEClientBase::on_disconnect_complete(reason);
}

}  // namespace garage_writer
}  // namespace esphome

#endif  // USE_ESP32
