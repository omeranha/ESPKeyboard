#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "keyboard_link.h"
#include "usb_keyboard.h"

#ifdef CONFIG_ESPKEYBOARD_TRANSPORT_ESPNOW
#include "transport_espnow.h"
#endif
#ifdef CONFIG_ESPKEYBOARD_TRANSPORT_BLE
#include "transport_ble.h"
#endif
#ifdef CONFIG_ESPKEYBOARD_TEST_TYPER
#include "test_typer.h"
#endif

#define REPORT_QUEUE_LENGTH 16
#define TRANSPORT_TASK_PRIORITY 5
#define ESPNOW_REFRESH_MS 100

static const char *TAG = "espkeyboard";
#ifdef CONFIG_ESPKEYBOARD_TRANSPORT_ESPNOW
static QueueHandle_t espnow_queue;
#endif
#ifdef CONFIG_ESPKEYBOARD_TRANSPORT_BLE
static QueueHandle_t ble_queue;
#endif

// Every report is a full snapshot, so when a queue is full the oldest entry goes and the newest, which is the state the host must end up in, is kept.
static void push_latest_report(QueueHandle_t queue, const uint8_t report[KEYBOARD_LINK_REPORT_SIZE])
{
	uint8_t discarded[KEYBOARD_LINK_REPORT_SIZE];

	if (xQueueSend(queue, report, 0) != pdTRUE) {
		xQueueReceive(queue, discarded, 0);
		xQueueSend(queue, report, 0);
	}
}

static void enqueue_keyboard_report(const uint8_t report[KEYBOARD_LINK_REPORT_SIZE])
{
#ifdef CONFIG_ESPKEYBOARD_TRANSPORT_ESPNOW
	if (espnow_queue != NULL) {
		push_latest_report(espnow_queue, report);
	}
#endif
#ifdef CONFIG_ESPKEYBOARD_TRANSPORT_BLE
	if (ble_queue != NULL) {
		push_latest_report(ble_queue, report);
	}
#endif
}

#ifdef CONFIG_ESPKEYBOARD_TRANSPORT_ESPNOW
// ESP-NOW has no connection state, so the current report is resent on a timer: a lost frame then costs at most one refresh period.
static void espnow_task(void *arg)
{
	uint8_t latest_report[KEYBOARD_LINK_REPORT_SIZE] = {0};

	(void)arg;
	for (;;) {
		xQueueReceive(espnow_queue, latest_report, pdMS_TO_TICKS(ESPNOW_REFRESH_MS));
		(void)espnow_link_send(latest_report);
	}
}
#endif

#ifdef CONFIG_ESPKEYBOARD_TRANSPORT_BLE
// BLE notifications are reliable, so reports go out only when the keyboard state changes.
static void ble_task(void *arg)
{
	uint8_t report[KEYBOARD_LINK_REPORT_SIZE];

	(void)arg;
	for (;;) {
		xQueueReceive(ble_queue, report, portMAX_DELAY);
		ble_link_send(report);
	}
}
#endif

static QueueHandle_t start_transport_task(TaskFunction_t task, const char *name)
{
	QueueHandle_t queue = xQueueCreate(REPORT_QUEUE_LENGTH, KEYBOARD_LINK_REPORT_SIZE);
	if (queue == NULL || xTaskCreate(task, name, 3072, NULL, TRANSPORT_TASK_PRIORITY, NULL) != pdPASS) {
		ESP_LOGE(TAG, "Could not start the %s task", name);
		return NULL;
	}
	return queue;
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

#ifdef CONFIG_ESPKEYBOARD_TRANSPORT_ESPNOW
	if (espnow_link_start() == ESP_OK) {
		espnow_queue = start_transport_task(espnow_task, "espnow_tx");
	}
#endif
#ifdef CONFIG_ESPKEYBOARD_TRANSPORT_BLE
	if (ble_link_start() == ESP_OK) {
		ble_queue = start_transport_task(ble_task, "ble_tx");
	}
#endif

#ifdef CONFIG_ESPKEYBOARD_TEST_TYPER
	if (test_typer_start(enqueue_keyboard_report) != ESP_OK) {
		ESP_LOGE(TAG, "Could not create the test typer task");
	}
#endif

	esp_err_t error = usb_keyboard_start(enqueue_keyboard_report);
	if (error != ESP_OK) {
		ESP_LOGE(TAG, "USB keyboard host failed to start: %s", esp_err_to_name(error));
	}
}
