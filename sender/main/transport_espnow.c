#include "transport_espnow.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define TX_RETRY_LIMIT 3
// ESP-NOW reports the per-frame result once its own MAC-level retries finish, which takes a few milliseconds. A long wait here stalls the sender task and drops key transitions during a burst of typing.
#define TX_CALLBACK_TIMEOUT_MS 10

static const char *TAG = "espnow_link";
static SemaphoreHandle_t send_done;
static volatile esp_now_send_status_t send_status;
static uint8_t paired_mac[6];
static uint16_t next_sequence;

static void send_callback(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
	(void)tx_info;
	send_status = status;
	xSemaphoreGive(send_done);
}

static esp_err_t initialize_wifi(void)
{
	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	if (esp_netif_create_default_wifi_sta() == NULL) {
		ESP_LOGE(TAG, "Could not create default Wi-Fi station interface");
		return ESP_FAIL;
	}

	const wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&wifi_config));
	ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_start());
	ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
	ESP_ERROR_CHECK(esp_wifi_set_channel(CONFIG_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));

	uint8_t local_mac[6];
	ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, local_mac));
	ESP_LOGI(TAG, "Station MAC: " MACSTR ", channel %d", MAC2STR(local_mac), CONFIG_ESPNOW_CHANNEL);
	return ESP_OK;
}

esp_err_t espnow_link_start(void)
{
	esp_err_t error = initialize_wifi();
	if (error != ESP_OK) {
		return error;
	}

	if (!keyboard_link_parse_mac(CONFIG_ESPNOW_PEER_MAC, paired_mac)) {
		ESP_LOGE(TAG, "ESP-NOW is off: set the receiver MAC in menuconfig (ESPKeyboard sender > Paired receiver station MAC address)");
		return ESP_ERR_INVALID_STATE;
	}
	ESP_LOGI(TAG, "Paired receiver: " MACSTR, MAC2STR(paired_mac));

	send_done = xSemaphoreCreateBinary();
	if (send_done == NULL) {
		return ESP_ERR_NO_MEM;
	}

	ESP_ERROR_CHECK(esp_now_init());
	ESP_ERROR_CHECK(esp_now_register_send_cb(send_callback));

	esp_now_peer_info_t peer = {0};
	memcpy(peer.peer_addr, paired_mac, sizeof(peer.peer_addr));
	peer.channel = CONFIG_ESPNOW_CHANNEL;
	peer.ifidx = WIFI_IF_STA;
	peer.encrypt = false;
	error = esp_now_add_peer(&peer);
	if (error != ESP_OK) {
		return error;
	}

#ifdef CONFIG_ESPKEYBOARD_ESPNOW_RATE_24M
	esp_now_rate_config_t rate = {.phymode = WIFI_PHY_MODE_11G, .rate = WIFI_PHY_RATE_24M};
	if (esp_now_set_peer_rate_config(paired_mac, &rate) != ESP_OK) {
		ESP_LOGW(TAG, "Could not select 24 Mbps; staying at the default rate");
	}
#endif
	return ESP_OK;
}

bool espnow_link_send(const uint8_t report[KEYBOARD_LINK_REPORT_SIZE])
{
	if (send_done == NULL) {
		return false;
	}

	keyboard_link_packet_t packet = {
		.version = KEYBOARD_LINK_VERSION,
	};
	keyboard_link_set_sequence(&packet, next_sequence++);
	memcpy(packet.report, report, sizeof(packet.report));

	for (unsigned int attempt = 1; attempt <= TX_RETRY_LIMIT; ++attempt) {
		xSemaphoreTake(send_done, 0);

		esp_err_t error = esp_now_send(paired_mac, (const uint8_t *)&packet, sizeof(packet));
		if (error != ESP_OK) {
			ESP_LOGW(TAG, "Send attempt %u failed: %s", attempt, esp_err_to_name(error));
			vTaskDelay(pdMS_TO_TICKS(2));
			continue;
		}

		// Separate "the radio never finished" from "the receiver did not acknowledge": the first points at the timeout being too tight, the second at the RF link.
		if (xSemaphoreTake(send_done, pdMS_TO_TICKS(TX_CALLBACK_TIMEOUT_MS)) != pdTRUE) {
			ESP_LOGW(TAG, "Send attempt %u gave no result within %d ms", attempt, TX_CALLBACK_TIMEOUT_MS);
		} else if (send_status == ESP_NOW_SEND_SUCCESS) {
			return true;
		} else {
			ESP_LOGW(TAG, "Send attempt %u was not acknowledged by the receiver", attempt);
		}
	}

	ESP_LOGE(TAG, "Could not send keyboard report; the receiver resyncs on the next one");
	return false;
}
