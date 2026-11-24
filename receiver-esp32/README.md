# ESPKeyboard ESP32 receiver

An ESP-IDF project that receives keyboard reports from the paired [`../sender/`](../sender/) over ESP-NOW and presents them to the computer as a USB HID keyboard, using the chip's USB OTG controller in device mode through TinyUSB (`esp_tinyusb`).

## Requirements and build

- ESP-IDF 6.1 (6.0 or newer)
- An ESP32-S3 or ESP32-S2 board. These are the Wi-Fi chips with a USB OTG controller, which is what a USB device needs.
- The board's USB OTG pins (D- on GPIO19, D+ on GPIO20) connected to the computer. On a DevKit-style board that is the port labelled USB, as opposed to UART.

```text
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
idf.py -p COM3 flash monitor
```

`set-target` regenerates `sdkconfig`, so run it before `menuconfig` and again whenever you change chip. The IDF Component Manager resolves `esp_tinyusb` on the first build.

Once this firmware runs, the USB OTG pins belong to the keyboard, so on the ESP32-S3 the built-in USB serial/JTAG port is gone. Use the board's UART port for `flash monitor`, or hold BOOT while pressing reset to enter download mode.

## Configuration

Open **ESPKeyboard receiver** in menuconfig. The project requires a 1000 Hz FreeRTOS tick, which `sdkconfig.defaults` sets and the build enforces.

| Option | Default | Meaning |
| --- | --- | --- |
| ESP-NOW channel | 1 | Must match the sender. Legal values are 1 to 13. |
| Paired sender station MAC address | `00:00:00:00:00:00` | The sender's station MAC. Frames from any other address are ignored, and ESP-NOW stays off while this is the all-zero default. |
| Release all keys after the sender is silent for (ms) | 1500 | The sender refreshes the current report every 100 ms, so this is about fifteen missed refreshes. |

## Pairing

At boot the receiver prints its station MAC and channel, even while no sender is configured:

```text
I (...) espnow_receiver: Station MAC: 68:c6:3a:98:56:e1, channel 1
```

Enter that address in the sender's menuconfig as its paired receiver. Read the sender's station MAC from its boot log and enter it here, then rebuild and flash. This is pairing, not authentication. ESP-NOW frames are not encrypted and a MAC address can be spoofed.

## What the computer sees

One HID keyboard with a boot-protocol interface, a 1 ms polling interval, and remote wakeup. It uses Espressif's default USB vendor and product IDs, and the serial number is the board's station MAC. The host's lock-key LED state is logged when it changes.

If the computer suspends the bus while a key arrives, the receiver asks the host to wake up when the host has enabled remote wakeup.

## How the link works

The sender's eleven-byte packet holds a protocol version, a sequence number and the full eight-byte report, defined in [`../common/keyboard_link.h`](../common/keyboard_link.h). The receive path handles that the same way as the ESP8266 receiver:

- The sequence number only detects duplicates, loss and sender reboots. A reboot restarts the counter at zero, so eight consecutive stale packets count as a restart rather than being discarded forever.
- A report is sent to the computer as soon as it arrives when the USB endpoint is idle. When the endpoint is busy the receiver waits for its completion event, not for a timer.
- Distinct reports are queued rather than overwritten, so a press and a release that arrive close together are both delivered. When the queue of eight fills, the oldest entry is dropped.
- A report identical to the one already sent is not sent again, so the sender's 100 ms refresh does not produce repeats.
- Reports are discarded while the computer has not enumerated the device, so unplugging during typing cannot bank keys and replay them on reconnect.
- After the configured silence all keys are released, so losing the radio mid-keypress cannot leave a key held down.

Every five seconds the receiver logs link counters:

```text
I (...) espnow_receiver: rx=94 ok=94 peer=0 fmt=0 dup=84 gaps=0 resync=0
```

A high `dup` is normal. `rx=0` means nothing is arriving, so check that both boards use the same channel and each has the other's MAC. `peer` counts frames from a different MAC and `gaps` counts frames lost over the air.

## Code layout

| File | Responsibility |
| --- | --- |
| `app_main.c` | Report queue, the pump task that feeds the USB endpoint, and the sender timeout. |
| `espnow_receiver.c` | Wi-Fi station setup, peer filtering, sequence handling and counters. |
| `usb_hid_device.c` | TinyUSB descriptors, HID callbacks and report sending. |
