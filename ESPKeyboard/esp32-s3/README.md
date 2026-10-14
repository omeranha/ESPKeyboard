# ESPKeyboard ESP32-S3 transmitter

This ESP-IDF project uses the native USB Host library and Espressif's
`usb_host_hid` class driver. It accepts one USB HID keyboard that exposes a
boot-protocol keyboard interface, normalizes its 8-byte boot report, and sends
complete sequenced state snapshots to the paired ESP8266 over ESP-NOW.

## Requirements and build

- ESP-IDF 6.1 or newer
- ESP32-S3 with its USB OTG host port wired to a powered USB-A receptacle
- An ESP8266/ESP8285 receiver running [`../esp8266/`](../esp8266/)

The IDF Component Manager resolves `usb_host_hid` 1.2.1 from the component
registry on the first build.

```text
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
idf.py -p COM3 flash monitor
```

In **ESPKeyboard ESP-NOW link**, configure the same legal Wi-Fi channel used by
the receiver and set the paired receiver's Station MAC address. The default
MAC is invalid so the firmware will not start the bridge until pairing is
configured. The firmware prints this board's Station MAC at startup; enter
that value in the receiver's menuconfig (`.\build.ps1 -Menuconfig`) as its
paired transmitter MAC.

## USB input support

Only HID interfaces advertising the boot subclass and keyboard protocol are
opened. The S3 requests boot protocol and idle reports, and forwards the
complete eight-byte report: modifiers, reserved byte, and six key usages.
Non-boot/report-protocol keyboards and additional keyboard interfaces are
ignored. Supporting arbitrary HID report descriptors requires a report
descriptor parser and is not part of this initial bridge.

The USB Host driver owns USB host event processing; this project does not mix
TinyUSB host calls with ESP-IDF's USB Host stack. Supply a stable 5 V VBUS to
the keyboard and common ground. Follow the ESP32-S3 board's USB OTG pinout
(native D- on GPIO19 and D+ on GPIO20); do not power a keyboard directly from
the board unless its VBUS circuit is rated for the keyboard's current.
