#include "test_typer.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/hid_usage_keyboard.h"

#include "usb_keyboard.h"

// Long enough that the press and the release are two distinct snapshots on the link, so a receiver cannot coalesce them away.
#define TEST_TYPER_HOLD_MS 50

static const char *TAG = "test_typer";
static test_typer_report_cb_t report_callback;

static void test_typer_task(void *arg)
{
	uint8_t report[KEYBOARD_LINK_REPORT_SIZE];
	bool was_idle = true;

	(void)arg;
	ESP_LOGW(TAG, "Test typer enabled: sending 'a' every %d ms while no keyboard is attached", CONFIG_ESPKEYBOARD_TEST_TYPER_INTERVAL_MS);

	for (;;) {
		vTaskDelay(pdMS_TO_TICKS(CONFIG_ESPKEYBOARD_TEST_TYPER_INTERVAL_MS));

		if (usb_keyboard_connected()) {
			if (was_idle) {
				ESP_LOGI(TAG, "Paused; a real keyboard is connected");
				was_idle = false;
			}
			continue;
		}
		if (!was_idle) {
			ESP_LOGI(TAG, "Resumed; no keyboard connected");
			was_idle = true;
		}

		memset(report, 0, sizeof(report));
		report[2] = HID_KEY_A;
		report_callback(report);

		vTaskDelay(pdMS_TO_TICKS(TEST_TYPER_HOLD_MS));

		memset(report, 0, sizeof(report));
		report_callback(report);
	}
}

esp_err_t test_typer_start(test_typer_report_cb_t on_report)
{
	report_callback = on_report;
	return xTaskCreate(test_typer_task, "test_typer", 2560, NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
