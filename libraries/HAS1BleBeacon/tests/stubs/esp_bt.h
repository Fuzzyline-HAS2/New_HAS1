#pragma once

#include <stdint.h>

using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;

enum esp_bt_mode_t { BT_MODE_IDLE, BT_MODE_BLE, BT_MODE_CLASSIC_BT, BT_MODE_BTDM };

struct esp_vhci_host_callback_t {
  void (*notify_host_send_available)();
  int (*notify_host_recv)(uint8_t*, uint16_t);
};

esp_err_t esp_vhci_host_register_callback(const esp_vhci_host_callback_t* callback);
bool esp_vhci_host_check_send_available();
void esp_vhci_host_send_packet(uint8_t* data, uint16_t size);
