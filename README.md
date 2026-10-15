# ESPKeyboard

ESPKeyboard bridges a USB keyboard to a computer over a low-latency ESP-NOW
link using two boards:

- [`esp32-s3/`](esp32-s3/) reads a boot-protocol USB keyboard as a USB host and
  transmits complete keyboard reports.
- [`esp8266/`](esp8266/) receives the reports and presents them to the computer
  as a low-speed USB HID keyboard, bit-banged in software on two GPIOs.
- [`common/keyboard_link.h`](common/keyboard_link.h) defines the shared
  wireless packet format.

Build and pairing instructions are in each board project's README.

## Changes from the original ESPUSB project

ESPKeyboard keeps and adapts the ESP8266 software-USB device code from
[cnlohr/espusb](https://github.com/cnlohr/espusb), but reshapes the repository
into a two-board wireless keyboard bridge.

### Removed

- The web interface, web server, and web-based firmware update path.
- The HID mouse interface, its report descriptor, and its endpoint. The device
  enumerates as a keyboard only.
- The legacy Make-based builds, standalone bootloader, and old host/demo
  utilities. The ESP8266 firmware is built by a PowerShell script that issues
  the same single compile-and-link command the upstream Makefile did, so no
  make, MSYS2 or WSL is needed on Windows.
- The `esp82xx` submodule and obsolete configuration, notes, and assets that
  were only used by those legacy projects.
- The optional `hardware/` board design files and `table/` decoding-table
  generator. The checked-in USB decoding table remains with the ESP8266
  firmware.

### Changed and added

- Rebuilt the ESP8266 firmware around the ESP8266 NONOS SDK 1.5.4 with a
  Windows-native build script, deliberately *not* the RTOS SDK. The software USB
  driver cannot meet its timing on the RTOS SDK: the decoder has to latch onto a
  falling edge inside the eight-bit SYNC field, roughly 3.3 us or 267 CPU cycles
  at 80 MHz, and FreeRTOS's level-1 interrupt entry spends about 220 cycles in
  context saving and interrupt dispatch before the handler runs. The
  ESP8266 receiver's own counters showed the result exactly: every packet either
  arrived too late to decode or decoded as garbage, and the device never
  enumerated.
- Kept the low-speed, bit-banged USB HID device with upstream's GPIO and
  interrupt setup intact, since that is the configuration the hardware is known
  to enumerate with.
- Completed the device's control endpoint: standard `GET_STATUS`,
  `GET_CONFIGURATION` and `GET_INTERFACE`, the HID `GET_REPORT`, `GET_IDLE` and
  `GET_PROTOCOL` requests, LED state capture from `SET_REPORT`, and the data
  toggle reset that `SET_CONFIGURATION` and `CLEAR_FEATURE(ENDPOINT_HALT)`
  require. `SET_ADDRESS` now takes effect on the status stage acknowledgement
  rather than when the request arrives, as USB 2.0 9.4.6 requires.
- Added diagnostic counters inside the receive interrupt that record why a
  packet was rejected, so a device that will not enumerate reports which stage
  of reception failed instead of only that nothing arrived.
- Added an ESP-NOW receiver to the ESP8266 firmware. It validates its paired
  transmitter, sequences complete keyboard-state reports, forwards them to the
  USB keyboard endpoint, and releases keys after a transmitter timeout. The
  radio is held off until USB has enumerated, because Wi-Fi interrupts are the
  most likely thing to disturb the bit-banged timing.
- Added an ESP32-S3 transmitter using ESP-IDF's native USB Host HID class
  driver. It reads a USB boot-protocol keyboard and sends complete keyboard
  reports over ESP-NOW, with an optional test mode that synthesises a keystroke
  so the link can be exercised without a keyboard attached.
- Added shared packet-format code, per-board channel/MAC pairing settings,
  build/install guidance, and documentation for USB, pairing, and current
  limitations.
