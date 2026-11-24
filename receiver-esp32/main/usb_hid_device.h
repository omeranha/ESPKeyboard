#ifndef USB_HID_DEVICE_H
#define USB_HID_DEVICE_H

#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "keyboard_link.h"

esp_err_t usb_hid_device_start(void);
bool usb_hid_device_mounted(void);
void usb_hid_device_notify_on_report_done(TaskHandle_t task);
bool usb_hid_device_send(const uint8_t report[KEYBOARD_LINK_REPORT_SIZE]);

#endif
