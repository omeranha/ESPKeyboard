# ESPKeyboard

ESPKeyboard bridges a USB keyboard to a computer over a low-latency ESP-NOW
link using two boards:

- `esp32-s3/` reads a boot-protocol USB keyboard as a USB host and transmits
  complete keyboard reports.
- `esp8266/` receives the reports and presents them to the computer as a
  low-speed USB HID keyboard.
- `common/keyboard_link.h` defines the shared wireless packet format.

Build and pairing instructions are in each board project's README.
