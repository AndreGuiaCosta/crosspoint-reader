// Host/simulator stub for the ESP-IDF Bluetooth controller API.
//
// CrossPointWebServerActivity calls these to release the BLE controller's
// memory on the device; the native simulator has no BT controller, so these
// are no-ops. Only the symbols the firmware actually references are provided.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  ESP_BT_CONTROLLER_STATUS_IDLE = 0,
  ESP_BT_CONTROLLER_STATUS_INITED,
  ESP_BT_CONTROLLER_STATUS_ENABLED,
  ESP_BT_CONTROLLER_STATUS_NUM,
} esp_bt_controller_status_t;

typedef enum {
  ESP_BT_MODE_IDLE = 0x00,
  ESP_BT_MODE_BLE = 0x01,
  ESP_BT_MODE_CLASSIC_BT = 0x02,
  ESP_BT_MODE_BTDM = 0x03,
} esp_bt_mode_t;

static inline esp_bt_controller_status_t esp_bt_controller_get_status(void) {
  return ESP_BT_CONTROLLER_STATUS_IDLE;
}
static inline int esp_bt_controller_disable(void) { return 0; }
static inline int esp_bt_controller_deinit(void) { return 0; }
static inline int esp_bt_mem_release(esp_bt_mode_t mode) {
  (void)mode;
  return 0;
}

#ifdef __cplusplus
}
#endif
