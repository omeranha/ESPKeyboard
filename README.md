# ESPKeyboard

ESPKeyboard turns a wired USB keyboard into a wireless one. An ESP32 reads the keyboard as a USB host and forwards every key report over one or both of two links:

- **Bluetooth LE.** The ESP32 appears as a standard HID keyboard, so a PC, phone or tablet pairs with it directly. No second board is needed.
- **ESP-NOW.** The ESP32 sends the reports to a second board plugged into the target computer, which presents itself as a USB keyboard. This works on machines that cannot pair Bluetooth, such as a BIOS screen, a KVM or a locked-down PC.

```text
                         BLE HID
                  +-------------------> PC / phone / tablet
  wired USB       |
  keyboard ---> sender (ESP32-S3)
                  |
                  |  ESP-NOW      +--> receiver-esp32    (ESP32-S3 / S2, native USB) --> PC
                  +-------------->|
                                  +--> receiver-esp8266  (software USB on two GPIOs)  --> PC
```

Both links can run at the same time. Everything except the ESP8266 receiver is built with ESP-IDF and FreeRTOS.

## Projects

| Folder | Role | Targets | Build system |
| --- | --- | --- | --- |
| [`sender/`](sender/) | USB host keyboard reader. Sends over BLE and/or ESP-NOW. | ESP32-S3 (BLE and ESP-NOW), ESP32-S2 (ESP-NOW only) | ESP-IDF |
| [`receiver-esp32/`](receiver-esp32/) | Receives ESP-NOW and acts as a USB HID keyboard. | ESP32-S3, ESP32-S2 | ESP-IDF |
| [`receiver-esp8266/`](receiver-esp8266/) | Receives ESP-NOW and acts as a low-speed USB HID keyboard, bit-banged in software. | ESP8266 / ESP8285 | NONOS SDK 1.5.4 and PowerShell |
| [`common/keyboard_link.h`](common/keyboard_link.h) | Wire format and MAC parsing shared by all three. | | |

The sender needs a chip with a USB host controller (USB OTG). The ESP32-S2 and ESP32-S3 have one. The S2 has no Bluetooth radio, so on that chip the sender is ESP-NOW only. The ESP32 receiver needs the same USB OTG block, this time in device mode.

The ESP8266 receiver stays on the NONOS SDK on purpose. Its software USB decoder only meets its timing there, and the reason is documented in [`receiver-esp8266/README.md`](receiver-esp8266/README.md).

## Requirements

- ESP-IDF 6.1 for `sender/` and `receiver-esp32/`. Both component manifests require 6.0 or newer, and 6.1 is the version they were built with.
- The IDF Component Manager downloads `usb_host_hid` and `esp_tinyusb` on the first build.
- Windows PowerShell for the ESP8266 receiver. See its README.

## Quick start

### Bluetooth only

1. Build and flash [`sender/`](sender/) for `esp32s3`. The Bluetooth transport is on by default.
2. Connect the keyboard to the board's USB OTG pins.
3. Pair "ESPKeyboard" from the PC or phone's Bluetooth settings.

### ESP-NOW with a receiver

1. Flash a receiver and read the station MAC it prints at boot.
2. Put that MAC in the sender's menuconfig, then flash the sender.
3. Read the sender's station MAC from its boot log and put it in the receiver's configuration.
4. Rebuild and flash the receiver. Both boards must use the same channel.

The sender and the ESP32 receiver both print their MAC even before they are paired, so the first flash can use the default all-zero MAC. Step-by-step instructions are in each project's README.

ESP-NOW frames are unencrypted and the pairing only checks the sender's MAC address, which can be spoofed. Use ESP-NOW in a radio environment you trust. The Bluetooth link is encrypted once paired, and a passkey can be required to pair.

## Latency

These figures come from protocol timing and from reading the code paths. They have not been measured on hardware. They count only what this project adds after the keyboard.

| Path | Average | Worst case, no retransmission | What dominates |
| --- | --- | --- | --- |
| BLE to a PC or phone | 4 to 6 ms | about 12 ms | The BLE connection interval. The sender asks the host for 7.5 to 11.25 ms and the host may refuse, in which case the host's own interval applies. |
| ESP-NOW to the ESP32 receiver, 1 Mbps (default) | about 2 ms | about 3 ms | Airtime (about 0.6 ms) and the 1 ms USB polling interval. |
| ESP-NOW to the ESP32 receiver, 24 Mbps | about 1.3 ms | about 2 ms | The 1 ms USB polling interval. |
| ESP-NOW to the ESP8266 receiver | about 7 ms | about 13 ms | The 10 ms USB polling interval of the software low-speed device, plus its 2 ms pump timer. |

The wired keyboard adds its own delay before any of this. The sender sees a key press only when the keyboard answers the host's next poll, so a keyboard with an 8 ms interval adds 4 ms on average and up to 8 ms. A 1 ms keyboard adds 0.5 ms on average. The sender cannot change that.

Where the time goes on the ESP-NOW path:

- **Airtime.** An ESP-NOW frame is about 55 bytes. At 1 Mbps with the long preamble that is about 0.6 ms, and the acknowledgement adds about 0.3 ms after the receiver already has the data. At 24 Mbps it is under 0.1 ms. The faster rate has less range and less margin in a noisy room, so it is a menuconfig choice in the sender and defaults to 1 Mbps, the ESP-NOW default. The receiver needs no setting.
- **Software.** Each hop is one FreeRTOS queue wake-up with no polling: USB host callback, transport task, `esp_now_send`, then on the receiver the Wi-Fi callback, queue, and `tud_hid_report`. The receiver sends a report straight away when the USB endpoint is idle, and otherwise waits on the endpoint's completion event instead of a timer.
- **Loss.** A frame the receiver does not acknowledge is retried immediately, up to three times. If every attempt fails, the 100 ms state refresh repairs it, so a missed release can last at most 100 ms.

What is already tuned:

- FreeRTOS tick of 1000 Hz, CPU at 240 MHz and `-O2` on both ESP32 projects.
- Wi-Fi power save off on both ends, so the radio is always listening.
- Each transport runs in its own task and queue, so a slow or missing ESP-NOW receiver cannot delay a Bluetooth report.
- A full queue drops the oldest report, never the newest, so the final key state always arrives.
- Reports identical to the last one are not resent to the computer.

What limits it, and what you can do:

- Running Bluetooth and ESP-NOW at the same time shares one radio, which adds jitter to both. Switch off the transport you do not use for the lowest latency.
- The ESP8266 receiver is bound by the 10 ms polling interval of a low-speed USB device. The USB 2.0 specification does not allow a low-speed endpoint to poll faster. Use the ESP32 receiver when latency matters.
- To measure end to end, put a logic analyzer probe on the keyboard's USB data line and another on the receiver's USB data line, and compare the timestamps. Timestamps taken on the PC cannot see the keyboard's own scan time.

## Behavior notes

- The sender forwards the keyboard's raw 8-byte boot-protocol report: modifiers, a reserved byte and six key usages. The computer on the other end applies its own keyboard layout and its own key repeat, so there is no layout table in the firmware.
- Only a keyboard that offers a boot-protocol interface is supported. Keyboards that only expose a report-protocol interface are ignored.
- Lock-key LEDs on the wired keyboard toggle locally when you press Num Lock, Caps Lock or Scroll Lock. They do not follow the state of the computer you are typing into.

## Credits

- [cnlohr/espusb](https://github.com/cnlohr/espusb) by Charles Lohr is the base of the ESP8266 software USB device. The changes are listed in [`receiver-esp8266/README.md`](receiver-esp8266/README.md).
- Espressif's ESP-IDF examples `esp_hid_device`, `usb/host/hid` and `usb/device/tusb_hid` are the reference for the BLE HID, USB host and USB device code.
- The previous Arduino version of this project used [EspUsbHost](https://github.com/tanakamasayuki/EspUsbHost) and [ESP32-BLE-Keyboard](https://github.com/T-vK/ESP32-BLE-Keyboard). They are replaced by the ESP-IDF `usb_host_hid` and `esp_hid` components.
