# ESPKeyboard ESP8266 receiver

Software USB low-speed HID keyboard on an ESP8266/ESP8285, built on the
[espusb](https://github.com/cnlohr/espusb) driver by Charles Lohr.

Built against the ESP8266 NONOS SDK 1.5.4 rather than the RTOS SDK, which
cannot meet the USB timing. See [Why not the RTOS SDK](#why-not-the-rtos-sdk).

**Current state: stages 1 to 4.** USB enumerates, and keyboard reports arriving
over ESP-NOW are replayed to the host. Stage 5, confirming the radio does not
break enumeration or sustained typing, is what needs testing now.

## Configuration

There is no menuconfig on this SDK. Edit
[`src/espkeyboard_config.h`](src/espkeyboard_config.h) and rebuild:

| Setting | Meaning |
| --- | --- |
| `ESPNOW_CHANNEL` | Must match `CONFIG_ESPNOW_CHANNEL` on the ESP32-S3 |
| `ESPNOW_PEER_MAC` | Station MAC of the paired ESP32-S3 |
| `TRANSMITTER_TIMEOUT_MS` | Release all keys after this much silence |
| `SEQUENCE_RESYNC_AFTER` | Stale packets tolerated before assuming a transmitter reboot |
| `REPORT_PUMP_INTERVAL_MS` | How often a queued report is handed to the endpoint |
| `RADIO_START_GRACE_MS` | Upper bound on holding the radio off for USB to enumerate |

`ESPNOW_PEER_MAC` is prefilled with `f0:9e:9e:21:9d:98`, the transmitter MAC
from this board's own logs; change it for other hardware. An unset, broadcast
or multicast address is rejected at startup and the radio stays off, rather
than accepting reports from whatever happens to be transmitting.

The ESP32-S3's `CONFIG_ESPNOW_PEER_MAC` must in turn be this board's station
MAC, which it prints at startup, and both sides must agree on the channel.

ESP-NOW frames here are unencrypted, and a MAC filter is pairing rather than
authentication. Treat the radio link as trusted-environment only.

## Wiring

Unchanged from the RTOS receiver, and the same circuit espusb documents.

- GPIO4 to USB D-
- GPIO5 to USB D+
- Common ground to USB ground
- A pull-up (>= 1k) from D- to 3.3 V

![Basic schematic](basic-schematic.png)

Pins are set in [`src/usb_config.h`](src/usb_config.h). D- must be the
lower-numbered of the two GPIOs.

## Build and flash

One-time: install the pinned toolchain and SDK. This writes only under
`C:\esp\espusb-sdk` and does not touch `PATH`, ESP-IDF, or the RTOS SDK.

```powershell
.\install-sdk.ps1
```

Then:

```powershell
.\build.ps1                      # build only
.\build.ps1 -Flash -Port COM6    # build and flash
.\build.ps1 -Clean               # remove the build/ folder
```

Serial log is 115200 baud. The ROM bootloader's own banner comes out at 74880
baud, so those first lines look like noise at 115200; that is normal.

Keep `-BurnBaud` at its 115200 default. 460800 fails to enter the bootloader on
this board with "Timed out waiting for packet header".

There is no `make`, MSYS2, Cygwin or WSL in this build. The upstream Makefile
compiles and links the whole firmware in a single compiler invocation, so
`build.ps1` just expands the same flags and runs that command, then
`esptool elf2image`, then `write_flash`.

## What to expect

```text
ESPKeyboard ESP8266 receiver (NONOS 1.5.4(baaeaebb))
reset reason=... epc1=...
usb: D-=GPIO4 D+=GPIO5, low speed, boot keyboard
boot ok
usb: host selected configuration 1, address 3
link: starting radio (USB configured)
link: station MAC 68:c6:3a:98:56:e1 channel 1
link: paired transmitter f0:9e:9e:21:9d:98
link: ESP-NOW up, report pump running every 2 ms
```

Device Manager should show one HID Keyboard Device and no "Device Descriptor
Request Failed". Pressing Caps Lock on the host drives a SET_REPORT, which the
firmware logs:

```text
usb: host LEDs=0x02 (num=0 caps=1 scroll=0)
```

That line proves the control endpoint works in both directions, not just that
enumeration finished.

Once the radio is up, the link counters print once a second:

```text
link: rx=94 ok=94 peer=0 fmt=0 dup=84 nocfg=0 gaps=0 resync=0 qfull=0 sent=10 release=0
  report=00 00 00 00 00 00 00 00 last packet 43 ms ago
```

| Counter | Meaning |
| --- | --- |
| `rx` | ESP-NOW frames the callback saw at all |
| `ok` | Accepted: right peer, right length, right protocol version |
| `peer` | Rejected, came from a MAC that is not the paired transmitter |
| `fmt` | Rejected on length or protocol version |
| `dup` | Sequence not newer than the last: the transmitter's 100 ms refresh of an unchanged report |
| `nocfg` | Discarded because no USB host had selected a configuration |
| `gaps` | Sequence numbers skipped, so frames were lost over the air |
| `resync` | Transmitter reboots detected and recovered from |
| `qfull` | Report queue overflowed and the oldest snapshot was dropped |
| `sent` | Reports handed to the USB endpoint |
| `release` | Times all keys were released because the transmitter went silent |

`rx=0` means nothing is arriving: check that both sides use the same channel
and that each has the other's station MAC. A high `dup` is normal and expected,
because the transmitter refreshes the current report every 100 ms whether or not
anything changed, and only genuine state changes are forwarded to the host.

If the host never configures the device, the status timer instead prints the
receive interrupt's own counters once per second while they are still changing:

```text
usb: cfg=0 addr=0 host=0 irqs=27 setup tokens=0 packets=0
  decoder: no-high=0 no-low=0 late-lock=18 bit-timeout=0 crc-fail=9 overflow=0 bad-pid=0 ok=0 last len=10
```

Read it as follows. `irqs` counts interrupts, so a zero there means no packets
reached GPIO5 at all, which is wiring or pull-up. Otherwise the counters say
which stage of reception failed:

| Counter | Meaning |
| --- | --- |
| `no-high`, `no-low` | Woke up outside a packet, or too late to find a falling edge in SYNC |
| `late-lock` | Locked on but decoded nothing: interrupt entry missed the SYNC field |
| `crc-fail` | Decoded a packet whose CRC did not check: sampling phase or clock |
| `bit-timeout` | Bit loop ran past the longest legal packet |
| `bad-pid` | Good CRC but an unrecognised PID |
| `ok` | Packets that passed CRC and reached a handler |

## How the link works

The transmitter sends an eleven-byte packet holding a protocol version, a
sixteen-bit sequence number, and the complete eight-byte boot-keyboard report.
It sends one on every change and refreshes the current state every 100 ms.

Because every packet carries the whole state rather than a delta, a lost packet
costs nothing permanent: the next one re-establishes the truth. That shapes the
receive path:

- The sequence number only detects duplicates, loss and transmitter reboots. A
  reboot restarts the counter at zero, which looks like a long run of stale
  packets, so `SEQUENCE_RESYNC_AFTER` consecutive stale packets are taken as a
  restart and the receiver resynchronises instead of ignoring it forever.
- Reports identical to the last queued one are dropped, so the 100 ms refresh
  does not hand the host a stream of repeats.
- Distinct states are queued rather than overwritten, so a press and release
  that arrive between two pump ticks are both delivered instead of cancelling
  out. If the queue overflows the *oldest* entry is dropped, since the newest
  key state is the one that matters.
- Reports are discarded entirely while no host has selected a configuration.
  Without that, unplugging the USB side during typing would bank a backlog of
  keystrokes and replay them all at once on reconnect.
- If the transmitter goes silent for `TRANSMITTER_TIMEOUT_MS`, all keys are
  released. Otherwise losing the radio mid-keypress would leave that key held
  down on the host indefinitely.

The receive callback and the pump timer both run in task context, never in the
USB interrupt, and on a single core, so the queue needs no locking.

## Why not the RTOS SDK

espusb's README states that SDKs newer than 1.5.x are incompatible. The reason
is interrupt entry latency, and it is measurable.

The decoder latches onto the packet by finding a falling edge inside the eight-bit
SYNC field, and it needs an edge with two SYNC symbols still after it. The last
usable one is at SYNC bit 5, which gives about 3.3 us, or 267 CPU cycles at
80 MHz, from the triggering edge to reaching the sync search.

On the RTOS SDK the GPIO interrupt arrives through FreeRTOS: `_UserExceptionVector`
to `_xt_user_exc` to `_xt_user_entry1` to `_xt_int_enter` to `_xt_context_save`
to `_xt_isr_handler`, which then iterates interrupt numbers 0 through 3 before
dispatching number 4. That is roughly 170 instructions, call it 220 cycles,
before the handler's own prologue runs. Add a flash instruction-cache miss and
it is over budget. The RTOS build's own counters showed it exactly: 27
interrupts producing 18 `late-lock` and 9 `crc-fail`, and zero good packets.

The NONOS SDK dispatches through the ROM with far less preamble, which is the
environment `PHASE_DELAY` in [`src/usb_asm_1bit.S`](src/usb_asm_1bit.S) was
tuned against.

## Notes for anyone changing this

- The bit timing assumes an 80 MHz CPU clock. `user_init()` calls
  `system_update_cpu_freq(80)`; do not remove it.
- The radio is held off until a host configures the device, or
  `RADIO_START_GRACE_MS` elapses, whichever comes first. Wi-Fi interrupts are
  the most likely thing to disturb USB reception, so enumeration gets a clear
  run first. `wifi_set_sleep_type(NONE_SLEEP_T)` and
  `wifi_station_set_auto_connect(0)` matter for the same reason: modem sleep
  and AP scanning both produce long radio bursts, and associating with an AP
  would move the station off the ESP-NOW channel.
- `src/keyboard_link_wire.h` duplicates the wire format from
  `../../common/keyboard_link.h`, which cannot be included here: it pulls in
  `<stdint.h>`, and the SDK's `c_types.h` declares `uint32_t` as
  `unsigned long` where newlib uses `unsigned int`, so the typedefs collide.
  Static assertions pin the struct size and field offsets. Change both files
  together and bump `KEYBOARD_LINK_VERSION`.
- Everything the receive interrupt touches must be in RAM. On this SDK that is
  the default: `sdk/ld/eagle.app.v6.ld` maps `.text` into `iram1_0_seg` and
  `.rodata` into `dram0_0_seg`, so plain functions are in IRAM and
  `static const` descriptors are in DRAM without any attribute. `build.ps1`
  asserts this after every link by checking the addresses of `gpio_intr`,
  `usb_send_data`, `usb_pid_handle_in`, `usb_ramtable` and the `.rodata`
  bounds, and fails the build if any of them moves to flash. Mark anything that
  is *not* on the interrupt path `ICACHE_FLASH_ATTR` to keep it out of IRAM.
- IRAM is the tight resource: `.text` is about 30 kB of the 32 kB
  `iram1_0_seg`, leaving roughly 2.5 kB. `build.ps1` prints the headroom after
  every link and warns under 1 kB. New code that is not interrupt-critical must
  be `ICACHE_FLASH_ATTR`, which is why the ESP-NOW and pump code is in flash.
- `usb_asm_1bit.S` reads `usb_internal_state` at hard-coded offsets from
  `usb.h`. `usb.c` has `_Static_assert`s tying the two together; keep them
  accurate when adding fields.
- NONOS 1.5.4 declares none of the ROM routines its own macros call
  (`ets_isr_attach`, `ets_memcpy`, `ets_timer_*`, `os_printf_plus`), so
  [`src/esp8266_rom.h`](src/esp8266_rom.h) supplies the prototypes. Without
  them the compiler assumes `int` returns and unpromoted arguments.
- `src/user_config.h` exists only because the SDK's `osapi.h` includes it
  unconditionally.
- The internal pull-up on D- is left enabled, matching upstream, even though
  the board also has an external one. That is the configuration this hardware
  is known to enumerate with.

## Still to do

5. Confirm enumeration and sustained typing hold up with the radio active. The
   decoder counters and the link counters above are the instruments: watch
   whether `late-lock` or `crc-fail` start climbing once ESP-NOW is up, and
   whether `gaps` or `qfull` appear during fast typing.

Interop between this SDK's ESP-NOW and the ESP32-S3's is the other thing worth
confirming early. NONOS 1.5.4 implements the older role-based API
(`esp_now_set_self_role`, `esp_now_add_peer(mac, role, channel, key, len)`)
while IDF has no concept of roles, so this side registers as
`ESP_NOW_ROLE_SLAVE` with the transmitter added as `ESP_NOW_ROLE_CONTROLLER`.
If `rx` stays at zero, that pairing is the first thing to question.
