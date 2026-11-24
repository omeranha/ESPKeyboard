#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "espnow_receiver.h"
#include "keyboard_link.h"
#include "usb_hid_device.h"

#if CONFIG_FREERTOS_HZ < 1000
#error "receiver-esp32 needs CONFIG_FREERTOS_HZ=1000: a shorter tick rounds the retry wait down to zero and the pump task spins"
#endif

// The sender sends a snapshot per change plus a refresh every 100 ms, so this only has to absorb a short burst of fast typing.
#define REPORT_QUEUE_DEPTH 8
#define IDLE_POLL_MS 20
#define PENDING_RETRY_MS 5
#define STATUS_INTERVAL_MS 5000

static const char *TAG = "espkeyboard_rx";
static QueueHandle_t report_queue;
static volatile int64_t last_report_us;
static volatile bool have_report;
static uint8_t current_report[KEYBOARD_LINK_REPORT_SIZE];
static bool report_pending;

static bool report_is_idle(const uint8_t report[KEYBOARD_LINK_REPORT_SIZE])
{
	for (size_t i = 0; i < KEYBOARD_LINK_REPORT_SIZE; ++i) {
		if (report[i] != 0) {
			return false;
		}
	}
	return true;
}

static void on_sender_report(const uint8_t report[KEYBOARD_LINK_REPORT_SIZE])
{
	uint8_t discarded[KEYBOARD_LINK_REPORT_SIZE];

	last_report_us = esp_timer_get_time();
	have_report = true;

	// Drop the oldest rather than the newest: every packet carries the complete state, so the latest one is the most useful.
	if (xQueueSend(report_queue, report, 0) != pdTRUE) {
		xQueueReceive(report_queue, discarded, 0);
		xQueueSend(report_queue, report, 0);
	}
}

static void forget_pending_reports(void)
{
	xQueueReset(report_queue);
	memset(current_report, 0, sizeof(current_report));
	report_pending = false;
}

static void sender_timed_out(void)
{
	ESP_LOGW(TAG, "Sender timed out; releasing all keys");
	xQueueReset(report_queue);
	memset(current_report, 0, sizeof(current_report));
	report_pending = true;
}

static void report_pump_task(void *arg)
{
	uint8_t next_report[KEYBOARD_LINK_REPORT_SIZE];
	int64_t last_status_us = 0;

	(void)arg;
	usb_hid_device_notify_on_report_done(xTaskGetCurrentTaskHandle());
	for (;;) {
		if (!usb_hid_device_mounted()) {
			// A host that shows up later must not inherit keys held before it arrived.
			forget_pending_reports();
			vTaskDelay(pdMS_TO_TICKS(IDLE_POLL_MS));
			continue;
		}

		if (report_pending) {
			ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(PENDING_RETRY_MS));
		} else if (xQueueReceive(report_queue, next_report, pdMS_TO_TICKS(IDLE_POLL_MS)) == pdTRUE) {
			if (memcmp(current_report, next_report, sizeof(current_report)) != 0) {
				memcpy(current_report, next_report, sizeof(current_report));
				report_pending = true;
			}
		}

		if (!report_is_idle(current_report) && have_report && (esp_timer_get_time() - last_report_us) / 1000 >= CONFIG_ESPKEYBOARD_SENDER_TIMEOUT_MS) {
			sender_timed_out();
		}

		if (report_pending && usb_hid_device_send(current_report)) {
			report_pending = false;
		}

		int64_t now_us = esp_timer_get_time();
		if ((now_us - last_status_us) / 1000 >= STATUS_INTERVAL_MS) {
			last_status_us = now_us;
			espnow_receiver_log_status();
		}
	}
}

static void initialize_nvs(void)
{
	esp_err_t error = nvs_flash_init();
	if (error == ESP_ERR_NVS_NO_FREE_PAGES || error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		error = nvs_flash_init();
	}
	ESP_ERROR_CHECK(error);
}

void app_main(void)
{
	initialize_nvs();

	report_queue = xQueueCreate(REPORT_QUEUE_DEPTH, KEYBOARD_LINK_REPORT_SIZE);
	if (report_queue == NULL) {
		ESP_LOGE(TAG, "Could not allocate the report queue");
		return;
	}

	esp_err_t error = usb_hid_device_start();
	if (error != ESP_OK) {
		ESP_LOGE(TAG, "USB HID device failed to start: %s", esp_err_to_name(error));
		return;
	}

	if (xTaskCreate(report_pump_task, "report_pump", 4096, NULL, 5, NULL) != pdPASS) {
		ESP_LOGE(TAG, "Could not create the report pump task");
		return;
	}

	error = espnow_receiver_start(on_sender_report);
	if (error != ESP_OK) {
		ESP_LOGE(TAG, "ESP-NOW receiver is not running: %s", esp_err_to_name(error));
	}
}
