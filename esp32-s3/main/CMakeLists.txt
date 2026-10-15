idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_event esp_netif esp_wifi nvs_flash usb_host_hid
)
