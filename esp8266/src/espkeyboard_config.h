#ifndef _ESPKEYBOARD_CONFIG_H
#define _ESPKEYBOARD_CONFIG_H

// Link settings. There is no menuconfig on this SDK, so edit these and rebuild.

// Must match CONFIG_ESPNOW_CHANNEL on the ESP32-S3 transmitter. Legal 1..13.
// ESP-NOW peers only hear each other on the same channel, and this firmware
// never associates with an access point, so the channel is pinned by hand.
#define ESPNOW_CHANNEL 1

// Station MAC of the paired ESP32-S3, which it prints at boot as
// "Station MAC: ...". Reports from any other sender are ignored.
//
// This is pairing, not authentication: ESP-NOW frames here are unencrypted and
// a MAC address can be spoofed. Use these boards in a trusted RF environment.
#define ESPNOW_PEER_MAC { 0xf0, 0x9e, 0x9e, 0x21, 0x9d, 0x98 }

// Release every key if the transmitter goes quiet for this long. It refreshes
// the current report every 100 ms even when nothing changes, so this is about
// fifteen missed refreshes. Without it, losing the transmitter mid-keypress
// would leave that key held down on the host forever.
#define TRANSMITTER_TIMEOUT_MS 1500

// A transmitter reboot restarts its sequence counter at zero, which looks like
// a run of stale packets. Tolerate this many before concluding it restarted
// and resynchronising, rather than discarding its reports indefinitely.
#define SEQUENCE_RESYNC_AFTER 8

// How often the pump hands a queued report to the USB endpoint. The endpoint's
// bInterval is 10 ms, so the host polls slower than this anyway.
#define REPORT_PUMP_INTERVAL_MS 2

// Give USB a chance to enumerate before the radio is switched on, since Wi-Fi
// interrupts are the most likely thing to disturb the bit-banged USB timing.
// The radio also starts as soon as a host selects a configuration, so this is
// only the upper bound on the wait.
#define RADIO_START_GRACE_MS 3000

#endif
