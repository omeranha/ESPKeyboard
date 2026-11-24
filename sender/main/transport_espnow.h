#ifndef TRANSPORT_ESPNOW_H
#define TRANSPORT_ESPNOW_H

#include <stdbool.h>

#include "esp_err.h"
#include "keyboard_link.h"

esp_err_t espnow_link_start(void);
bool espnow_link_send(const uint8_t report[KEYBOARD_LINK_REPORT_SIZE]);

#endif
