#include "usb_keyboard.h"

#include <string.h>

#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "usb/hid.h"
#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"
#include "usb/usb_host.h"

#define LED_NUM_LOCK 0x01
#define LED_CAPS_LOCK 0x02
#define LED_SCROLL_LOCK 0x04

#define CONTROL_QUEUE_LENGTH 8

typedef enum {
	CONTROL_OPEN_KEYBOARD,
	CONTROL_SET_LEDS,
} control_type_t;

typedef struct {
	control_type_t type;
	hid_host_device_handle_t handle;
	uint8_t leds;
} control_message_t;

static const char *TAG = "usb_keyboard";
static usb_keyboard_report_cb_t report_callback;
static volatile hid_host_device_handle_t active_keyboard;
static QueueHandle_t control_queue;
static uint8_t previous_report[KEYBOARD_LINK_REPORT_SIZE];
static uint8_t lock_leds;

static void check_esp_err(esp_err_t error, const char *operation)
{
	if (error != ESP_OK) {
		ESP_LOGE(TAG, "%s failed: %s", operation, esp_err_to_name(error));
	}
}

static bool report_has_key(const uint8_t report[KEYBOARD_LINK_REPORT_SIZE], uint8_t usage)
{
	for (size_t i = 2; i < KEYBOARD_LINK_REPORT_SIZE; ++i) {
		if (report[i] == usage) {
			return true;
		}
	}
	return false;
}

static void toggle_lock_leds(const uint8_t report[KEYBOARD_LINK_REPORT_SIZE])
{
	static const struct {
		uint8_t usage;
		uint8_t led;
	} locks[] = {
		{HID_KEY_NUM_LOCK, LED_NUM_LOCK},
		{HID_KEY_CAPS_LOCK, LED_CAPS_LOCK},
		{HID_KEY_SCROLL_LOCK, LED_SCROLL_LOCK},
	};
	uint8_t leds = lock_leds;

	for (size_t i = 0; i < sizeof(locks) / sizeof(locks[0]); ++i) {
		if (report_has_key(report, locks[i].usage) && !report_has_key(previous_report, locks[i].usage)) {
			leds ^= locks[i].led;
		}
	}
	memcpy(previous_report, report, sizeof(previous_report));

	if (leds != lock_leds) {
		lock_leds = leds;
		const control_message_t message = {.type = CONTROL_SET_LEDS, .leds = leds};
		xQueueSend(control_queue, &message, 0);
	}
}

static void hid_interface_event_callback(hid_host_device_handle_t handle, const hid_host_interface_event_t event, void *arg)
{
	(void)arg;
	if (handle != active_keyboard) {
		return;
	}

	if (event == HID_HOST_INTERFACE_EVENT_INPUT_REPORT) {
		uint8_t report[64];
		size_t report_length = 0;
		esp_err_t error = hid_host_device_get_raw_input_report_data(handle, report, sizeof(report), &report_length);
		if (error != ESP_OK) {
			ESP_LOGE(TAG, "Could not read keyboard report: %s", esp_err_to_name(error));
			return;
		}
		if (report_length < KEYBOARD_LINK_REPORT_SIZE) {
			ESP_LOGW(TAG, "Ignoring short boot-keyboard report (%u bytes)", (unsigned)report_length);
			return;
		}
		toggle_lock_leds(report);
		report_callback(report);
	} else if (event == HID_HOST_INTERFACE_EVENT_DISCONNECTED) {
		ESP_LOGI(TAG, "Keyboard disconnected; releasing all keys");
		uint8_t released[KEYBOARD_LINK_REPORT_SIZE] = {0};
		report_callback(released);
		active_keyboard = NULL;
		lock_leds = 0;
		memset(previous_report, 0, sizeof(previous_report));
		check_esp_err(hid_host_device_close(handle), "Closing disconnected HID interface");
	} else if (event == HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR) {
		ESP_LOGW(TAG, "USB HID input transfer failed");
	}
}

static void open_keyboard(hid_host_device_handle_t handle)
{
	if (active_keyboard != NULL) {
		ESP_LOGW(TAG, "Ignoring additional keyboard interface");
		return;
	}

	const hid_host_device_config_t config = {
		.callback = hid_interface_event_callback,
		.callback_arg = NULL,
	};
	esp_err_t error = hid_host_device_open(handle, &config);
	if (error != ESP_OK) {
		ESP_LOGE(TAG, "Could not open keyboard interface: %s", esp_err_to_name(error));
		return;
	}
	active_keyboard = handle;

	error = hid_class_request_set_protocol(handle, HID_REPORT_PROTOCOL_BOOT);
	if (error == ESP_OK) {
		error = hid_class_request_set_idle(handle, 0, 0);
	}
	if (error == ESP_OK) {
		error = hid_host_device_start(handle);
	}
	if (error != ESP_OK) {
		ESP_LOGE(TAG, "Could not start boot keyboard: %s", esp_err_to_name(error));
		active_keyboard = NULL;
		check_esp_err(hid_host_device_close(handle), "Closing unsupported keyboard");
		return;
	}

	ESP_LOGI(TAG, "Boot keyboard connected");
}

static void set_keyboard_leds(uint8_t leds)
{
	hid_host_device_handle_t keyboard = active_keyboard;
	if (keyboard != NULL) {
		check_esp_err(hid_class_request_set_report(keyboard, HID_REPORT_TYPE_OUTPUT, 0, &leds, sizeof(leds)), "Setting keyboard LEDs");
	}
}

// Control requests block until the USB client task completes them, and that same task runs the HID driver callbacks, so they must not be issued from a callback.
static void control_task(void *arg)
{
	control_message_t message;

	(void)arg;
	for (;;) {
		xQueueReceive(control_queue, &message, portMAX_DELAY);
		if (message.type == CONTROL_OPEN_KEYBOARD) {
			open_keyboard(message.handle);
		} else {
			set_keyboard_leds(message.leds);
		}
	}
}

static void hid_driver_event_callback(hid_host_device_handle_t handle, const hid_host_driver_event_t event, void *arg)
{
	(void)arg;
	if (event != HID_HOST_DRIVER_EVENT_CONNECTED) {
		return;
	}

	hid_host_dev_params_t params;
	esp_err_t error = hid_host_device_get_params(handle, &params);
	if (error != ESP_OK) {
		ESP_LOGE(TAG, "Could not query HID interface: %s", esp_err_to_name(error));
		return;
	}
	if (params.sub_class != HID_SUBCLASS_BOOT_INTERFACE || params.proto != HID_PROTOCOL_KEYBOARD) {
		ESP_LOGI(TAG, "Ignoring HID interface; a boot-protocol keyboard is required");
		return;
	}

	const control_message_t message = {.type = CONTROL_OPEN_KEYBOARD, .handle = handle};
	if (xQueueSend(control_queue, &message, 0) != pdTRUE) {
		ESP_LOGW(TAG, "Control queue full; keyboard interface ignored");
	}
}
static void usb_library_task(void *arg)
{
	TaskHandle_t app_task = (TaskHandle_t)arg;
	const usb_host_config_t host_config = {
		.skip_phy_setup = false,
		.intr_flags = ESP_INTR_FLAG_LOWMED,
	};
	ESP_ERROR_CHECK(usb_host_install(&host_config));
	xTaskNotifyGive(app_task);

	for (;;) {
		uint32_t event_flags = 0;
		esp_err_t error = usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
		if (error != ESP_OK) {
			ESP_LOGE(TAG, "USB host event handling failed: %s", esp_err_to_name(error));
			vTaskDelay(pdMS_TO_TICKS(10));
		}
		if ((event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) != 0) {
			ESP_ERROR_CHECK(usb_host_device_free_all());
			break;
		}
	}

	ESP_ERROR_CHECK(usb_host_uninstall());
	vTaskDelete(NULL);
}

bool usb_keyboard_connected(void)
{
	return active_keyboard != NULL;
}

esp_err_t usb_keyboard_start(usb_keyboard_report_cb_t on_report)
{
	report_callback = on_report;

	control_queue = xQueueCreate(CONTROL_QUEUE_LENGTH, sizeof(control_message_t));
	if (control_queue == NULL || xTaskCreate(control_task, "keyboard_control", 4096, NULL, 4, NULL) != pdPASS) {
		return ESP_ERR_NO_MEM;
	}
	if (xTaskCreate(usb_library_task, "usb_events", 4096, xTaskGetCurrentTaskHandle(), 2, NULL) != pdPASS) {
		return ESP_ERR_NO_MEM;
	}
	if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) == 0) {
		ESP_LOGE(TAG, "USB host library did not initialize");
		return ESP_ERR_TIMEOUT;
	}

	const hid_host_driver_config_t hid_config = {
		.create_background_task = true,
		.task_priority = 5,
		.stack_size = 4096,
		.core_id = 0,
		.callback = hid_driver_event_callback,
		.callback_arg = NULL,
	};
	esp_err_t error = hid_host_install(&hid_config);
	if (error == ESP_OK) {
		ESP_LOGI(TAG, "Ready for a boot-protocol USB keyboard");
	}
	return error;
}
