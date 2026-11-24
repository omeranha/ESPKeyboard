#include "usb_hid_device.h"

#include <stdio.h>

#include "class/hid/hid_device.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"

#define HID_INTERFACE_STRING_INDEX 4
#define ENDPOINT_IN_ADDRESS 0x81
#define ENDPOINT_SIZE KEYBOARD_LINK_REPORT_SIZE
#define POLL_INTERVAL_MS 1
#define CONFIGURATION_LENGTH (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)

static const char *TAG = "usb_hid_device";
static char serial_number[13];
static volatile TaskHandle_t report_done_task;

static const uint8_t report_descriptor[] = {TUD_HID_REPORT_DESC_KEYBOARD()};

static const uint8_t configuration_descriptor[] = {
	TUD_CONFIG_DESCRIPTOR(1, 1, 0, CONFIGURATION_LENGTH, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
	TUD_HID_DESCRIPTOR(0, HID_INTERFACE_STRING_INDEX, HID_ITF_PROTOCOL_KEYBOARD, sizeof(report_descriptor), ENDPOINT_IN_ADDRESS, ENDPOINT_SIZE, POLL_INTERVAL_MS),
};

static const char *string_descriptor[] = {
	(char[]){0x09, 0x04},
	"ESPKeyboard",
	"ESPKeyboard Receiver",
	serial_number,
	"Keyboard",
};

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
	(void)instance;
	return report_descriptor;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen)
{
	(void)instance;
	(void)report_id;
	(void)report_type;
	(void)buffer;
	(void)reqlen;
	return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer, uint16_t bufsize)
{
	(void)instance;
	(void)report_id;
	if (report_type == HID_REPORT_TYPE_OUTPUT && bufsize >= 1) {
		ESP_LOGI(TAG, "Host LEDs: num=%d caps=%d scroll=%d", buffer[0] & 1, (buffer[0] >> 1) & 1, (buffer[0] >> 2) & 1);
	}
}

void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report, uint16_t len)
{
	(void)instance;
	(void)report;
	(void)len;
	TaskHandle_t task = report_done_task;
	if (task != NULL) {
		xTaskNotifyGive(task);
	}
}

void usb_hid_device_notify_on_report_done(TaskHandle_t task)
{
	report_done_task = task;
}

esp_err_t usb_hid_device_start(void)
{
	uint8_t mac[6];
	esp_err_t error = esp_read_mac(mac, ESP_MAC_WIFI_STA);
	if (error != ESP_OK) {
		return error;
	}
	snprintf(serial_number, sizeof(serial_number), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

	tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG();
	config.descriptor.device = NULL;
	config.descriptor.full_speed_config = configuration_descriptor;
	config.descriptor.string = string_descriptor;
	config.descriptor.string_count = sizeof(string_descriptor) / sizeof(string_descriptor[0]);
	return tinyusb_driver_install(&config);
}

bool usb_hid_device_mounted(void)
{
	return tud_mounted();
}

bool usb_hid_device_send(const uint8_t report[KEYBOARD_LINK_REPORT_SIZE])
{
	if (tud_suspended()) {
		tud_remote_wakeup();
		return false;
	}
	if (!tud_hid_ready()) {
		return false;
	}
	return tud_hid_report(0, report, KEYBOARD_LINK_REPORT_SIZE);
}
