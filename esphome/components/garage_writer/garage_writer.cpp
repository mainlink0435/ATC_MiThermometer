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

// Delay (ms) after writing before disconnecting. Generous to ensure the write
// is transmitted: a BLE write only reaches the slave at its next listen
// window, which can be ~2.5s if the thermometer's connection latency is high.
static const uint32_t DISCONNECT_DELAY_MS = 8000;

void GarageWriter::setup() {
  BLEClientBase::setup();
  // We manage connections ourselves: only connect when a command is pending,
  // and disconnect afterwards so the thermometer can sleep.
  this->set_auto_connect(false);
}

void GarageWriter::loop() {
  BLEClientBase::loop();
  // ESPHome 2025.10.x sets the connected state directly (bypassing our
  // set_state() hook), so poll connected() here and send any pending command.
  if (this->connected() && this->pending_write_) {
    this->write_pending_();
  }
}

void GarageWriter::set_state(uint8_t state) {
  this->pending_state_ = state;
  this->pending_write_ = true;
  if (this->connected()) {
    this->write_pending_();
  } else {
    // Drop any stale characteristic handle, then let the tracker connect on
    // the next scan hit.
    this->rx_char_ = nullptr;
    this->set_auto_connect(true);
  }
}

void GarageWriter::set_state(esp32_ble_tracker::ClientState st) {
  BLEClientBase::set_state(st);
  if (st == esp32_ble_tracker::ClientState::IDLE) {
    // Connection fully torn down: drop the (now-freed) characteristic handle
    // and reconnect if a new command is queued.
    this->rx_char_ = nullptr;
    if (this->pending_write_) {
      this->set_auto_connect(true);
    }
  }
}

void GarageWriter::write_pending_() {
  if (!this->pending_write_)
    return;
  this->pending_write_ = false;
  if (this->rx_char_ == nullptr) {
    this->rx_char_ = this->get_characteristic(SERVICE_UUID, CHAR_UUID);
  }
  if (this->rx_char_ == nullptr) {
    ESP_LOGW(TAG, "Characteristic 0x%04X not found on device", CHAR_UUID);
    this->set_auto_connect(false);
    this->unconditional_disconnect();
    return;
  }
  uint8_t data[2] = {CMD_GARAGE, this->pending_state_};
  esp_err_t err = this->rx_char_->write_value(data, 2, ESP_GATT_WRITE_TYPE_NO_RSP);
  if (err == ESP_OK) {
    ESP_LOGD(TAG, "Sent garage state %u", this->pending_state_);
  } else {
    ESP_LOGW(TAG, "Write failed, err=%d", err);
  }
  // Stop auto-reconnect and drop the link shortly after the write so the
  // thermometer can go back to sleep.
  this->set_auto_connect(false);
  this->set_timeout(DISCONNECT_DELAY_MS, [this]() { this->unconditional_disconnect(); });
}

}  // namespace garage_writer
}  // namespace esphome

#endif  // USE_ESP32
