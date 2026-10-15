#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "usb/hid.h"
#include "usb/hid_host.h"
#include "usb/usb_host.h"

#include "keyboard_link.h"

#define TX_QUEUE_LENGTH 16
#define TX_RETRY_LIMIT 3
// ESP-NOW reports the per-frame result once its own MAC-level retries finish,
// which takes a few milliseconds. A long wait here would stall the queue and
// drop key transitions during a burst of typing.
#define TX_CALLBACK_TIMEOUT_MS 10
#define REPORT_REFRESH_MS 100

static const char *TAG = "ESPKeyboardS3";
static QueueHandle_t report_queue;
static TaskHandle_t send_task_handle;
static hid_host_device_handle_t active_keyboard;
static uint8_t paired_mac[6];
static uint8_t latest_report[KEYBOARD_LINK_REPORT_SIZE];
static uint16_t next_sequence;

static void check_esp_err(esp_err_t error, const char *operation)
{
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "%s failed: %s", operation, esp_err_to_name(error));
    }
}

static void send_callback(const esp_now_send_info_t *tx_info,
        esp_now_send_status_t status)
{
    (void)tx_info;
    if (send_task_handle != NULL) {
        xTaskNotify(send_task_handle, status == ESP_NOW_SEND_SUCCESS ? 1 : 2,
                eSetValueWithOverwrite);
    }
}

static bool send_packet(const uint8_t report[KEYBOARD_LINK_REPORT_SIZE])
{
    keyboard_link_packet_t packet = {
        .version = KEYBOARD_LINK_VERSION,
    };
    keyboard_link_set_sequence(&packet, next_sequence++);
    memcpy(packet.report, report, sizeof(packet.report));

    for (unsigned int attempt = 1; attempt <= TX_RETRY_LIMIT; ++attempt) {
        uint32_t result = 0;
        xTaskNotifyStateClear(NULL);

        esp_err_t error = esp_now_send(paired_mac, (const uint8_t *)&packet,
                sizeof(packet));
        if (error != ESP_OK) {
            ESP_LOGW(TAG, "ESP-NOW send attempt %u failed: %s",
                    attempt, esp_err_to_name(error));
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        if (xTaskNotifyWait(0, UINT32_MAX, &result,
                    pdMS_TO_TICKS(TX_CALLBACK_TIMEOUT_MS)) == pdTRUE && result == 1) {
            return true;
        }
        ESP_LOGW(TAG, "ESP-NOW send attempt %u did not report success", attempt);
    }

    ESP_LOGE(TAG, "Could not send keyboard report; receiver will resync on next report");
    return false;
}

static void report_sender_task(void *arg)
{
    uint8_t queued_report[KEYBOARD_LINK_REPORT_SIZE];
    TickType_t last_send = 0;

    (void)arg;
    send_task_handle = xTaskGetCurrentTaskHandle();
    for (;;) {
        if (xQueueReceive(report_queue, queued_report,
                    pdMS_TO_TICKS(REPORT_REFRESH_MS)) == pdTRUE) {
            memcpy(latest_report, queued_report, sizeof(latest_report));
            (void)send_packet(latest_report);
            last_send = xTaskGetTickCount();
        } else if ((TickType_t)(xTaskGetTickCount() - last_send) >=
                pdMS_TO_TICKS(REPORT_REFRESH_MS)) {
            (void)send_packet(latest_report);
            last_send = xTaskGetTickCount();
        }
    }
}

static void enqueue_keyboard_report(const uint8_t report[KEYBOARD_LINK_REPORT_SIZE])
{
    if (xQueueSend(report_queue, report, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Keyboard report queue full; input snapshot dropped");
    }
}

static void hid_interface_event_callback(hid_host_device_handle_t handle,
        const hid_host_interface_event_t event, void *arg)
{
    (void)arg;
    if (handle != active_keyboard) {
        return;
    }

    if (event == HID_HOST_INTERFACE_EVENT_INPUT_REPORT) {
        uint8_t report[64];
        size_t report_length = 0;
        esp_err_t error = hid_host_device_get_raw_input_report_data(handle,
                report, sizeof(report), &report_length);
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "Could not read keyboard report: %s", esp_err_to_name(error));
            return;
        }
        if (report_length < KEYBOARD_LINK_REPORT_SIZE) {
            ESP_LOGW(TAG, "Ignoring short boot-keyboard report (%u bytes)",
                    (unsigned)report_length);
            return;
        }
        enqueue_keyboard_report(report);
    } else if (event == HID_HOST_INTERFACE_EVENT_DISCONNECTED) {
        ESP_LOGI(TAG, "Keyboard disconnected; releasing all keys");
        uint8_t released[KEYBOARD_LINK_REPORT_SIZE] = {0};
        enqueue_keyboard_report(released);
        active_keyboard = NULL;
        check_esp_err(hid_host_device_close(handle), "Closing disconnected HID interface");
    } else if (event == HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR) {
        ESP_LOGW(TAG, "USB HID input transfer failed");
    }
}

static void hid_driver_event_callback(hid_host_device_handle_t handle,
        const hid_host_driver_event_t event, void *arg)
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
    if (params.sub_class != HID_SUBCLASS_BOOT_INTERFACE ||
            params.proto != HID_PROTOCOL_KEYBOARD) {
        ESP_LOGI(TAG, "Ignoring HID interface; a boot-protocol keyboard is required");
        return;
    }
    if (active_keyboard != NULL) {
        ESP_LOGW(TAG, "Ignoring additional keyboard interface");
        return;
    }

    const hid_host_device_config_t config = {
        .callback = hid_interface_event_callback,
        .callback_arg = NULL,
    };
    error = hid_host_device_open(handle, &config);
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

static void initialize_wifi(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    if (esp_netif_create_default_wifi_sta() == NULL) {
        ESP_LOGE(TAG, "Could not create default Wi-Fi station interface");
        abort();
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
    ESP_LOGI(TAG, "Station MAC: " MACSTR, MAC2STR(local_mac));
}

static void initialize_esp_now(void)
{
    ESP_LOGI(TAG, "Paired receiver: " MACSTR, MAC2STR(paired_mac));

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(send_callback));

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, paired_mac, sizeof(peer.peer_addr));
    peer.channel = CONFIG_ESPNOW_CHANNEL;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
}

static void initialize_nvs(void)
{
    esp_err_t error = nvs_flash_init();
    if (error == ESP_ERR_NVS_NO_FREE_PAGES ||
            error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        error = nvs_flash_init();
    }
    ESP_ERROR_CHECK(error);
}

void app_main(void)
{
    if (CONFIG_ESPNOW_CHANNEL < 1 || CONFIG_ESPNOW_CHANNEL > 13) {
        ESP_LOGE(TAG, "ESP-NOW channel must be between 1 and 13");
        return;
    }

    initialize_nvs();
    initialize_wifi();
    if (!keyboard_link_parse_mac(CONFIG_ESPNOW_PEER_MAC, paired_mac)) {
        ESP_LOGE(TAG, "Set the receiver MAC in menuconfig to enable ESP-NOW");
        return;
    }

    initialize_esp_now();
    report_queue = xQueueCreate(TX_QUEUE_LENGTH, KEYBOARD_LINK_REPORT_SIZE);
    if (report_queue == NULL) {
        ESP_LOGE(TAG, "Could not allocate the keyboard report queue");
        return;
    }
    if (xTaskCreate(report_sender_task, "keyboard_tx", 3072, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Could not create the ESP-NOW sender task");
        return;
    }

    TaskHandle_t app_task = xTaskGetCurrentTaskHandle();
    if (xTaskCreate(usb_library_task, "usb_events", 4096, app_task, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Could not create the USB host library task");
        return;
    }
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) == 0) {
        ESP_LOGE(TAG, "USB host library did not initialize");
        return;
    }

    const hid_host_driver_config_t hid_config = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = 0,
        .callback = hid_driver_event_callback,
        .callback_arg = NULL,
    };
    ESP_ERROR_CHECK(hid_host_install(&hid_config));
    ESP_LOGI(TAG, "Ready for a boot-protocol USB keyboard");
}
