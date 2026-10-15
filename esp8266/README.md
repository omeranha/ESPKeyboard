# ESPKeyboard ESP8266 receiver

Software USB low-speed HID keyboard on an ESP8266/ESP8285, built on the
[espusb](https://github.com/cnlohr/espusb) driver by Charles Lohr.

Receives keyboard reports from the paired ESP32-S3 over ESP-NOW and replays
them to a USB host.

## Wiring

- GPIO4 to USB D-
- GPIO5 to USB D+
- Common ground to USB ground
- A pull-up (>= 1k) from D- to 3.3 V

![Basic schematic](basic-schematic.png)

Pins are set in [`src/usb_config.h`](src/usb_config.h). D- must be the
lower-numbered of the two GPIOs.

## Build and flash

One-time: install the pinned toolchain and SDK. This writes only under
`C:\esp\espusb-sdk` and leaves `PATH`, ESP-IDF and any RTOS SDK untouched.

```powershell
.\install-sdk.ps1
```

Then pair with the transmitter and build. `-PeerMac` is the station MAC the
ESP32-S3 prints at boot; it is saved to the untracked `.peer-mac` so later
builds do not need it again.

```powershell
.\build.ps1 -PeerMac f0:9e:9e:21:9d:98
.\build.ps1 -Flash -Port COM6
.\build.ps1 -Clean
```

Use `-Channel` if the transmitter is not on channel 1. Both boards must agree.
Everything else lives in [`src/espkeyboard_config.h`](src/espkeyboard_config.h).

Keep `-BurnBaud` at its 115200 default; 460800 fails to enter the bootloader on
this board. The serial log is 115200 too, though the ROM bootloader's own banner
comes out at 74880, so those first lines look like noise.

There is no `make`, MSYS2 or WSL here. The upstream Makefile compiles and links
the whole firmware in one command, so `build.ps1` expands the same flags, runs
that command, then `esptool elf2image` and `write_flash`. Artefacts land in
`build/`.

## What to expect

```text
ESPKeyboard ESP8266 receiver (NONOS 1.5.4(baaeaebb))
usb: D-=GPIO4 D+=GPIO5, low speed, boot keyboard
boot ok
usb: host selected configuration 1, address 3
link: starting radio (USB configured)
link: station MAC 68:c6:3a:98:56:e1 channel 1
link: ESP-NOW up, report pump running every 2 ms
link: rx=94 ok=94 peer=0 fmt=0 dup=84 nocfg=0 gaps=0 resync=0 qfull=0 sent=10 release=0
```

The host should show one HID Keyboard Device. Pressing Caps Lock drives a
SET_REPORT, which is logged, so it proves the control endpoint works in both
directions rather than just that enumeration finished.

Counters print once a second. A high `dup` is normal: the transmitter refreshes
the current report every 100 ms whether or not anything changed, and only real
state changes are forwarded. `rx=0` means nothing is arriving, so check that
both sides use the same channel and each has the other's station MAC. `gaps`
counts frames lost over the air, `qfull` a report queue overflow during fast
typing.

If no host ever configures the device, the receive interrupt's own counters are
printed instead:

```text
usb: cfg=0 addr=0 host=0 irqs=27 setup tokens=0 packets=0
  decoder: no-high=0 no-low=0 late-lock=18 bit-timeout=0 crc-fail=9 ...
```

`irqs=0` means no packets reached GPIO5 at all, which is wiring or the pull-up.
Otherwise `late-lock` means the interrupt missed the SYNC field, `crc-fail`
means it latched on but sampled wrong, and `ok` counts packets that reached a
handler.

## How the link works

The transmitter sends an eleven-byte packet holding a protocol version, a
sequence number, and the complete eight-byte boot-keyboard report, defined in
[`../common/keyboard_link.h`](../common/keyboard_link.h). It sends one on every
change and refreshes the current state every 100 ms.

Because every packet carries the whole state rather than a delta, a lost packet
costs nothing permanent. That shapes the receive path:

- The sequence number only detects duplicates, loss and reboots. A reboot
  restarts the counter at zero, which looks like a long run of stale packets, so
  `SEQUENCE_RESYNC_AFTER` consecutive stale packets are treated as a restart.
- Reports identical to the last queued one are dropped, so the 100 ms refresh
  does not hand the host a stream of repeats.
- Distinct states are queued rather than overwritten, so a press and release
  arriving between two pump ticks are both delivered. On overflow the *oldest*
  entry is dropped.
- Reports are discarded while no host has selected a configuration, so
  unplugging during typing cannot bank a backlog and replay it on reconnect.
- After `TRANSMITTER_TIMEOUT_MS` of silence all keys are released, so losing the
  radio mid-keypress cannot leave a key held down.

The receive callback and the pump timer both run in task context, never in the
USB interrupt, and on a single core, so the queue needs no locking.

## Why the NONOS SDK

Upstream espusb states that SDKs newer than 1.5.x are incompatible. The reason
is interrupt entry latency, and it is measurable.

The decoder latches onto a packet by finding a falling edge inside the eight-bit
SYNC field, and needs an edge with two SYNC symbols still after it. The last
usable one is at SYNC bit 5, which allows about 3.3 us, or 267 CPU cycles at
80 MHz, from the triggering edge to reaching the sync search.

On the RTOS SDK the GPIO interrupt arrives through FreeRTOS: `_UserExceptionVector`
to `_xt_user_exc` to `_xt_user_entry1` to `_xt_int_enter` to `_xt_context_save`
to `_xt_isr_handler`, which iterates interrupt numbers 0 to 3 before dispatching
number 4. That is roughly 220 cycles before the handler's own prologue, and a
flash cache miss puts it over budget. The counters above were taken from that
build: 27 interrupts, 18 late-lock, 9 crc-fail, nothing decoded.

The NONOS SDK dispatches through the ROM with far less preamble, which is the
environment `PHASE_DELAY` in [`src/usb_asm_1bit.S`](src/usb_asm_1bit.S) was
tuned against.

## Notes for anyone changing this

- The bit timing assumes an 80 MHz CPU clock. `user_init()` calls
  `system_update_cpu_freq(80)`; do not remove it.
- Everything the receive interrupt touches must be in RAM, which is the default
  here: `sdk/ld/eagle.app.v6.ld` maps `.text` into `iram1_0_seg` and `.rodata`
  into `dram0_0_seg`, so plain functions are in IRAM and `static const`
  descriptors in DRAM without any attribute. `build.ps1` asserts this after
  every link and fails if anything moves to flash.
- IRAM is the tight resource: `.text` is about 30 kB of 32 kB. `build.ps1`
  reports the headroom and warns under 1 kB. New code off the interrupt path
  must be `ICACHE_FLASH_ATTR`, which is why the ESP-NOW and pump code is in
  flash.
- `usb_asm_1bit.S` reads `usb_internal_state` at hard-coded offsets from
  `usb.h`. `usb.c` has `_Static_assert`s tying the two together.
- NONOS 1.5.4 declares none of the ROM routines its own macros call, so
  [`src/esp8266_rom.h`](src/esp8266_rom.h) supplies the prototypes. Without
  them the compiler assumes `int` returns and unpromoted arguments.
- `src/user_config.h` exists only because the SDK's `osapi.h` includes it
  unconditionally.
- The internal pull-up on D- is left enabled, matching upstream, even though the
  board also has an external one. That is the configuration this hardware is
  known to enumerate with.
- ESP-NOW here is the older role-based API, so this side registers as
  `ESP_NOW_ROLE_SLAVE` with the transmitter added as `ESP_NOW_ROLE_CONTROLLER`,
  while IDF has no concept of roles.
