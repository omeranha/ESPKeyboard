#ifndef ESPNOW_RECEIVER_H
#define ESPNOW_RECEIVER_H

#include "esp_err.h"
#include "keyboard_link.h"

typedef void (*espnow_report_cb_t)(const uint8_t report[KEYBOARD_LINK_REPORT_SIZE]);

esp_err_t espnow_receiver_start(espnow_report_cb_t on_report);
void espnow_receiver_log_status(void);

#endif
