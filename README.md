# ESPKeyboard

This repository contains the two firmware projects for a wireless USB keyboard
bridge:

- [`ESPKeyboard/esp32-s3/`](ESPKeyboard/esp32-s3/) is the USB-host transmitter.
- [`ESPKeyboard/esp8266/`](ESPKeyboard/esp8266/) is the USB-device receiver.
- [`ESPKeyboard/common/`](ESPKeyboard/common/) defines the ESP-NOW report
  protocol shared by both projects.

Start with [`ESPKeyboard/README.md`](ESPKeyboard/README.md), then follow each
board project's build and pairing instructions. Existing USB hardware files
remain under [`hardware/`](hardware/).

The ESP8266 USB-device implementation was adapted from
[omeranha/espusb](https://github.com/omeranha/espusb).
