#ifndef USB_KEYBOARD_H
#define USB_KEYBOARD_H

#include <stdbool.h>

#include "esp_err.h"
#include "keyboard_link.h"

typedef void (*usb_keyboard_report_cb_t)(const uint8_t report[KEYBOARD_LINK_REPORT_SIZE]);

esp_err_t usb_keyboard_start(usb_keyboard_report_cb_t on_report);
bool usb_keyboard_connected(void);

#endif
