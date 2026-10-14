#include "usb.h"
#include "keyboard_link.h"

#include <string.h>

#include "esp_event.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "tcpip_adapter.h"

static const char *TAG = "ESPKeyboard8266";
static QueueHandle_t report_queue;
static uint8_t paired_mac[6];
static volatile TickType_t last_packet_tick;

static esp_err_t wifi_event_callback(void *ctx, system_event_t *event)
{
    (void)ctx;
    (void)event;
    return ESP_OK;
}

void IRAM_ATTR usb_handle_custom_control(int bmRequestType, int bRequest,
        int wLength, struct usb_internal_state_struct *ist)
{
    (void)bmRequestType;
    (void)bRequest;
    (void)wLength;
    (void)ist;
}

static bool mac_matches(const uint8_t *left, const uint8_t *right)
{
    return memcmp(left, right, 6) == 0;
}

static void esp_now_receive_callback(const uint8_t *mac_addr, const uint8_t *data,
        int data_len)
{
    if (mac_addr == NULL || data == NULL ||
            data_len != sizeof(keyboard_link_packet_t) ||
            !mac_matches(mac_addr, paired_mac)) {
        return;
    }

    keyboard_link_packet_t packet;
    memcpy(&packet, data, sizeof(packet));
    if (packet.version != KEYBOARD_LINK_VERSION) {
        return;
    }

    if (xQueueSend(report_queue, &packet, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Input queue full; keyboard report dropped");
        return;
    }
    last_packet_tick = xTaskGetTickCount();
}

static void initialize_wifi(void)
{
    tcpip_adapter_init();
    ESP_ERROR_CHECK(esp_event_loop_init(wifi_event_callback, NULL));

    const wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_config));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_channel(CONFIG_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));

    uint8_t local_mac[6];
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, local_mac));
    ESP_LOGI(TAG, "Station MAC: " MACSTR, MAC2STR(local_mac));
}

static void initialize_esp_now(void)
{
    ESP_LOGI(TAG, "Paired transmitter: " MACSTR, MAC2STR(paired_mac));

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(esp_now_receive_callback));

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, paired_mac, sizeof(peer.peer_addr));
    peer.channel = CONFIG_ESPNOW_CHANNEL;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
}

static bool sequence_is_newer(uint16_t candidate, uint16_t previous)
{
    return (int16_t)(candidate - previous) > 0;
}

static void keyboard_report_task(void *arg)
{
    keyboard_link_packet_t packet;
    uint8_t current_report[KEYBOARD_LINK_REPORT_SIZE] = {0};
    bool have_sequence = false;
    bool report_pending = true;
    uint16_t last_sequence = 0;

    (void)arg;
    for (;;) {
        if (!report_pending && xQueueReceive(report_queue, &packet, 1) == pdTRUE) {
            uint16_t sequence = keyboard_link_get_sequence(&packet);
            if (have_sequence && !sequence_is_newer(sequence, last_sequence)) {
                continue;
            }
            if (have_sequence && (uint16_t)(sequence - last_sequence) != 1) {
                ESP_LOGW(TAG, "ESP-NOW sequence gap: %u -> %u",
                        (unsigned)last_sequence, (unsigned)sequence);
            }
            have_sequence = true;
            last_sequence = sequence;
            if (memcmp(current_report, packet.report, sizeof(current_report)) != 0) {
                memcpy(current_report, packet.report, sizeof(current_report));
                report_pending = true;
            }
        }

        TickType_t now = xTaskGetTickCount();
        if (memcmp(current_report, (uint8_t[KEYBOARD_LINK_REPORT_SIZE]){0},
                    sizeof(current_report)) != 0 &&
                (TickType_t)(now - last_packet_tick) >= pdMS_TO_TICKS(1500)) {
            memset(current_report, 0, sizeof(current_report));
            xQueueReset(report_queue);
            report_pending = true;
            ESP_LOGW(TAG, "Transmitter timed out; releasing all keys");
        }

        if (report_pending) {
            esp_err_t err = usb_send_keyboard_report(current_report);
            if (err == ESP_OK) {
                report_pending = false;
            } else if (err != ESP_ERR_INVALID_STATE) {
                ESP_LOGE(TAG, "USB keyboard report failed: %s", esp_err_to_name(err));
                vTaskDelay(pdMS_TO_TICKS(10));
            } else {
                vTaskDelay(1);
            }
        }
    }
}

void app_main(void)
{
    if (CONFIG_ESPNOW_CHANNEL < 1 || CONFIG_ESPNOW_CHANNEL > 13) {
        ESP_LOGE(TAG, "ESP-NOW channel must be between 1 and 13");
        return;
    }

    report_queue = xQueueCreate(16, sizeof(keyboard_link_packet_t));
    if (report_queue == NULL) {
        ESP_LOGE(TAG, "Could not allocate the keyboard report queue");
        return;
    }

    initialize_wifi();
    if (!keyboard_link_parse_mac(CONFIG_ESPNOW_PEER_MAC, paired_mac)) {
        ESP_LOGE(TAG, "Set the transmitter MAC in menuconfig to enable ESP-NOW");
        return;
    }

    initialize_esp_now();
    ESP_ERROR_CHECK(usb_init());
    if (xTaskCreate(keyboard_report_task, "keyboard_report", 3072, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Could not create the keyboard report task");
    }
}
