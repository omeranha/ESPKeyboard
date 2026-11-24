set(srcs "app_main.c" "usb_keyboard.c")

if(CONFIG_ESPKEYBOARD_TRANSPORT_ESPNOW)
	list(APPEND srcs "transport_espnow.c")
endif()

if(CONFIG_ESPKEYBOARD_TRANSPORT_BLE)
	list(APPEND srcs "transport_ble.c")
endif()

if(CONFIG_ESPKEYBOARD_TEST_TYPER)
	list(APPEND srcs "test_typer.c")
endif()

idf_component_register(
	SRCS ${srcs}
	INCLUDE_DIRS "." "../../common"
	PRIV_REQUIRES bt esp_event esp_hid esp_netif esp_wifi nvs_flash usb_host_hid
)
