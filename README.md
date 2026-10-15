# ESPKeyboard

This repository contains the two firmware projects for a wireless USB keyboard
bridge:

- [`ESPKeyboard/esp32-s3/`](ESPKeyboard/esp32-s3/) is the USB-host transmitter.
- [`ESPKeyboard/esp8266/`](ESPKeyboard/esp8266/) is the USB-device receiver.
- [`ESPKeyboard/common/`](ESPKeyboard/common/) defines the ESP-NOW report
  protocol shared by both projects.

Start with [`ESPKeyboard/README.md`](ESPKeyboard/README.md), then follow each
board project's build and pairing instructions.

## Changes from the original ESPUSB project

ESPKeyboard keeps and adapts the ESP8266 software-USB device code from
[cnlohr/espusb](https://github.com/cnlohr/espusb), but reshapes the repository
into a two-board wireless keyboard bridge.

### Removed

- The web interface, web server, and web-based firmware update path.
- The HID mouse interface, its report descriptor, and its endpoint. The device
  enumerates as a keyboard only.
- The legacy Make-based builds, standalone bootloader, and old host/demo
  utilities.
- The `esp82xx` submodule and obsolete configuration, notes, and assets that
  were only used by those legacy projects.
- The optional `hardware/` board design files and `table/` decoding-table
  generator. The checked-in USB decoding table remains with the ESP8266
  firmware.

### Changed and added

- Migrated the ESP8266 firmware to ESP8266 RTOS SDK 3.4.0 and its CMake
  project/component build system.
- Kept the low-speed, bit-banged USB HID device, and adapted its GPIO/interrupt
  setup and timing-sensitive assembly for the RTOS SDK toolchain. The RTOS SDK
  needs the GPIO interrupt line unmasked explicitly and links `.rodata` into
  flash-mapped instruction space, so the interrupt is enabled by hand and the
  USB descriptors are pinned to DRAM.
- Completed the device's control endpoint: standard `GET_STATUS`,
  `GET_CONFIGURATION` and `GET_INTERFACE`, the HID `GET_REPORT`, `GET_IDLE` and
  `GET_PROTOCOL` requests, LED state capture from `SET_REPORT`, and the data
  toggle reset that `SET_CONFIGURATION` and `CLEAR_FEATURE(ENDPOINT_HALT)`
  require.
- Added an ESP-NOW receiver to the ESP8266 firmware. It validates its paired
  transmitter, sequences complete keyboard-state reports, forwards them to
  the USB keyboard endpoint, and releases keys after a transmitter timeout.
- Added an ESP32-S3 transmitter using ESP-IDF's native USB Host HID class
  driver. It reads a USB boot-protocol keyboard and sends complete keyboard
  reports over ESP-NOW.
- Added shared packet-format code, per-board channel/MAC pairing settings,
  build/install guidance, and documentation for USB, pairing, and current
  limitations.
