idf_component_register(
	SRCS "app_main.c" "espnow_receiver.c" "usb_hid_device.c"
	INCLUDE_DIRS "." "../../common"
	PRIV_REQUIRES esp_event esp_netif esp_timer esp_wifi nvs_flash
)
