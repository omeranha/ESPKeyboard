#include "transport_ble.h"

#include <string.h>

#include "esp_bt.h"
#include "esp_hidd.h"
#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"

#define KEYBOARD_REPORT_ID 1
#define BATTERY_LEVEL_PERCENT 100
#define ADV_INTERVAL_MIN_MS 30
#define ADV_INTERVAL_MAX_MS 50
#define HID_SERVICE_UUID16 0x1812
#define CONN_INTERVAL_MIN_UNITS 6
#define CONN_INTERVAL_MAX_UNITS 9
#define CONN_SLAVE_LATENCY 0
#define CONN_SUPERVISION_TIMEOUT_UNITS 200

void ble_store_config_init(void);

static const char *TAG = "ble_link";
static esp_hidd_dev_t *hid_device;
static volatile bool link_encrypted;

static const uint8_t keyboard_report_map[] = {
	0x05, 0x01,       // Usage Page (Generic Desktop)
	0x09, 0x06,       // Usage (Keyboard)
	0xA1, 0x01,       // Collection (Application)
	0x85, KEYBOARD_REPORT_ID,
	0x05, 0x07,       //   Usage Page (Keyboard/Keypad)
	0x19, 0xE0,       //   Usage Minimum (Left Control)
	0x29, 0xE7,       //   Usage Maximum (Right GUI)
	0x15, 0x00,       //   Logical Minimum (0)
	0x25, 0x01,       //   Logical Maximum (1)
	0x75, 0x01,       //   Report Size (1)
	0x95, 0x08,       //   Report Count (8)
	0x81, 0x02,       //   Input (Data, Variable, Absolute): modifier bitmap
	0x95, 0x01,       //   Report Count (1)
	0x75, 0x08,       //   Report Size (8)
	0x81, 0x01,       //   Input (Constant): reserved byte
	0x95, 0x05,       //   Report Count (5)
	0x75, 0x01,       //   Report Size (1)
	0x05, 0x08,       //   Usage Page (LEDs)
	0x19, 0x01,       //   Usage Minimum (Num Lock)
	0x29, 0x05,       //   Usage Maximum (Kana)
	0x91, 0x02,       //   Output (Data, Variable, Absolute): LED bitmap
	0x95, 0x01,       //   Report Count (1)
	0x75, 0x03,       //   Report Size (3)
	0x91, 0x01,       //   Output (Constant): LED padding
	0x95, 0x06,       //   Report Count (6)
	0x75, 0x08,       //   Report Size (8)
	0x15, 0x00,       //   Logical Minimum (0)
	0x26, 0xFF, 0x00, //   Logical Maximum (255)
	0x05, 0x07,       //   Usage Page (Keyboard/Keypad)
	0x19, 0x00,       //   Usage Minimum (0)
	0x2A, 0xFF, 0x00, //   Usage Maximum (255)
	0x81, 0x00,       //   Input (Data, Array): six key usages
	0xC0,             // End Collection
};

static esp_hid_raw_report_map_t report_maps[] = {
	{.data = keyboard_report_map, .len = sizeof(keyboard_report_map)},
};

static esp_hid_device_config_t hid_config = {
	.vendor_id = 0x16C0,
	.product_id = 0x05DF,
	.version = 0x0100,
	.device_name = CONFIG_ESPKEYBOARD_BLE_DEVICE_NAME,
	.manufacturer_name = "ESPKeyboard",
	.serial_number = "0001",
	.report_maps = report_maps,
	.report_maps_len = 1,
};

static int gap_event_handler(struct ble_gap_event *event, void *arg);

// The host picks the connection interval, and a key report waits for the next connection event. Ask for 7.5 to 11.25 ms with no slave latency; the host is free to refuse.
static void request_short_connection_interval(uint16_t connection_handle)
{
	const struct ble_gap_upd_params params = {
		.itvl_min = CONN_INTERVAL_MIN_UNITS,
		.itvl_max = CONN_INTERVAL_MAX_UNITS,
		.latency = CONN_SLAVE_LATENCY,
		.supervision_timeout = CONN_SUPERVISION_TIMEOUT_UNITS,
	};
	int rc = ble_gap_update_params(connection_handle, &params);
	if (rc != 0) {
		ESP_LOGW(TAG, "Could not request a shorter connection interval: %d", rc);
	}
}

static void start_advertising(void)
{
	struct ble_hs_adv_fields fields = {0};
	static const ble_uuid16_t hid_service_uuid = BLE_UUID16_INIT(HID_SERVICE_UUID16);

	fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
	fields.appearance = ESP_HID_APPEARANCE_KEYBOARD;
	fields.appearance_is_present = 1;
	fields.name = (const uint8_t *)CONFIG_ESPKEYBOARD_BLE_DEVICE_NAME;
	fields.name_len = strlen(CONFIG_ESPKEYBOARD_BLE_DEVICE_NAME);
	fields.name_is_complete = 1;
	fields.uuids16 = &hid_service_uuid;
	fields.num_uuids16 = 1;
	fields.uuids16_is_complete = 1;

	int rc = ble_gap_adv_set_fields(&fields);
	if (rc != 0) {
		ESP_LOGE(TAG, "Could not set advertising data: %d", rc);
		return;
	}

	struct ble_gap_adv_params adv_params = {0};
	adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
	adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
	adv_params.itvl_min = BLE_GAP_ADV_ITVL_MS(ADV_INTERVAL_MIN_MS);
	adv_params.itvl_max = BLE_GAP_ADV_ITVL_MS(ADV_INTERVAL_MAX_MS);

	rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &adv_params, gap_event_handler, NULL);
	if (rc != 0) {
		ESP_LOGE(TAG, "Could not start advertising: %d", rc);
	}
}

static int gap_event_handler(struct ble_gap_event *event, void *arg)
{
	struct ble_gap_conn_desc desc;
	struct ble_sm_io io = {0};

	(void)arg;
	switch (event->type) {
	case BLE_GAP_EVENT_CONNECT:
		if (event->connect.status != 0) {
			ESP_LOGW(TAG, "Connection failed: %d", event->connect.status);
			start_advertising();
			return 0;
		}
		ESP_LOGI(TAG, "Host connected; requesting encryption");
		// The HID service does not demand encryption itself, so ask for it here. Reports are held back until it is up.
		if (ble_gap_security_initiate(event->connect.conn_handle) != 0) {
			ESP_LOGW(TAG, "Could not start security procedure");
		}
		return 0;

	case BLE_GAP_EVENT_DISCONNECT:
		ESP_LOGI(TAG, "Host disconnected (reason %d)", event->disconnect.reason);
		link_encrypted = false;
		start_advertising();
		return 0;

	case BLE_GAP_EVENT_ADV_COMPLETE:
		start_advertising();
		return 0;

	case BLE_GAP_EVENT_ENC_CHANGE:
		link_encrypted = event->enc_change.status == 0;
		if (link_encrypted) {
			ESP_LOGI(TAG, "Link encrypted; keyboard reports enabled");
			request_short_connection_interval(event->enc_change.conn_handle);
		} else {
			ESP_LOGW(TAG, "Encryption failed (status %d)", event->enc_change.status);
		}
		return 0;

	case BLE_GAP_EVENT_REPEAT_PAIRING:
		// The host lost its bond and is pairing again, so drop ours and let it.
		if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
			ble_store_util_delete_peer(&desc.peer_id_addr);
		}
		return BLE_GAP_REPEAT_PAIRING_RETRY;

	case BLE_GAP_EVENT_PASSKEY_ACTION:
		if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
			io.action = BLE_SM_IOACT_DISP;
			io.passkey = CONFIG_ESPKEYBOARD_BLE_PASSKEY;
			ESP_LOGI(TAG, "Enter passkey %06d on the host", CONFIG_ESPKEYBOARD_BLE_PASSKEY);
			ble_sm_inject_io(event->passkey.conn_handle, &io);
		}
		return 0;

	default:
		return 0;
	}
}

static void hid_event_callback(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
	(void)handler_args;
	(void)base;
	(void)event_data;

	switch ((esp_hidd_event_t)id) {
	case ESP_HIDD_START_EVENT:
		ESP_LOGI(TAG, "Advertising as \"%s\"", CONFIG_ESPKEYBOARD_BLE_DEVICE_NAME);
		start_advertising();
		break;
	default:
		break;
	}
}

static void nimble_host_task(void *param)
{
	(void)param;
	nimble_port_run();
	nimble_port_freertos_deinit();
}

static void configure_security(void)
{
	ble_hs_cfg.sm_bonding = 1;
	ble_hs_cfg.sm_sc = 1;
	ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ID | BLE_SM_PAIR_KEY_DIST_ENC;
	ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ID | BLE_SM_PAIR_KEY_DIST_ENC;
	if (CONFIG_ESPKEYBOARD_BLE_PASSKEY == 0) {
		ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
		ble_hs_cfg.sm_mitm = 0;
	} else {
		ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_ONLY;
		ble_hs_cfg.sm_mitm = 1;
	}
}

esp_err_t ble_link_start(void)
{
	esp_bt_controller_config_t controller_config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();

	esp_err_t error = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
	if (error == ESP_OK) {
		error = esp_bt_controller_init(&controller_config);
	}
	if (error == ESP_OK) {
		error = esp_bt_controller_enable(ESP_BT_MODE_BLE);
	}
	if (error == ESP_OK) {
		error = esp_nimble_init();
	}
	if (error != ESP_OK) {
		ESP_LOGE(TAG, "Bluetooth controller start failed: %s", esp_err_to_name(error));
		return error;
	}

	configure_security();

	error = esp_hidd_dev_init(&hid_config, ESP_HID_TRANSPORT_BLE, hid_event_callback, &hid_device);
	if (error != ESP_OK) {
		ESP_LOGE(TAG, "HID device init failed: %s", esp_err_to_name(error));
		return error;
	}
	esp_hidd_dev_battery_set(hid_device, BATTERY_LEVEL_PERCENT);
	ble_svc_gap_device_name_set(CONFIG_ESPKEYBOARD_BLE_DEVICE_NAME);
	ble_svc_gap_device_appearance_set(ESP_HID_APPEARANCE_KEYBOARD);

	ble_store_config_init();
	ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
	nimble_port_freertos_init(nimble_host_task);
	return ESP_OK;
}

void ble_link_send(const uint8_t report[KEYBOARD_LINK_REPORT_SIZE])
{
	if (hid_device == NULL || !link_encrypted) {
		return;
	}

	uint8_t data[KEYBOARD_LINK_REPORT_SIZE];
	memcpy(data, report, sizeof(data));
	esp_err_t error = esp_hidd_dev_input_set(hid_device, 0, KEYBOARD_REPORT_ID, data, sizeof(data));
	if (error != ESP_OK) {
		ESP_LOGW(TAG, "Could not send keyboard report: %s", esp_err_to_name(error));
	}
}
