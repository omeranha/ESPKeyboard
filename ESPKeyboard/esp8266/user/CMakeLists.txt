idf_component_register(
    SRCS "usb.c" "usb_asm_1bit.S"
    INCLUDE_DIRS "."
    REQUIRES esp8266 freertos
)
