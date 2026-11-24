#include "espnow_receiver.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_wifi.h"

// A sender reboot restarts its sequence counter at zero, which looks like a run of stale packets. Tolerate this many before concluding it restarted.
#define SEQUENCE_RESYNC_AFTER 8

static const char *TAG = "espnow_receiver";
static espnow_report_cb_t report_callback;
static uint8_t paired_mac[6];
static uint16_t last_sequence;
static bool have_sequence;
static uint32_t stale_packets;

static volatile uint32_t rx_total;
static volatile uint32_t rx_accepted;
static volatile uint32_t rx_wrong_peer;
static volatile uint32_t rx_bad_format;
static volatile uint32_t rx_duplicate;
static volatile uint32_t link_gaps;
static volatile uint32_t link_resyncs;

static bool sequence_is_newer(uint16_t candidate, uint16_t previous)
{
	return (int16_t)(candidate - previous) > 0;
}

static void receive_callback(const esp_now_recv_info_t *recv_info, const uint8_t *data, int length)
{
	keyboard_link_packet_t packet;

	rx_total++;
	if (recv_info == NULL || data == NULL || length != sizeof(packet)) {
		rx_bad_format++;
		return;
	}
	if (memcmp(recv_info->src_addr, paired_mac, sizeof(paired_mac)) != 0) {
		rx_wrong_peer++;
		return;
	}

	memcpy(&packet, data, sizeof(packet));
	if (packet.version != KEYBOARD_LINK_VERSION) {
		rx_bad_format++;
		return;
	}

	uint16_t sequence = keyboard_link_get_sequence(&packet);
	if (have_sequence && !sequence_is_newer(sequence, last_sequence)) {
		if (++stale_packets < SEQUENCE_RESYNC_AFTER) {
			rx_duplicate++;
			return;
		}
		link_resyncs++;
	} else if (have_sequence && (uint16_t)(sequence - last_sequence) != 1) {
		link_gaps++;
	}

	stale_packets = 0;
	have_sequence = true;
	last_sequence = sequence;
	rx_accepted++;
	report_callback(packet.report);
}

esp_err_t espnow_receiver_start(espnow_report_cb_t on_report)
{
	report_callback = on_report;

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

	if (!keyboard_link_parse_mac(CONFIG_ESPNOW_PEER_MAC, paired_mac)) {
		ESP_LOGE(TAG, "ESP-NOW is off: set the sender MAC in menuconfig (ESPKeyboard receiver > Paired sender station MAC address)");
		return ESP_ERR_INVALID_STATE;
	}
	ESP_LOGI(TAG, "Paired sender: " MACSTR, MAC2STR(paired_mac));

	ESP_ERROR_CHECK(esp_now_init());
	return esp_now_register_recv_cb(receive_callback);
}

void espnow_receiver_log_status(void)
{
	ESP_LOGI(TAG, "rx=%u ok=%u peer=%u fmt=%u dup=%u gaps=%u resync=%u", (unsigned)rx_total, (unsigned)rx_accepted, (unsigned)rx_wrong_peer,
			(unsigned)rx_bad_format, (unsigned)rx_duplicate, (unsigned)link_gaps, (unsigned)link_resyncs);
}
