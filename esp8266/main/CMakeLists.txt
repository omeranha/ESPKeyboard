idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES user pthread esp8266 tcpip_adapter esp_event
)
