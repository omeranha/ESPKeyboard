#ifndef TRANSPORT_BLE_H
#define TRANSPORT_BLE_H

#include "esp_err.h"
#include "keyboard_link.h"

esp_err_t ble_link_start(void);
void ble_link_send(const uint8_t report[KEYBOARD_LINK_REPORT_SIZE]);

#endif
