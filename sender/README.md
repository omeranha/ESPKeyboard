# ESPKeyboard sender

An ESP-IDF project that reads one USB keyboard with the native USB Host library and Espressif's `usb_host_hid` class driver, and forwards its reports over Bluetooth LE HID, ESP-NOW, or both.

The sender normalizes each keyboard report to the 8-byte boot format (modifiers, a reserved byte, six key usages). Every ESP-NOW packet carries a complete, sequenced snapshot of that state, so a lost frame costs nothing permanent.

## Requirements and build

- ESP-IDF 6.1 (6.0 or newer)
- An ESP32-S3 or ESP32-S2 board whose USB OTG pins go to a powered USB-A receptacle. The ESP32-S2 has no Bluetooth, so it only supports ESP-NOW.
- For ESP-NOW, a receiver running [`../receiver-esp32/`](../receiver-esp32/) or [`../receiver-esp8266/`](../receiver-esp8266/)

```text
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
idf.py -p COM3 flash monitor
```

`set-target` regenerates `sdkconfig`, so run it before `menuconfig` and again whenever you change chip. The IDF Component Manager resolves `usb_host_hid` on the first build. On the ESP32-S3 the defaults in `sdkconfig.defaults.esp32s3` turn on the NimBLE stack and its HID service.

## Configuration

Open **ESPKeyboard sender** in menuconfig.

| Option | Default | Meaning |
| --- | --- | --- |
| Send over ESP-NOW | on | Enables the ESP-NOW transport. |
| ESP-NOW channel | 1 | Must match the receiver. Legal values are 1 to 13. |
| ESP-NOW data rate | 1 Mbps | 24 Mbps shortens each frame on the air from about 0.6 ms to under 0.1 ms, at the cost of range and margin. |
| Paired receiver station MAC address | `00:00:00:00:00:00` | The receiver's station MAC. ESP-NOW stays off while this is the all-zero default. |
| Act as a Bluetooth LE HID keyboard | on | Enables the BLE transport. Only offered on chips with Bluetooth. |
| Bluetooth device name | `ESPKeyboard` | The name hosts see when scanning. |
| Bluetooth pairing passkey | `0` | `0` pairs with no code. `1` to `999999` makes the host ask for that six-digit code. |
| Send a test keystroke periodically | off | Bring-up aid. See below. |

The pairing options and the test typer are not set in `sdkconfig.defaults`. They live in the untracked `sdkconfig`, so a MAC address or passkey does not end up in version control.

The two transports are independent. With BLE only, leave ESP-NOW switched off and no receiver is needed. With ESP-NOW only, switch BLE off to leave the Bluetooth radio idle.

## Pairing over ESP-NOW

At boot the sender prints its own station MAC and channel, even while no receiver is configured:

```text
I (...) espnow_link: Station MAC: f0:9e:9e:21:9d:98, channel 1
```

Enter that address in the receiver's configuration. Then read the receiver's station MAC from its boot log and enter it here. Frames from any other address are ignored by the receiver. This is pairing, not authentication: ESP-NOW frames are not encrypted here and a MAC address can be spoofed.

The sender sends a packet on every change and refreshes the current state every 100 ms. Each send is retried up to three times when the receiver does not acknowledge it.

## Pairing over Bluetooth

The sender advertises as a HID keyboard under the configured name. Pair it from the host's Bluetooth settings.

- The sender requests encryption as soon as a host connects and sends no key reports until the link is encrypted. A host that connects but never pairs receives nothing.
- Bonds are stored in flash, so the host reconnects by itself after a power cycle. Only one host is connected at a time.
- With a passkey configured the host asks for the six-digit code. With `0` the pairing needs no confirmation, which means anyone in range can pair while the keyboard is advertising.
- If you remove the sender on the host and pair again, the sender replaces its old bond automatically. `idf.py erase-flash` clears every stored bond.

Bluetooth notifications are reliable, so BLE reports are sent only when the keyboard state changes. After encryption the sender asks the host for a 7.5 to 11.25 ms connection interval, because a report waits for the next connection event. The host decides, so the interval you get depends on the host.

## USB keyboard input

Only HID interfaces that advertise the boot subclass and keyboard protocol are opened. The sender requests boot protocol and a zero idle rate, and forwards the full eight-byte report. Keyboards that only offer a report-protocol interface, and additional keyboard interfaces, are ignored. Supporting arbitrary report descriptors would need a descriptor parser.

Pressing Num Lock, Caps Lock or Scroll Lock toggles the matching LED on the keyboard. The state is tracked on the sender, so the LEDs do not follow the computer you are typing into.

The sender does not apply a keyboard layout or key repeat. The receiving host does both.

Supply a stable 5 V VBUS to the keyboard and a common ground. Use the board's USB OTG pins (D- on GPIO19, D+ on GPIO20 for both chips), and do not power a keyboard from the board unless its VBUS circuit is rated for the keyboard's current.

On the ESP32-S3 the USB OTG pins are also the pins of the built-in USB serial/JTAG port, so with a keyboard on them you need the board's UART port for `flash monitor`. A board without one can enter download mode by holding BOOT while pressing reset.

## Test typer

**Send a test keystroke periodically** sends the letter `a` every `ESPKEYBOARD_TEST_TYPER_INTERVAL_MS` milliseconds, so the links and the receiver's USB keyboard can be exercised without a keyboard attached. It pauses while a real keyboard is connected. Each cycle sends a press, holds it for 50 ms, then always sends a release.

Turn it off for normal use: it types into whatever computer the receiver is plugged into.

## Code layout

| File | Responsibility |
| --- | --- |
| `app_main.c` | Starts the transports and the USB host. Each transport has its own queue and task, so a slow one never delays the other. |
| `usb_keyboard.c` | USB host and HID driver handling, keyboard LEDs. |
| `transport_espnow.c` | Wi-Fi station setup, ESP-NOW peer, sequenced packets with retry. |
| `transport_ble.c` | NimBLE advertising, security, and the HID-over-GATT keyboard. |
| `test_typer.c` | The optional test typer. |
